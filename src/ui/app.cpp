#include "ui/app.h"
#include "ui/app_impl.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "agent/agent.h"
#include "core/settings.h"
#include "core/tether.h"
#include "core/ui_queue.h"
#include "core/util.h"
#include "lvgl.h"
#include "platform/audio.h"
#include "platform/display.h"
#include "platform/sys.h"
#include "platform/keys.h"
#include "platform/proc.h"
#include "ui/models.h"
#include "ui/settings_pages.h"
#include "ui/theme.h"

namespace ui {

std::atomic<bool> g_exit{false};

namespace {

void on_signal(int) { g_exit = true; }

}  // namespace

void remove_old_thumbnails() {
    const std::string dir = util::cache_dir();
    std::vector<std::string> old;
    if (DIR *d = opendir(dir.c_str())) {
        while (const dirent *e = readdir(d))
            if (strncmp(e->d_name, kThumbPrefix, strlen(kThumbPrefix)) == 0) old.push_back(dir + "/" + e->d_name);
        closedir(d);
    }
    for (const std::string &path : old) unlink(path.c_str());
}

// ------------------------------------------------------------ setup
void App::wire_agent() {
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
        // because the image cache is keyed by path, and is deleted with its chat row (or at
        // the next start: remove_old_thumbnails).
        static std::atomic<int> serial{0};
        const std::string base = util::cache_dir() + "/" + kThumbPrefix + std::to_string(serial++) + "_" + std::to_string(time(nullptr));
        std::string data;
        if (!util::read_file(jpeg, data) || !util::write_file_atomic(base + ".jpg", data)) return;
        std::thread([this, base] {
            const std::string thumb = base + ".png";
            const auto res = proc::run({"ffmpeg", "-v", "error", "-y", "-i", base + ".jpg", "-vf", "scale=128:-2", thumb}, "", 8000);
            unlink((base + ".jpg").c_str());
            if (res.ok()) uiq::post([this, thumb] { chat.add_image(thumb, true); });
            else unlink(thumb.c_str());
        }).detach();
    };
    Agent::get().start(ev);
}

// Picks sensible models the first time, so the app works before settings are touched.
void App::auto_select_models() {
    const std::string service = models_wanted();
    if (service.empty() || selecting_models.exchange(true)) return;
    std::thread([this, service] {
        std::string err;
        const models::List list = models::list(err);
        selecting_models = false;
        uiq::post([this, list, err, service] { models_answered(list, err, service); });
    }).detach();
}

// The service (address and key) to ask for its models, or "" when there is nothing to ask. A
// service's list is read once: what it lacks (since ZB-088 a service that labels none of its models
// gets no chat model chosen) the user picks in settings, and asking again would only wake the Wi-Fi.
// Only a service with another address or key is asked again (periodic() looks every model_retry_ms).
std::string App::models_wanted() const {
    const Settings s = Settings::get();
    if (!s.configured() || (!s.stt_model.empty() && !s.tts_model.empty() && !s.eff_llm_model().empty())) return "";
    const std::string service = s.eff_api_base() + '\n' + s.eff_api_key();
    return service == models_listed_for ? "" : service;
}

// The service's answer, on the UI thread. One that could not be reached (offline, most likely) is
// asked again after twice as long each time, up to kModelRetryMaxMs, and at once when the
// connection comes back (follow_connection).
void App::models_answered(const models::List &list, const std::string &err, const std::string &service) {
    if (!err.empty()) {
        model_retry_ms = std::min(model_retry_ms * 2, kModelRetryMaxMs);
        return;
    }
    models_listed_for = service;
    model_retry_ms = kModelRetryMs;
    const Settings st = Settings::update([&list](Settings &s) { models::choose(s, list); });
    if ((st.stt_model.empty() || st.tts_model.empty()) && !warned_models) {
        warned_models = true;
        chat.add_note("Choose speech models in settings (SPACE) to use voice.", theme::kThink);
    }
}

// A connection that comes back asks at once for the models still to be chosen (auto_select_models).
void App::follow_connection(bool online) {
    if (online && !was_online) {
        model_retry_ms = kModelRetryMs;
        last_model_try = 0;
    }
    was_online = online;
}

void App::welcome() {
    const Settings &s = Settings::get();
    if (!s.configured())
        chat.add_note("No AI service is set up yet. Press SPACE for settings and enter an address and key under AI service.",
                      theme::kThink);
    else
        chat.add_note("Press TAB to talk, press it again when you have finished. Or press ENTER and type.",
                      theme::kDim);
}

// ------------------------------------------------------------ recording
void App::start_recording() {
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
    rec_start = sys::now_ms();
}

void App::finish_recording(bool submit) {
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
void App::when_online(std::function<void()> send) {
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
void App::run_setup() {
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
    display::restore_after_crash(-1);              // lit as set; installs the crash handler (set_light)

    const Settings &s = Settings::get();           // a snapshot, as the settings were at the start
    display::set_brightness(s.brightness);
    audio::set_mic_gain(s.mic_gain);
    audio::set_mic_builtin(s.mic_builtin);
    audio::set_volume_async(s.volume);

    remove_old_thumbnails();   // before the first photo of this run
    app.wire_agent();
    app.welcome();
    app.auto_select_models();
    if (!s.setup_done) app.run_setup();
    input::set_dump(keydump);
    input::start();
    app.idle.use(sys::now_ms());

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
    display::screen_on(Settings::get().brightness);   // as set now, not at the start; drawn again if it was off
    // Leave a clean screen for the launcher to draw over.
    lv_obj_clean(screen);
    lv_refr_now(nullptr);
    return 0;
}

}  // namespace ui