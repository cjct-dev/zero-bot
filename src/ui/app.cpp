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
#include "agent/speech.h"
#include "ui/alarm_view.h"
#include "ui/breath_view.h"
#include "agent/llm.h"
#include "core/music_lib.h"
#include "core/radio.h"
#include "core/settings.h"
#include "core/store.h"
#include "core/tether.h"
#include "core/timers.h"
#include "core/ui_queue.h"
#include "core/util.h"
#include "lvgl.h"
#include "platform/audio.h"
#include "platform/display.h"
#include "platform/keys.h"
#include "platform/proc.h"
#include "smarthome/home.h"
#include "ui/chat_view.h"
#include "ui/key_guide.h"
#include "ui/menu.h"
#include "ui/settings_pages.h"
#include "ui/theme.h"

namespace ui {

namespace {

// A made-up home for the screenshots, saved on the stand-in test network: none of its addresses
// is ever contacted, and nothing is scanned.
void sample_home() {
    setenv("ZB_TEST_NETWORK", "1", 1);
    const discovery::Network net = home::network_now(true);
    auto plug = [](const char *id, const char *name, const char *room, bool on, bool ask) {
        return json{{"id", id}, {"name", name}, {"room", room}, {"type", "plug"}, {"caps", home::kPower}, {"ask_first", ask},
                    {"ref", {{"child", std::string(id) == "main" ? "" : id}}}, {"state", {{"on", on}}}};
    };
    const json plug_info = {{"scheme", "KLAP"}, {"device_type", "SMART.TAPOPLUG"}};
    json devices = json::array();
    devices.push_back({{"id", "tapo:sample-1"}, {"kind", "tapo"}, {"name", "Bed heater"}, {"model", "P110"}, {"last_ip", "127.0.0.2"},
                       {"port", 80}, {"info", plug_info}, {"things", {plug("main", "Bed heater", "Bedroom", false, true)}}});
    devices.push_back({{"id", "tapo:sample-2"}, {"kind", "tapo"}, {"name", "Office strip"}, {"model", "P304M"},
                       {"last_ip", "127.0.0.3"}, {"port", 80}, {"info", plug_info},
                       {"things", {plug("s1", "Desk lamp", "Office", true, false), plug("s2", "Monitor", "Office", false, false),
                                   plug("s3", "Fan", "", false, false), plug("s4", "Kettle", "", false, true)}}});
    json other_owner = plug_info;
    other_owner["owner"] = "00112233445566778899AABBCCDDEEFF";
    devices.push_back({{"id", "tapo:sample-3"}, {"kind", "tapo"}, {"name", "Tapo P110"}, {"model", "P110"}, {"last_ip", "127.0.0.4"},
                       {"port", 80}, {"info", other_owner}, {"needs_setup", true}, {"things", json::array()}});
    // A strip whose last answer listed no sockets: still on the page, under "To set up".
    json pump = plug("s1", "Pond pump", "Garden", false, false);
    pump["gone"] = 1;
    devices.push_back({{"id", "tapo:sample-4"}, {"kind", "tapo"}, {"name", "Garden strip"}, {"model", "P300"}, {"last_ip", "127.0.0.7"},
                       {"port", 80}, {"info", plug_info}, {"things", json::array({pump})}});
    devices.push_back({{"id", "hue:sample"}, {"kind", "hue_bridge"}, {"name", "Hue Bridge"}, {"model", "BSB002"}, {"last_ip", "127.0.0.5"}});
    devices.push_back({{"id", "cast:sample"}, {"kind", "cast"}, {"name", "Living room TV"}, {"model", "Chromecast"}, {"last_ip", "127.0.0.6"}});
    const json root = {{"version", 2},
                       {"secrets", {{"tapo", {{"email", "sam@example.com"}, {"password_sha1", std::string(40, '0')}}}}},
                       {"networks", {{net.id(), {{"label", net.label()}, {"gateway_mac", net.gateway_mac}, {"allowed_at", 1},
                                                 {"last_scan", 1}, {"devices", devices}}}}}};
    util::save_json(util::data_dir() + "/smarthome.json", root);
}

constexpr unsigned long kDoubleTapMs = 450;     // two presses this close together count as a double tap
constexpr unsigned long kEscHoldMs = 1200;      // hold ESC this long to leave the app
constexpr unsigned long kDimAfterMs = 45000;    // idle time before the backlight dims
constexpr double kMinSpeechSeconds = 0.4;
constexpr double kMaxSpeechSeconds = 90;

std::atomic<bool> g_exit{false};
void on_signal(int) { g_exit = true; }

struct App {
    ChatView chat;
    Menu menu;
    BreathView breath;
    AlarmView banner;                 // over everything else while a timer's alarm rings
    // Rings when a timer ends. Its sound gives way, for a few seconds at most, while the assistant's
    // voice is coming out of the speaker, so the timer's name can be heard. The alarm ends the ring
    // itself after a minute; the sound's own limit is only a backstop a little beyond that.
    timers::Alarm alarm{timers::Alarm::Sound{
        [] {
            audio::AlarmSound::get().start([] { return SpeechQueue::get().playing(); },
                                           static_cast<int>(timers::kRingMs) + 5000);
        },
        [] { audio::AlarmSound::get().stop(); }}};
    // The status bar's countdown is worked out from these ends, read again only when timers change.
    int timer_revision = -1;
    std::vector<time_t> timer_ends;
    // The key press that silenced an alarm: its repeats and its release do nothing more.
    Key spent_key = Key::None;
    std::string spent_text;
    // A breathing exercise the assistant has asked for, started once its spoken reply has finished.
    std::string breath_pattern;
    int breath_minutes = 0;
    audio::Recorder recorder;
    unsigned long rec_start = 0;
    unsigned long last_delete = 0;   // for the double tap that starts a new conversation
    // Volume and brightness keys: what the footer shows for a moment, and settings waiting to be
    // written once the key has been let go (so a held key does not rewrite the file ten times a second).
    std::string level_text;
    unsigned long level_until = 0, settings_dirty_at = 0;
    bool muted = false;
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
        ev.breathe = [this](const std::string &pattern, int minutes) {
            uiq::post([this, pattern, minutes] { breath_pattern = pattern; breath_minutes = minutes; });
        };
        ev.image = [this](const std::string &jpeg) {
            // Called on the agent thread as the photo is taken: shrink it so the screen only
            // decodes a thumbnail. Starting ffmpeg takes one to three seconds on the device, so it
            // runs beside the request instead of holding the photo back from the service, on a
            // copy, since the next photo replaces this one. Each thumbnail gets its own name
            // because the image cache is keyed by path.
            static std::atomic<int> serial{0};
            const std::string base = util::cache_dir() + "/thumb_" + std::to_string(serial++ % 8) + "_" + std::to_string(time(nullptr));
            std::string data;
            if (!util::read_file(jpeg, data) || !util::write_file_atomic(base + ".jpg", data)) return;
            std::thread([this, base] {
                const std::string thumb = base + ".png";
                const auto res = proc::run({"ffmpeg", "-v", "error", "-y", "-i", base + ".jpg", "-vf", "scale=128:-2", thumb}, "", 8000);
                unlink((base + ".jpg").c_str());
                if (res.ok()) uiq::post([this, thumb] { chat.add_image(thumb); });
            }).detach();
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
                    chat.add_note("Choose speech models in settings (SPACE) to use voice.", theme::kThink);
                }
            });
        }).detach();
    }

    void welcome() {
        const Settings &s = Settings::get();
        if (!s.configured())
            chat.add_note("No AI service is set up yet. Press SPACE for settings and enter an address and key under AI service.",
                          theme::kThink);
        else
            chat.add_note("Press TAB to talk, press it again when you have finished. Or press ENTER and type.",
                          theme::kDim);
    }

    // ------------------------------------------------------------ recording
    void start_recording() {
        if (alarm.ringing()) { alarm.stop(); sync_banner(); }   // an alarm never rings into the microphone
        if (chat.typing()) chat.typing_end();
        Agent::get().interrupt_speech();
        audio::Music::get().hold(audio::Music::kMic, true);
        Agent::get().set_listening(true);   // nothing new is spoken while the microphone is open
        if (!recorder.start()) {
            chat.add_note("The microphone could not be started.", theme::kListen);
            audio::Music::get().hold(audio::Music::kMic, false);
            Agent::get().set_listening(false);
            return;
        }
        rec_start = display::now_ms();
    }

    void finish_recording(bool submit) {
        const double seconds = recorder.seconds();
        std::vector<int16_t> pcm = recorder.stop();
        audio::Music &music = audio::Music::get();
        const bool sending = submit && seconds >= kMinSpeechSeconds;
        // Music that was playing stays quiet from the question to the end of the spoken answer,
        // rather than coming back for the few seconds in between.
        if (sending) music.hold(audio::Music::kExchange, true);
        music.hold(audio::Music::kMic, false);
        if (!sending) {
            Agent::get().set_listening(false);
            if (submit) chat.add_note("That was too short to hear. Press TAB, speak, then press TAB again.", theme::kDim);
            return;
        }
        audio::normalize(pcm);
        auto shared = std::make_shared<std::vector<int16_t>>(std::move(pcm));
        when_online([shared] { Agent::get().submit_audio(std::move(*shared)); });
        // Cleared only after the recording has been handed over, so a request that was waiting for
        // the user to finish sees what they said and treats it as steering.
        Agent::get().set_listening(false);
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
        hooks.new_conversation = [this] { new_conversation(); };
        hooks.exit_app = [] { g_exit = true; };
        hooks.run_setup = [this] { menu.close(); run_setup(); };
        open_settings(menu, hooks);
    }

    // Forgets what has been said so far. Calendar, reminders and saved memories are untouched.
    void new_conversation() {
        Agent::get().reset_conversation();
        chat.clear();
        chat.add_note("New conversation. What was said before is forgotten.", theme::kDim);
        audio::chime(1);
    }

    // Applies the newest volume off the UI thread; the mixer command takes a moment, and a held
    // key asks for many steps in a row.
    void apply_volume(int percent) { audio::set_volume_async(percent); }

    static std::string level_bar(const char *label, int percent) {
        std::string bar;
        for (int i = 0; i < 10; ++i) bar += i < (percent + 5) / 10 ? "\xE2\x96\xAE" : "\xE2\x96\xAF";   // ▮ ▯
        return std::string(label) + "  " + bar + "  " + std::to_string(percent) + "%";
    }

    void show_level(const std::string &text) {
        level_text = text;
        level_until = display::now_ms() + 1500;
        if (menu.open()) { menu.toast(text); menu.refresh(); }
    }

    // The fn-layer control keys work everywhere: while talking, typing, in settings.
    bool control_key(const KeyEvent &ev) {
        Settings &s = Settings::get();
        audio::Music &music = audio::Music::get();
        switch (ev.key) {
            case Key::VolumeUp:
            case Key::VolumeDown: {
                if (ev.state == 0) return true;
                const int step = ev.key == Key::VolumeUp ? 5 : -5;
                s.volume = std::max(0, std::min(100, (s.volume + (step > 0 ? 0 : 4)) / 5 * 5 + step));
                muted = false;
                apply_volume(s.volume);
                settings_dirty_at = display::now_ms();
                show_level(level_bar("Volume", s.volume));
                return true;
            }
            case Key::Mute:
                if (ev.state != 1) return true;
                muted = !muted;
                apply_volume(muted ? 0 : s.volume);
                show_level(muted ? "Muted   ·   fn+A to unmute" : level_bar("Volume", s.volume));
                return true;
            case Key::BrightnessUp:
            case Key::BrightnessDown: {
                if (ev.state == 0) return true;
                const int step = ev.key == Key::BrightnessUp ? 10 : -10;
                s.brightness = std::max(10, std::min(100, (s.brightness + (step > 0 ? 0 : 9)) / 10 * 10 + step));
                display::set_brightness(s.brightness);
                settings_dirty_at = display::now_ms();
                show_level(level_bar("Brightness", s.brightness));
                return true;
            }
            case Key::MediaPlay:
                if (ev.state != 1) return true;
                if (!music.playing()) show_level("No music playing. Ask for some.");
                else if (music.live()) { music.pause(!music.paused()); show_level(music.paused() ? "Radio paused" : "Radio on"); }
                else { music.pause(!music.paused()); show_level(music.paused() ? "Music paused" : "Music playing"); }
                return true;
            case Key::MediaNext:
            case Key::MediaPrevious:
                if (ev.state != 1) return true;
                if (!music.playing()) { show_level("No music playing. Ask for some."); return true; }
                if (music.live()) { change_station(ev.key == Key::MediaNext ? 1 : -1); return true; }
                if (music.paused()) music.pause(false);
                if (ev.key == Key::MediaNext) music.next(); else music.previous();
                show_level(ev.key == Key::MediaNext ? "Next track" : "Previous track");
                return true;
            default:
                return false;
        }
    }

    // fn+E and fn+W while a station plays: the next or previous station in the user's list. No
    // check of the stream here (that would wait on the network on this thread); a station that
    // will not play is given up by the player, and update_indicators() reports it.
    void change_station(int step) {
        const std::vector<radio::Station> list = radio::stations();
        audio::Music &music = audio::Music::get();
        if (list.size() < 2) { show_level(list.empty() ? "No radio stations set up" : "No other station"); return; }
        const int at = radio::find(list, music.title());
        const int n = static_cast<int>(list.size());
        const radio::Station &next = list[static_cast<size_t>(((at < 0 ? 0 : at + step) % n + n) % n)];
        music.play_live(radio::stream_for(next, radio::metered_link()), next.name);
        show_level(next.name);
        last_indicators = 0;   // the status bar shows the new name at the next tick
    }

    void handle_key(const KeyEvent &ev) {
        const unsigned long now = display::now_ms();
        if (ev.state != 0) {
            last_activity = now;
            if (dimmed) { display::set_brightness(Settings::get().brightness); dimmed = false; }
        }

        // A ringing alarm takes the next key press, whatever the key and whatever is on screen (the
        // chat, a menu, the breathing exercise), and the press goes no further. TAB also starts
        // listening, so that "give it five more minutes" is one press away.
        if (ev.state == 1) spent_key = Key::None;
        else if (spent_key != Key::None && ev.key == spent_key && ev.text == spent_text) {
            if (ev.state == 0) spent_key = Key::None;
            return;   // the repeats and the release of the press that silenced it
        }
        if (ev.state == 1 && alarm.ringing()) {
            const bool tab = ev.key == Key::Tab;
            alarm.key(tab);
            sync_banner();
            spent_key = ev.key;
            spent_text = ev.text;
            if (tab) {
                if (breath.active()) breath.stop();
                if (menu.open()) menu.close();
                if (!recorder.active()) start_recording();
            }
            return;
        }

        if (control_key(ev)) return;

        // A breathing exercise takes the whole screen. ESC ends it; TAB ends it and starts listening,
        // since wanting to say something is a fair reason to stop. Other keys are left alone so a
        // stray press does not break the calm.
        if (breath.active()) {
            if (ev.state != 1) return;
            if (ev.key == Key::Esc) { breath.stop(); esc_armed = false; return; }
            if (ev.key != Key::Tab) return;
            breath.stop();
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
            // Outside a text field, SPACE closes settings as it opened them, and H shows the keys.
            if (!menu.editing() && keyguide::is_settings_key(ev)) { menu.close(); return; }
            if (!menu.editing() && keyguide::is_guide_key(ev)) { open_key_guide(menu, true); return; }
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
        using keyguide::ChatKey;
        switch (keyguide::chat_key(ev)) {
            // Two taps of the delete key (not while typing, where it deletes text) start a new conversation.
            case ChatKey::DeleteTap:
                if (last_delete && now - last_delete < kDoubleTapMs) { last_delete = 0; new_conversation(); }
                else last_delete = now;
                break;
            case ChatKey::Type: chat.typing_begin(""); break;
            case ChatKey::Settings: open_menu(); break;            // SPACE; TAB twice also works
            case ChatKey::Guide: open_key_guide(menu, false); break;
            case ChatKey::ScrollUp: chat.scroll(34); break;
            case ChatKey::ScrollDown: chat.scroll(-34); break;
            case ChatKey::PageUp: chat.scroll(110); break;
            case ChatKey::PageDown: chat.scroll(-110); break;
            case ChatKey::TypeHint: show_level("Press ENTER to type"); break;
            case ChatKey::None: break;
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
        if (level_until && display::now_ms() < level_until && !recorder.active() && !chat.typing()) {
            chat.footer_hints(level_text);
            return;
        }
        level_until = 0;
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
                chat.footer_hints(keyguide::idle_hint(s.configured()));
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
        audio::Music &music = audio::Music::get();
        const std::string current = music.current();
        // A live station has a name and no track; one that has ended by itself says why, once.
        const std::string ended = music.take_problem();
        if (!ended.empty()) {
            chat.add_note(ended, theme::kListen);
            Agent::get().add_context(ended);
        }
        if (music.live()) chat.set_now_playing(music.title());
        else if (current.empty()) chat.set_now_playing("");
        else {
            const Track t = MusicLibrary::get().lookup(current);
            chat.set_now_playing(t.artist.empty() ? t.title : t.title + " · " + t.artist);
        }
    }

    // ------------------------------------------------------------ timers
    // Runs on every tick. Cheap unless a timer has just ended or the countdown's text has changed:
    // the store is only asked when its next end has come or its timers have changed.
    void update_timers(unsigned long now_ms) {
        const time_t now = timers::follow_clock();     // a clock set meanwhile moves the timers with it
        const std::vector<timers::Ended> ended = alarm.poll(now, now_ms, recorder.active(), muted);
        if (!ended.empty()) timers_ended(ended);
        alarm.tick(now_ms);
        sync_banner();
        if (alarm.ringing()) last_activity = now_ms;   // the screen stays lit while it rings
        // Under a menu or the breathing exercise the status bar cannot be seen; changing it would
        // only make the screen redraw what is hidden. It catches up when they close.
        if (menu.open() || breath.active()) return;
        const int revision = Store::get().timers_revision();
        if (revision != timer_revision) {
            timer_revision = revision;
            timer_ends.clear();
            for (const Reminder &t : Store::get().timers()) timer_ends.push_back(t.due);
        }
        chat.set_timer(timers::badge(timer_ends, now));
    }

    void timers_ended(const std::vector<timers::Ended> &ended) {
        std::string names;
        for (const timers::Ended &e : ended) {
            const bool today = util::format_time(e.end, "%Y-%m-%d") == util::format_time(time(nullptr), "%Y-%m-%d");
            const std::string at = today ? util::format_time(e.end, "%H:%M") : util::short_date(e.end);
            if (e.missed) {   // it ended while the app was closed
                chat.add_alert("Missed timer: " + e.label + " (ended " + at + ")");
                continue;
            }
            chat.add_alert("Time's up: " + e.label);
            names += (names.empty() ? "" : ", ") + e.label;
            // What the user says next may be about it: "give it five more minutes".
            Agent::get().add_context("The timer \"" + e.label + "\" ended at " + at + " and its alarm rang.");
        }
        last_activity = display::now_ms();
        if (dimmed) { display::set_brightness(Settings::get().brightness); dimmed = false; }
        if (!names.empty() && Settings::get().speak_replies && !muted) Agent::get().say("Time's up: " + names + ".");
    }

    void sync_banner() {
        if (!alarm.ringing()) { banner.hide(); return; }
        const std::vector<std::string> &labels = alarm.labels();
        std::string names;
        for (const std::string &l : labels) names += (names.empty() ? "" : ", ") + l;
        banner.show(labels.size() == 1 ? "Time's up" : std::to_string(labels.size()) + " timers are up", names);
    }

    void check_alerts() {
        if (recorder.active()) return;   // a reminder waits rather than talking over the user
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
            if (breath.active()) {
                last_activity = now;                       // the screen stays lit for the whole exercise
                if (breath.tick(now)) audio::chime(1);     // a soft note when the last breath is done
            }
            update_timers(now);
        }
        // Wait for the assistant to finish its sentence before the orb takes over the screen.
        if (breath_minutes > 0 && !recorder.active() && Agent::get().state() == AgentState::Idle && !Agent::get().busy() &&
            !Agent::get().has_pending() && !SpeechQueue::get().active()) {
            if (menu.open()) menu.close();
            if (chat.typing()) chat.typing_end();
            if (dimmed) { display::set_brightness(Settings::get().brightness); dimmed = false; }
            breath.start(breath_pattern, breath_minutes);
            breath_minutes = 0;
            if (banner.visible()) banner.raise();         // the exercise came forward; an alarm stays in front
        }
        if (settings_dirty_at && now - settings_dirty_at > 1500) {
            settings_dirty_at = 0;
            Settings::get().save();
        }
        if (now - last_second >= 1000) {
            last_second = now;
            check_alerts();
            const bool active = recorder.active() || Agent::get().state() != AgentState::Idle;
            if (active) last_activity = now;
            // Belt and braces: whatever happened to a request, music is never left waiting for
            // an answer that is not coming.
            if (!active && !Agent::get().busy() && !Agent::get().has_pending() && !SpeechQueue::get().active() && !connecting_phone)
                audio::Music::get().hold(audio::Music::kExchange | audio::Music::kSpeech, false);
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
        // The status bar shows the name it is given from the first call on, an empty one included.
        chat.set_status("", theme::kFaint);
        bool failed = !chat.status_name().empty();
        if (failed) fprintf(stderr, "the status bar kept a name it was not given\n");
        Settings &s = Settings::get();
        s.agent_name = "Zero";
        chat.set_status(s.agent_name, theme::kFaint);
        chat.set_indicators(true, false, 68, false, "18:42");
        chat.footer_hints(keyguide::idle_hint(true));
        // A bubble once kept the height of one line while its text wrapped onto a second, so the
        // last word vanished ("...cybersecurity" for "...cybersecurity news"). Checked, not pictured.
        for (const char *said : {"What is the latest cybersecurity news", "What's the weather like in Lisbon tomorrow?", "Hi"}) {
            if (!theme::label_fits(chat.add_user(said, false))) {
                fprintf(stderr, "a chat bubble is too short for its text: %s\n", said);
                failed = true;
            }
        }
        chat.clear();
        chat.add_note("Press TAB to talk, press it again when you have finished. Or press ENTER and type.", theme::kDim);
        snap("welcome");
        {
            // Before an AI service is set up, the note and the bottom row say how to reach settings.
            const std::string base = s.api_base, key = s.api_key;
            s.api_base.clear();
            s.api_key.clear();
            chat.clear();
            welcome();
            update_footer();
            snap("not-set-up");
            s.api_base = base;
            s.api_key = key;
            chat.footer_hints(keyguide::idle_hint(true));   // as a set-up device shows it, for the frames to come
        }

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
        chat.set_now_playing("Harbour Classic FM");      // a radio station shows its name, as the player gives it
        snap("radio-playing");
        chat.set_now_playing("Dancing Queen · ABBA");

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
        chat.footer_hints(keyguide::idle_hint(true));
        snap("alert");
        {
            // The double tap goes through the real key handler, so this also checks the shortcut.
            KeyEvent del; del.key = Key::Backspace; del.state = 1;
            handle_key(del);
            snap("delete-once");                   // one tap alone changes nothing
            handle_key(del);
            snap("new-conversation");
            // A letter key does not start typing (ENTER does); it says how to.
            KeyEvent letter; letter.key = Key::Char; letter.text = "w"; letter.state = 1;
            handle_key(letter);
            update_footer();
            snap("letter-hint");
            if (chat.typing()) {
                fprintf(stderr, "a letter key started typing; only ENTER should\n");
                failed = true;
            }
            level_until = 0;
            {
                // SPACE opens settings and closes them again. H shows every key, as a page that
                // scrolls; ESC (pressed and let go) goes back to the chat.
                KeyEvent space; space.key = Key::Char; space.text = " "; space.state = 1;
                handle_key(space);
                const bool opened = menu.open() && menu.depth() == 1 && menu.title() == "Settings" && menu.selected() == "Assistant";
                handle_key(space);
                const bool closed = !menu.open();
                KeyEvent h; h.key = Key::Char; h.text = "h"; h.state = 1;
                handle_key(h);
                const bool guide = menu.open() && menu.title() == kKeyGuideTitle && menu.first_shown() == 0;
                snap("keys");
                KeyEvent down; down.key = Key::Down; down.state = 1;
                for (int i = 0; i < 8; ++i) handle_key(down);
                snap("keys-more");
                for (int i = 0; i < 30; ++i) handle_key(down);   // stops at the end
                snap("keys-end");
                const bool scrolled = menu.first_shown() > 8;
                handle_key(h);                                   // already showing: nothing more
                const bool once = menu.depth() == 1;
                KeyEvent esc; esc.key = Key::Esc; esc.state = 1;
                handle_key(esc);
                esc.state = 0;
                handle_key(esc);
                const bool back = !menu.open() && !chat.typing();
                if (!opened || !closed || !guide || !scrolled || !once || !back) {
                    fprintf(stderr, "SPACE or H did not open and close settings or the key guide as they should\n");
                    failed = true;
                }
                // Every line of the guide, and the hints on the bottom row, fit their room in the real fonts.
                auto width = [](const std::string &text, const lv_font_t *font) {
                    lv_point_t size;
                    lv_text_get_size(&size, text.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                    return static_cast<int>(size.x);
                };
                for (const keyguide::Section &sec : keyguide::sections())
                    for (const keyguide::Row &row : sec.rows)
                        if (width(row.keys, theme::bold()) > Menu::kPairKeyW - 4 || width(row.action, theme::body()) > Menu::pair_text_width()) {
                            fprintf(stderr, "the key guide's line for %s does not fit the screen\n", row.keys.c_str());
                            failed = true;
                        }
                for (const bool configured : {true, false})
                    if (width(keyguide::idle_hint(configured), theme::small()) > theme::kScreenW - 12) {
                        fprintf(stderr, "the hints on the bottom row do not fit: %s\n", keyguide::idle_hint(configured).c_str());
                        failed = true;
                    }
            }
            KeyEvent vol; vol.key = Key::VolumeUp; vol.state = 1;
            handle_key(vol);
            update_footer();
            snap("volume");
            vol.key = Key::Mute;
            handle_key(vol);
            update_footer();
            snap("muted");
            handle_key(vol);                       // unmute again
            level_until = 0;
            for (const auto &shot : {std::make_pair("box", 2500L), std::make_pair("box", 6000L), std::make_pair("sigh", 9000L)}) {
                breath.start(shot.first, 5);
                breath.show_at(shot.second);
                snap(shot.second == 2500L ? "breathing-in" : shot.second == 6000L ? "breathing-hold" : "breathing-out");
            }
            breath.stop();
        }

        // Timers, at fixed times so that the images come out the same on every run.
        const time_t t0 = 1800000000;
        {
            chat.clear();
            chat.add_user("The food needs to come out of the oven in 5 mins", false);
            chat.add_tool("Timers");
            chat.begin_answer();
            chat.append_answer("Your 5 minute timer for the food is running.");
            chat.end_answer("", false);
            chat.footer_hints(keyguide::idle_hint(true));
            chat.set_status(s.agent_name, theme::kFaint);
            chat.set_now_playing("So What");                       // short enough not to scroll
            chat.set_timer(timers::badge({t0 + 300}, t0 + 1));
            snap("timer-one");
            {
                // A title that fits its whole room stays still when a countdown takes part of it: it
                // is cut short, not scrolled (a moving title redraws the status bar many times a
                // second for as long as the timer runs). Too long for its whole room, a title scrolls
                // again once the countdown has gone.
                chat.set_timer("");
                chat.set_now_playing("Round Midnight");
                lv_refr_now(nullptr);
                const bool fits = !chat.now_playing_moves();
                chat.set_timer(timers::badge({t0 + 300}, t0 + 1));
                lv_refr_now(nullptr);
                const bool still = !chat.now_playing_moves();
                chat.set_timer("");
                chat.set_now_playing("Dancing Queen · ABBA");
                lv_refr_now(nullptr);
                const bool moves = chat.now_playing_moves();
                if (!fits || !still || !moves) {
                    fprintf(stderr, "the now-playing text scrolled beside a countdown, or not when it needs to\n");
                    failed = true;
                }
            }
            chat.add_user("I want to study for an hour, can you set a timer", false);
            chat.add_tool("Timers");
            chat.begin_answer();
            chat.append_answer("Your study timer is set for an hour. The food has 4 minutes 41 seconds left.");
            chat.end_answer("", false);
            chat.set_now_playing("");
            chat.set_timer(timers::badge({t0 + 300, t0 + 19 + 3600}, t0 + 19));
            snap("timer-two");
            chat.add_alert("Time's up: Food out of the oven");
            chat.set_timer(timers::badge({t0 + 19 + 3600}, t0 + 300));
            banner.show("Time's up", "Food out of the oven");
            snap("timer-alarm");
            banner.hide();
            chat.set_timer("");
        }
        {
            // The same through the app's own path: a timer that has just ended rings (silently,
            // as when the user has muted the device), and a key press silences it and goes no further.
            muted = true;
            Store::get().add_timer("Eggs", 1, time(nullptr) - 5);
            update_timers(display::now_ms());
            const bool rang = alarm.ringing() && banner.visible() && audio::Music::get().holding(audio::Music::kAlarm);
            KeyEvent key; key.key = Key::Char; key.text = "a"; key.state = 1;
            handle_key(key);
            snap("timer-silenced");
            if (!rang || alarm.ringing() || banner.visible() || chat.typing() || audio::Music::get().holding(audio::Music::kAlarm)) {
                fprintf(stderr, "the timer alarm did not ring and stop as it should\n");
                failed = true;
            }
            muted = false;
        }

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
        e = CalEvent();
        e.title = "Mum's birthday"; e.all_day = true; e.repeat = "yearly"; e.remind_min = 0;
        e.start = util::make_local(1960, util::local_fields(day_at(9, "00:00")).month, util::local_fields(day_at(9, "00:00")).day, 0, 0);
        const int birthday = store.add_event(e);
        store.add_memory("Allergic to shellfish");
        store.add_memory("Prefers window seats");
        store.add_reminder("Call the hotel", t0 + 5400);   // fixed, like the timers: the Reminders page shows it
        const int food = store.add_timer("Food out of the oven", 300, t0);
        store.add_timer("Study", 3600, t0 + 19);
        timers::fix_clock(t0 + 60);                    // what the Reminders page counts down from
        sample_home();

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
            menu.problem("That did not work", "The AI service is down or unreachable (HTTP 530, code 1033). "
                                              "This is on the service's side; try again later.");
            snap("setup-problem");
            menu.pop();
            for (int i = 0; i < 4; ++i) menu.key(down);
            menu.key(enter);                       // skip the live test in screenshots
            s.llm_model = "example-chat-model"; s.stt_model = "example-speech-to-text"; s.tts_model = "example-text-to-speech";
            menu.refresh();
            snap("setup-models");
            for (int i = 0; i < 4; ++i) menu.key(down);
            menu.key(enter);
            s.user_name = "Sam"; s.home_city = "London"; s.home_country = "United Kingdom";
            menu.refresh();
            snap("setup-you");
            for (int i = 0; i < 6; ++i) menu.key(down);
            menu.key(enter);
            snap("setup-extras");
            menu.close();
        }

        // Made-up stations for the radio page, one entry of them without its address.
        util::make_dirs(util::config_dir());
        util::write_file_atomic(radio::path(), R"({"stations": [
            {"name": "Harbour Classic FM", "nicknames": ["Harbour"], "url": "https://stream.example.org/harbour.mp3"},
            {"name": "Valley Radio", "url": "https://stream.example.org/valley.aac",
             "low_data_url": "https://stream.example.org/valley-low.aac"},
            {"name": "Night Jazz", "url": "https://stream.example.org/jazz/live.m3u8"},
            {"name": "Coast News"}]})");
        open_menu();
        snap("settings");                              // the top page: a row for each section
        KeyEvent down; down.key = Key::Down; down.state = 1;
        KeyEvent enter; enter.key = Key::Enter; enter.state = 1;
        KeyEvent esc; esc.key = Key::Esc; esc.state = 1;
        menu.key(enter);                               // "Assistant"
        snap("settings-assistant");
        menu.key(esc);
        menu.key(down);                                // "Device"
        menu.key(enter);
        snap("settings-device");
        menu.key(esc);
        menu.key(down);                                // "Your data", whose first row is "Calendar"
        menu.key(enter);
        snap("settings-data");
        if (menu.title() != "Your data" || menu.selected() != "Calendar") {
            fprintf(stderr, "the settings section did not open on its first row\n");
            failed = true;
        }
        menu.key(enter);
        snap("calendar");
        menu.key(enter);
        snap("event");
        menu.key(enter);
        snap("edit-text");
        menu.key(esc); menu.key(esc);
        menu.key(down); menu.key(down);                // the third event: the birthday
        menu.key(enter);
        snap("event-birthday");
        menu.key(esc); menu.key(esc);
        menu.key(down);                                // "Reminders and timers"
        menu.key(enter);
        snap("reminders");
        {
            // A timer that ends while the page is open takes its row away: the cursor stays on the
            // timer it was on, and the question before cancelling one says which.
            menu.key(down);                            // "Study"
            store.delete_timer(food);                  // as when "Food out of the oven" has just ended
            menu.tick();
            const bool kept = menu.selected() == "Study";
            menu.key(enter);
            const bool named = menu.title() == "Cancel Study?";
            menu.key(esc);
            store.add_timer("Food out of the oven", 300, t0);       // as it was, for the screens to come
            menu.tick();
            if (!kept || !named || menu.selected() != "Study") {
                fprintf(stderr, "the cursor left its timer when the rows changed, or the question did not name it\n");
                failed = true;
            }
        }
        menu.key(esc);
        menu.key(down);                                // "Memory"
        menu.key(enter);
        snap("memory");
        menu.key(esc);
        menu.key(down); menu.key(down);                // "Smart home"
        menu.key(enter);
        snap("smart-home");
        menu.key(enter);                               // the first thing: Bed heater
        snap("home-thing");
        menu.key(esc);
        menu.key(down);                                // Desk lamp, one socket of a strip
        menu.key(enter);
        for (int i = 0; i < 5; ++i) menu.key(down);    // "Part of"
        menu.key(enter);
        snap("home-device");
        menu.key(esc); menu.key(esc);
        for (int i = 0; i < 5; ++i) menu.key(down);    // the strip that listed nothing, under "To set up"
        snap("home-to-set-up");
        KeyEvent up; up.key = Key::Up; up.state = 1;
        menu.key(up);                                  // the plug still to be set up
        menu.key(enter);
        snap("home-setup");
        for (int i = 0; i < 3; ++i) menu.key(down);    // "Set up…"
        menu.key(enter);
        snap("home-pairing");
        menu.key(esc); menu.key(esc);
        for (int i = 0; i < 4; ++i) menu.key(down);    // "Tapo account", past the strip that listed nothing
        menu.key(enter);
        snap("tapo-account");
        menu.key(esc); menu.key(esc);                  // back to "Your data", on "Smart home"
        menu.key(down); menu.key(down);                // "Radio stations", after "Music library"
        menu.key(enter);
        snap("radio-stations");
        if (menu.title() != "Radio stations") {
            fprintf(stderr, "the key presses did not reach the radio stations page\n");
            failed = true;
        }
        for (int i = 0; i < 3; ++i) menu.key(down);    // "Read the file again", below how to add stations
        snap("radio-stations-help");
        menu.key(esc);                                 // back to "Your data"
        menu.key(esc);                                 // the top page, still on "Your data"
        menu.key(down);                                // "AI service"
        menu.key(enter);
        snap("settings-service");
        menu.key(esc);
        menu.key(down);                                // "About"
        menu.key(enter);
        snap("settings-about");
        menu.key(esc);
        menu.key(down);                                // "Keys": from settings, it opens at the keys for settings
        menu.key(enter);
        snap("keys-settings");
        if (menu.title() != kKeyGuideTitle || menu.first_shown() == 0) {
            fprintf(stderr, "the key guide did not open at the keys for settings\n");
            failed = true;
        }
        (void)flight; (void)birthday;
        menu.close();
        timers::fix_clock(0);
        return failed ? 1 : 0;
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
    app.breath.create(screen);
    app.banner.create(screen);
    if (shots) return app.render_shots(shots_dir);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    const Settings &s = Settings::get();
    display::set_brightness(s.brightness);
    audio::set_mic_gain(s.mic_gain);
    audio::set_mic_builtin(s.mic_builtin);
    audio::set_volume_async(s.volume);

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

    app.alarm.stop();                              // lets go of the music
    audio::AlarmSound::get().stop_and_wait();      // a player left running would ring on after the app
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
