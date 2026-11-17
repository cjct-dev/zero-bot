#include "ui/app.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <thread>
#include <unistd.h>

#include "agent/agent.h"
#include "agent/llm.h"
#include "core/music_lib.h"
#include "core/settings.h"
#include "core/store.h"
#include "core/tether.h"
#include "core/ui_queue.h"
#include "core/util.h"
#include "lvgl.h"
#include "platform/audio.h"
#include "platform/display.h"
#include "platform/keys.h"
#include "platform/proc.h"
#include "ui/chat_view.h"
#include "ui/menu.h"
#include "ui/settings_pages.h"
#include "ui/theme.h"

namespace ui {

namespace {

constexpr unsigned long kDoubleTapMs = 450;     // two TAB presses this close together open settings
constexpr unsigned long kEscHoldMs = 1200;      // hold ESC this long to leave the app
constexpr unsigned long kDimAfterMs = 45000;    // idle time before the backlight dims
constexpr double kMinSpeechSeconds = 0.4;
constexpr double kMaxSpeechSeconds = 90;

std::atomic<bool> g_exit{false};
void on_signal(int) { g_exit = true; }

struct App {
    ChatView chat;
    Menu menu;
    audio::Recorder recorder;
    unsigned long rec_start = 0;
    bool music_paused_for_mic = false;
    unsigned long esc_down = 0;
    bool esc_armed = false;
    unsigned long last_activity = 0;
    bool dimmed = false;
    unsigned long last_tick = 0, last_second = 0, last_indicators = 0, last_model_try = 0;
    std::atomic<bool> selecting_models{false};
    std::atomic<bool> connecting_phone{false};
    bool warned_models = false;

    // ------------------------------------------------------------ setup
    void wire_agent() {
        AgentEvents ev;
        ev.user_message = [this](const std::string &t, bool steering) {
            uiq::post([this, t, steering] { chat.add_user(t, steering); });
        };
        ev.answer_begin = [this] { uiq::post([this] { chat.begin_answer(); }); };
        ev.answer_delta = [this](const std::string &d) { uiq::post([this, d] { chat.append_answer(d); }); };
        ev.answer_end = [this](const std::string &full, bool interrupted) {
            uiq::post([this, full, interrupted] { chat.end_answer(full, interrupted); });
        };
        ev.answer_discard = [this] { uiq::post([this] { chat.discard_answer(); }); };
        ev.tool = [this](const std::string &label) { uiq::post([this, label] { chat.add_tool(label); }); };
        ev.error = [this](const std::string &m) { uiq::post([this, m] { chat.add_note(m, theme::kListen); }); };
        ev.image = [this](const std::string &jpeg) {
            // Runs on the agent thread: shrink the photo so the screen only decodes a thumbnail.
            // Each thumbnail gets its own name because the image cache is keyed by path.
            static std::atomic<int> serial{0};
            const std::string thumb = util::cache_dir() + "/thumb_" + std::to_string(serial++ % 8) + "_" +
                                      std::to_string(time(nullptr)) + ".png";
            const auto res = proc::run({"ffmpeg", "-v", "error", "-y", "-i", jpeg, "-vf", "scale=128:-2", thumb}, "", 8000);
            if (res.ok()) uiq::post([this, thumb] { chat.add_image(thumb); });
        };
        Agent::get().start(ev);
    }

    // Picks sensible models the first time, so the app works before settings are touched.
    void auto_select_models() {
        Settings &s = Settings::get();
        if (!s.configured() || (!s.stt_model.empty() && !s.tts_model.empty() && !s.eff_llm_model().empty())) return;
        if (selecting_models.exchange(true)) return;
        std::thread([this] {
            std::string err;
            const auto models = llm::list_models(err);
            selecting_models = false;
            if (!err.empty()) return;   // offline, most likely; periodic() tries again later
            uiq::post([this, models] {
                Settings &st = Settings::get();
                for (auto &m : models) {
                    if (st.stt_model.empty() && m.mode == "audio_transcription") st.stt_model = m.id;
                    if (st.tts_model.empty() && m.mode == "audio_speech") st.tts_model = m.id;
                    if (st.eff_llm_model().empty() && m.mode == "chat") st.llm_model = m.id;
                }
                st.save();
                if ((st.stt_model.empty() || st.tts_model.empty()) && !warned_models) {
                    warned_models = true;
                    chat.add_note("Choose speech models in settings (TAB twice) to use voice.", theme::kThink);
                }
            });
        }).detach();
    }

    void welcome() {
        const Settings &s = Settings::get();
        if (!s.configured())
            chat.add_note("No AI service is set up yet. Press TAB twice and enter an address and key under AI service.",
                          theme::kThink);
        else
            chat.add_note("Press TAB to talk, press it again when you have finished. Or press ENTER and type.",
                          theme::kDim);
    }

    // ------------------------------------------------------------ recording
    void start_recording() {
        if (chat.typing()) chat.typing_end();
        Agent::get().interrupt_speech();
        audio::Music &music = audio::Music::get();
        music_paused_for_mic = music.playing() && !music.paused();
        if (music_paused_for_mic) music.pause(true);
        if (!recorder.start()) {
            chat.add_note("The microphone could not be started.", theme::kListen);
            resume_music();
            return;
        }
        rec_start = display::now_ms();
    }

    void resume_music() {
        if (music_paused_for_mic) audio::Music::get().pause(false);
        music_paused_for_mic = false;
    }

    void finish_recording(bool submit) {
        const double seconds = recorder.seconds();
        std::vector<int16_t> pcm = recorder.stop();
        resume_music();
        if (!submit) return;
        if (seconds < kMinSpeechSeconds) { chat.add_note("That was too short to hear. Press TAB, speak, then press TAB again.", theme::kDim); return; }
        audio::normalize(pcm);
        auto shared = std::make_shared<std::vector<int16_t>>(std::move(pcm));
        when_online([shared] { Agent::get().submit_audio(std::move(*shared)); });
    }

    // Runs `send` straight away when there is a connection. With none, and a paired phone, it
    // first tries the phone's Bluetooth tethering, then sends whether or not that worked (the
    // request then reports the missing connection itself).
    void when_online(std::function<void()> send) {
        if (tether::online() || !Settings::get().phone_fallback || connecting_phone) { send(); return; }
        connecting_phone = true;
        std::thread([this, send] {
            if (!tether::available()) { connecting_phone = false; uiq::post(send); return; }
            uiq::post([this] { chat.add_note("No connection. Trying your phone over Bluetooth…", theme::kDim); });
            const std::string err = tether::connect();
            connecting_phone = false;
            uiq::post([this, err, send] {
                if (err.empty()) chat.add_note("Online through your phone.", theme::kDim);
                last_indicators = 0;
                send();
            });
        }).detach();
    }

    // The first-run guide. When it ends the chat starts fresh with a spoken hello, which also
    // confirms that the service and the speaker both work.
    void run_setup() {
        open_setup(menu, [this] {
            chat.clear();
            welcome();
            auto_select_models();
            const Settings &s = Settings::get();
            if (s.configured() && s.speak_replies && !s.tts_model.empty() && tether::online())
                Agent::get().say("Hello" + (s.user_name.empty() ? std::string() : " " + s.user_name) +
                                 ". I'm ready. Press tab to talk to me.");
        });
    }

    // ------------------------------------------------------------ keys
    void open_menu() {
        SettingsHooks hooks;
        hooks.new_conversation = [this] {
            Agent::get().reset_conversation();
            chat.clear();
            welcome();
        };
        hooks.exit_app = [] { g_exit = true; };
        hooks.run_setup = [this] { menu.close(); run_setup(); };
        open_settings(menu, hooks);
    }

    void handle_key(const KeyEvent &ev) {
        const unsigned long now = display::now_ms();
        if (ev.state != 0) {
            last_activity = now;
            if (dimmed) { display::set_brightness(Settings::get().brightness); dimmed = false; }
        }

        // ESC: a short press backs out of whatever is happening, a long hold leaves the app.
        if (ev.key == Key::Esc) {
            if (ev.state == 1) { esc_down = now; esc_armed = true; }
            if (ev.state == 0 && esc_armed) {
                esc_armed = false;
                if (now - esc_down < kEscHoldMs) short_escape();
            }
            return;
        }

        if (menu.open()) {
            if (ev.key == Key::Tab) { if (ev.state == 1) menu.close(); return; }
            menu.key(ev);
            return;
        }

        if (ev.key == Key::Tab) {
            if (ev.state != 1) return;
            if (recorder.active()) {
                if (now - rec_start < kDoubleTapMs) { finish_recording(false); open_menu(); }
                else finish_recording(true);
            } else {
                start_recording();
            }
            return;
        }
        if (recorder.active() || ev.state == 0) return;

        if (chat.typing()) {
            if (ev.key == Key::Enter) {
                if (ev.state != 1) return;
                const std::string text = util::trim(chat.typing_text());
                chat.typing_end();
                if (!text.empty()) when_online([text] { Agent::get().submit_text(text); });
            } else if (ev.key == Key::Up || ev.key == Key::Down) {
                chat.scroll(ev.key == Key::Up ? 34 : -34);
            } else {
                chat.typing_key(ev);
            }
            return;
        }
        switch (ev.key) {
            case Key::Enter: if (ev.state == 1) chat.typing_begin(""); break;
            case Key::Char: if (ev.text != " ") chat.typing_begin(ev.text); break;
            case Key::Up: chat.scroll(34); break;
            case Key::Down: chat.scroll(-34); break;
            case Key::PgUp: chat.scroll(110); break;
            case Key::PgDn: chat.scroll(-110); break;
            default: break;
        }
    }

    void short_escape() {
        if (menu.open()) { KeyEvent ev; ev.key = Key::Esc; ev.state = 1; menu.key(ev); return; }
        if (recorder.active()) { finish_recording(false); return; }
        if (chat.typing()) { chat.typing_end(); return; }
        Agent &agent = Agent::get();
        if (agent.busy() || agent.state() != AgentState::Idle) { agent.cancel(); return; }
        if (audio::Music::get().playing()) audio::Music::get().stop();
    }

    // ------------------------------------------------------------ periodic work
    void update_footer() {
        Agent &agent = Agent::get();
        const Settings &s = Settings::get();
        if (recorder.active()) {
            chat.footer_listening(recorder.level(), recorder.seconds(), recorder.has_audio());
            chat.set_status(s.agent_name, theme::kListen);
            if (recorder.seconds() > kMaxSpeechSeconds) finish_recording(true);
            return;
        }
        switch (agent.state()) {
            case AgentState::Transcribing: chat.footer_busy("Transcribing", theme::kThink); chat.set_status(s.agent_name, theme::kThink); break;
            case AgentState::Thinking: chat.footer_busy("Thinking", theme::kThink); chat.set_status(s.agent_name, theme::kThink); break;
            case AgentState::Working: {
                const std::string detail = agent.detail();
                chat.footer_busy(detail.empty() ? "Working" : detail, theme::kThink);
                chat.set_status(s.agent_name, theme::kThink);
                break;
            }
            case AgentState::Speaking:
                chat.footer_busy(s.speak_replies ? "Speaking   ·   TAB to interrupt" : "Answering", theme::kAccent);
                chat.set_status(s.agent_name, theme::kAccent);
                break;
            case AgentState::Idle:
                chat.footer_hints(s.configured() ? "TAB talk   ·   ENTER type   ·   TAB TAB settings"
                                                 : "TAB TAB settings   ·   hold ESC to exit");
                chat.set_status(s.agent_name, theme::kFaint);
                break;
        }
    }

    void update_indicators() {
        const std::string link = tether::default_link();
        bool online = !link.empty(), charging = false;
        const bool phone = link.compare(0, 4, "bnep") == 0;
        int battery = -1;
#if !ZB_DESKTOP
        std::ifstream b("/sys/class/power_supply/bq27220-0/capacity");
        if (!(b >> battery)) battery = -1;
        std::string status;
        std::ifstream("/sys/class/power_supply/bq27220-0/status") >> status;
        charging = status == "Charging";
#endif
        chat.set_indicators(online, phone, battery, charging, util::format_time(time(nullptr), "%H:%M"));
        const std::string current = audio::Music::get().current();
        if (current.empty()) chat.set_now_playing("");
        else {
            const Track t = MusicLibrary::get().lookup(current);
            chat.set_now_playing(t.artist.empty() ? t.title : t.title + " · " + t.artist);
        }
    }

    void check_alerts() {
        for (auto &text : Store::get().take_due_alerts(time(nullptr))) {
            chat.add_alert(text);
            audio::chime(2);
            if (Settings::get().speak_replies) Agent::get().say("Reminder. " + text);
            last_activity = display::now_ms();
            if (dimmed) { display::set_brightness(Settings::get().brightness); dimmed = false; }
        }
    }

    void periodic() {
        const unsigned long now = display::now_ms();
        if (esc_armed && now - esc_down >= kEscHoldMs) { g_exit = true; return; }
        if (now - last_tick >= 66) {
            last_tick = now;
            update_footer();
            chat.tick();
            menu.tick();
        }
        if (now - last_second >= 1000) {
            last_second = now;
            check_alerts();
            const bool active = recorder.active() || Agent::get().state() != AgentState::Idle;
            if (active) last_activity = now;
            if (!dimmed && now - last_activity > kDimAfterMs) { display::set_brightness(10); dimmed = true; }
        }
        if (now - last_model_try >= 15000) {   // covers starting up before Wi-Fi has connected
            last_model_try = now;
            auto_select_models();
        }
        if (now - last_indicators >= 5000 || last_indicators == 0) {
            last_indicators = now ? now : 1;
            update_indicators();
        }
    }

    // ------------------------------------------------------------ screenshots
    int render_shots(const std::string &dir) {
        util::make_dirs(dir);
        int n = 0;
        auto snap = [&](const char *name) {
            for (int i = 0; i < 6; ++i) { chat.tick(); menu.tick(); }
            lv_refr_now(nullptr);
            char path[512];
            snprintf(path, sizeof(path), "%s/%02d-%s.png", dir.c_str(), ++n, name);
            if (!display::save_png(path)) fprintf(stderr, "could not write %s\n", path);
            else printf("%s\n", path);
        };
        Settings &s = Settings::get();
        s.agent_name = "Zero";
        chat.set_status(s.agent_name, theme::kFaint);
        chat.set_indicators(true, false, 68, false, "18:42");
        chat.footer_hints("TAB talk   ·   ENTER type   ·   TAB TAB settings");
        chat.add_note("Press TAB to talk, press it again when you have finished. Or press ENTER and type.", theme::kDim);
        snap("welcome");

        chat.clear();
        chat.add_user("What's the weather like in Lisbon tomorrow?", false);
        chat.add_tool("Checking the weather");
        chat.begin_answer();
        chat.append_answer("Tomorrow in Lisbon looks wet: rain most of the day, 18 to 21 degrees, with a 70 percent chance of showers. Take an umbrella.");
        chat.end_answer("", false);
        snap("answer");

        chat.add_user("How much is 85 euros in pounds, split three ways?", false);
        chat.add_tool("Converting currency");
        chat.add_tool("Calculating");
        chat.footer_busy("Calculating", theme::kThink);
        chat.set_status(s.agent_name, theme::kThink);
        snap("working");

        chat.begin_answer();
        chat.append_answer("85 euros is about 70.93 pounds, so that's 23.64 each.");
        chat.end_answer("", false);
        chat.footer_busy("Speaking   ·   TAB to interrupt", theme::kAccent);
        chat.set_status(s.agent_name, theme::kAccent);
        chat.set_now_playing("Dancing Queen · ABBA");
        snap("speaking");

        for (int i = 0; i < 40; ++i) {
            const float lvl = 0.15f + 0.75f * static_cast<float>((i * 37 % 17)) / 17.0f * (i % 9 < 6 ? 1.f : 0.25f);
            chat.footer_listening(lvl, 3.4, true);
        }
        chat.set_status(s.agent_name, theme::kListen);
        snap("listening");

        chat.footer_hints("");
        chat.typing_begin("remind me at 7 to call the hot");
        chat.set_status(s.agent_name, theme::kFaint);
        snap("typing");
        chat.typing_end();

        chat.add_alert("Check the oven");
        chat.add_user("こんにちは、元気ですか？", false);
        chat.footer_hints("TAB talk   ·   ENTER type   ·   TAB TAB settings");
        snap("alert");

        Store &store = Store::get();
        CalEvent e;
        auto day_at = [](int days_ahead, const char *clock) {
            time_t t = 0;
            util::parse_local(util::format_time(time(nullptr) + days_ahead * 86400, "%Y-%m-%d ") + clock, t);
            return t;
        };
        e.title = "Dinner with Marta"; e.start = day_at(3, "19:30"); e.remind_min = 30;
        store.add_event(e);
        e.title = "Flight to Lisbon"; e.start = day_at(5, "07:15"); e.notes = "Seat 14A"; e.remind_min = 120;
        const int flight = store.add_event(e);
        store.add_memory("Allergic to shellfish");
        store.add_memory("Prefers window seats");
        store.add_reminder("Call the hotel", time(nullptr) + 3600);

        run_setup();
        snap("setup-welcome");
        {
            KeyEvent enter; enter.key = Key::Enter; enter.state = 1;
            KeyEvent down; down.key = Key::Down; down.state = 1;
            menu.key(enter);
            snap("setup-internet");
            for (int i = 0; i < 4; ++i) menu.key(down);
            menu.key(enter);                       // continue (with or without a connection)
            snap("setup-service");
            for (int i = 0; i < 4; ++i) menu.key(down);
            menu.key(enter);                       // skip the live test in screenshots
            s.llm_model = "example-chat-model"; s.stt_model = "example-speech-to-text"; s.tts_model = "example-text-to-speech";
            menu.refresh();
            snap("setup-models");
            for (int i = 0; i < 4; ++i) menu.key(down);
            menu.key(enter);
            s.user_name = "Sam"; s.home_city = "London";
            menu.refresh();
            snap("setup-you");
            for (int i = 0; i < 5; ++i) menu.key(down);
            menu.key(enter);
            snap("setup-extras");
            menu.close();
        }

        open_menu();
        snap("settings");
        KeyEvent down; down.key = Key::Down; down.state = 1;
        KeyEvent enter; enter.key = Key::Enter; enter.state = 1;
        KeyEvent esc; esc.key = Key::Esc; esc.state = 1;
        for (int i = 0; i < 8; ++i) menu.key(down);    // first row of "Device"
        snap("settings-device");
        for (int i = 0; i < 7; ++i) menu.key(down);    // "Calendar", first row of "Your data"
        snap("settings-data");
        menu.key(enter);
        snap("calendar");
        menu.key(enter);
        snap("event");
        menu.key(enter);
        snap("edit-text");
        menu.key(esc); menu.key(esc); menu.key(esc);
        menu.key(down); menu.key(down);                // "Memory"
        menu.key(enter);
        snap("memory");
        menu.key(esc);
        for (int i = 0; i < 12; ++i) menu.key(down);
        snap("settings-service");
        (void)flight;
        menu.close();
        return 0;
    }
};

}  // namespace

int run_app(const std::string &shots_dir, bool keydump) {
    const bool shots = !shots_dir.empty();
#if ZB_DESKTOP
    display::init(shots ? display::Mode::Headless : display::Mode::Window);
#else
    display::init(shots ? display::Mode::Headless : display::Mode::Device);
#endif
    theme::init();
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, theme::color(theme::kBg), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    static App app;
    app.chat.create(screen);
    app.menu.create(screen);
    if (shots) return app.render_shots(shots_dir);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    const Settings &s = Settings::get();
    display::set_brightness(s.brightness);
    audio::set_mic_gain(s.mic_gain);
    const int volume = s.volume;
    std::thread([volume] { audio::set_volume(volume); }).detach();

    app.wire_agent();
    app.welcome();
    app.auto_select_models();
    if (!s.setup_done) app.run_setup();
    input::set_dump(keydump);
    input::start();
    app.last_activity = display::now_ms();

    while (!g_exit) {
        KeyEvent ev;
        while (input::poll(ev)) app.handle_key(ev);
        uiq::drain();
        app.periodic();
        const uint32_t wait = lv_timer_handler();
        usleep((wait > 16 ? 16 : (wait < 2 ? 2 : wait)) * 1000);
    }

    app.recorder.cancel();
    Agent::get().cancel();
    audio::Music::get().stop();
    display::set_brightness(s.brightness);
    // Leave a clean screen for the launcher to draw over.
    lv_obj_clean(screen);
    lv_refr_now(nullptr);
    return 0;
}

}  // namespace ui
