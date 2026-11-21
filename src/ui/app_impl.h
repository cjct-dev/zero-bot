// The app controller's state and parts, for the files that make it up. Nothing else
// includes it: the rest of the program sees only ui/app.h.
#pragma once
#include <atomic>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "agent/speech.h"
#include "core/timers.h"
#include "platform/audio.h"
#include "platform/keys.h"
#include "ui/alarm_view.h"
#include "ui/breath_view.h"
#include "ui/chat_view.h"
#include "ui/key_guide.h"
#include "ui/menu.h"
#include "ui/screen_idle.h"

namespace ui {

inline constexpr unsigned long kDoubleTapMs = 450;     // two presses this close together count as a double tap
inline constexpr unsigned long kEscHoldMs = 1200;      // hold ESC this long to leave the app
inline constexpr double kMinSpeechSeconds = 0.4;
inline constexpr double kMaxSpeechSeconds = 90;

extern std::atomic<bool> g_exit;   // set to leave the main loop (run_app)

// A photo's thumbnail, shown in the chat, lives in the cache folder until its chat row goes
// (ChatView::add_image). Rows are not deleted when the app ends, so the next start clears those.
inline constexpr const char *kThumbPrefix = "thumb_";
void remove_old_thumbnails();

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
    // DIAGNOSTICS (temporary, ZB-051): SPACE once opens settings, twice saves the conversation.
    // Without the diagnostics, SPACE would open settings at once again (open_menu in handle_key).
    keyguide::DoubleTap space_tap{kDoubleTapMs};
    // Volume and brightness keys: what the footer shows for a moment, and settings waiting to be
    // written once the key has been let go (so a held key does not rewrite the file ten times a second).
    std::string level_text;
    unsigned long level_until = 0, settings_dirty_at = 0;
    bool muted = false;
    unsigned long esc_down = 0;
    bool esc_armed = false;
    screen::Idle idle;                // when the screen dims and goes off (screen_idle.h)
    unsigned long last_tick = 0, last_second = 0, last_indicators = 0, last_model_try = 0;
    std::atomic<bool> selecting_models{false};
    std::atomic<bool> connecting_phone{false};
    bool warned_models = false;

    // ------------------------------------------------------------ setup
    void wire_agent();
    void auto_select_models();
    void welcome();

    // ------------------------------------------------------------ recording
    void start_recording();
    void finish_recording(bool submit);
    void when_online(std::function<void()> send);
    void run_setup();

    // ------------------------------------------------------------ keys
    void open_menu();
    void save_conversation();
    bool settings_may_open();
    void settle_keys(unsigned long now);
    void new_conversation();
    void apply_volume(int percent);
    static std::string level_bar(const char *label, int percent);
    void show_level(const std::string &text);
    bool control_key(const KeyEvent &ev);
    std::string station_chosen;   // the station being checked, until it plays or is given up
    unsigned long station_chosen_at = 0;
    void change_station(int step);
    void handle_key(const KeyEvent &ev);
    void short_escape();

    // ------------------------------------------------------------ periodic work
    void update_footer();
    void update_indicators();
    void update_timers(unsigned long now_ms);
    void timers_ended(const std::vector<timers::Ended> &ended);
    void sync_banner();
    void check_alerts();
    void set_light(screen::Light from, screen::Light to);
    bool wake_screen(unsigned long now);
    void tick_screen(unsigned long now, bool busy);
    void periodic();

    // ------------------------------------------------------------ screenshots
    int render_shots(const std::string &dir);
    template <typename Snap>
    bool diagnostics_shots(Snap &snap);
};

}  // namespace ui
