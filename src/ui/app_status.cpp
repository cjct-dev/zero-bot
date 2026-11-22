// The app's periodic work: the footer and the status bar, timers and their alarm, reminders, the
// screen's light (dimmed, off, lit), and the main loop's tick (periodic).
#include "ui/app_impl.h"

#include <ctime>
#include <string>
#include <vector>

#include "agent/agent.h"
#include "agent/speech.h"
#include "core/music_lib.h"
#include "core/settings.h"
#include "core/store.h"
#include "core/tether.h"
#include "core/timers.h"
#include "core/util.h"
#include "platform/audio.h"
#include "platform/display.h"
#include "platform/power.h"
#include "platform/sys.h"
#include "ui/key_guide.h"
#include "ui/theme.h"

namespace ui {

// ------------------------------------------------------------ periodic work
void App::update_footer() {
    Agent &agent = Agent::get();
    const Settings &s = Settings::get();
    if (level_until && sys::now_ms() < level_until && !recorder.active() && !chat.typing()) {
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
        // What the answer is doing: a spoken request's is spoken (only shown with spoken replies off), and
        // TAB stops it; a typed request's is only shown (ZB-091), and TAB does not stop it.
        case AgentState::Speaking:
            chat.footer_busy(s.speak_replies ? "Speaking   ·   TAB to interrupt" : "Answering", theme::kAccent);
            chat.set_status(s.agent_name, theme::kAccent);
            break;
        case AgentState::Answering:
            chat.footer_busy("Answering", theme::kAccent);
            chat.set_status(s.agent_name, theme::kAccent);
            break;
        case AgentState::Idle:
            chat.footer_hints(keyguide::idle_hint(s.configured()));
            chat.set_status(s.agent_name, theme::kFaint);
            break;
    }
}

void App::update_indicators() {
    const std::string link = tether::default_link();
    bool online = !link.empty(), charging = false;
    follow_connection(online);
    const bool phone = link.compare(0, 4, "bnep") == 0;
    int battery = -1;
#if !ZB_DESKTOP
    const power::Battery gauge = power::battery();
    battery = gauge.percent;
    charging = gauge.charging;
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
    if (music.live()) chat.set_now_playing(music.title(), true);   // shown for hours: it never scrolls
    else if (current.empty()) chat.set_now_playing("");
    else {
        const Track t = MusicLibrary::get().lookup(current);
        chat.set_now_playing(t.artist.empty() ? t.title : t.title + " · " + t.artist);
    }
}

// ------------------------------------------------------------ timers
// Runs on every tick. Cheap unless a timer has just ended or the countdown's text has changed:
// the store is only asked when its next end has come or its timers have changed.
void App::update_timers(unsigned long now_ms) {
    const time_t now = timers::follow_clock();     // a clock set meanwhile moves the timers with it
    const std::vector<timers::Ended> ended = alarm.poll(now, now_ms, recorder.active(), muted);
    if (!ended.empty()) timers_ended(ended);
    alarm.tick(now_ms);
    sync_banner();
    // Under a menu or the breathing exercise the status bar cannot be seen; changing it would
    // only make the screen redraw what is hidden. It catches up when they close.
    if (!menu.open() && !breath.active()) {
        const int revision = Store::get().timers_revision();
        if (revision != timer_revision) {
            timer_revision = revision;
            timer_ends.clear();
            for (const Reminder &t : Store::get().timers()) timer_ends.push_back(t.due);
        }
        chat.set_timer(timers::badge(timer_ends, now));
    }
    // A timer that has ended turns the screen on at once, and it stays lit while the alarm
    // rings. Last, so that the frame drawn before the light comes on already has the banner,
    // the alert and the countdown as they now are.
    if (!ended.empty() || alarm.ringing()) wake_screen(now_ms);
}

void App::timers_ended(const std::vector<timers::Ended> &ended) {
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
    if (!names.empty() && Settings::get().speak_replies && !muted) Agent::get().say("Time's up: " + names + ".");
}

void App::sync_banner() {
    if (!alarm.ringing()) { banner.hide(); return; }
    const std::vector<std::string> &labels = alarm.labels();
    std::string names;
    for (const std::string &l : labels) names += (names.empty() ? "" : ", ") + l;
    banner.show(labels.size() == 1 ? "Time's up" : std::to_string(labels.size()) + " timers are up", names);
}

void App::check_alerts() {
    if (recorder.active()) return;   // a reminder waits rather than talking over the user
    for (auto &text : Store::get().take_due_alerts(time(nullptr))) {
        chat.add_alert(text);
        audio::chime(2);
        if (Settings::get().speak_replies) Agent::get().say("Reminder. " + text);
        wake_screen(sys::now_ms());
    }
}

// ------------------------------------------------------------ the screen's light
// From one light to another, as screen_idle.h decides: dimmed, off (drawn black, nothing drawn,
// backlight off), or lit at the brightness setting.
void App::set_light(screen::Light from, screen::Light to) {
    if (from == to) return;
    // Dimmed or off, a crash would leave the backlight so: it puts back the setting instead.
    display::restore_after_crash(to == screen::Light::Lit ? -1 : Settings::get().brightness);
    if (to == screen::Light::Off) { display::screen_off(); return; }
    const int percent = to == screen::Light::Dimmed ? screen::kDimPercent : Settings::get().brightness;
    if (from == screen::Light::Off) display::screen_on(percent);   // redrawn before the light comes on
    else display::set_brightness(percent);
}

// A key, an alarm, a reminder, a request: the screen lit from now on. True when it was off.
bool App::wake_screen(unsigned long now) {
    const screen::Light was = idle.use(now);
    set_light(was, screen::Light::Lit);
    return was == screen::Light::Off;
}

// Every second: dims, then goes off after the minutes set, unless something is going on that
// the user is watching or waiting for. Music and radio play on with the screen off.
void App::tick_screen(unsigned long now, bool busy) {
    const screen::Light was = idle.light();
    set_light(was, idle.update(now, busy, Settings::get().screen_off_min));
}

void App::periodic() {
    const unsigned long now = sys::now_ms();
    if (esc_armed && now - esc_down >= kEscHoldMs) { g_exit = true; return; }
    settle_keys(now);   // DIAGNOSTICS (temporary)
    if (now - last_tick >= kTickMs) {
        last_tick = now;
        update_footer();
        chat.tick();
        menu.tick();
        if (breath.active()) {
            wake_screen(now);                          // the screen stays lit for the whole exercise
            if (breath.tick(now)) audio::chime(1);     // a soft note when the last breath is done
        }
        update_timers(now);
    }
    // Wait for the assistant to finish its sentence before the orb takes over the screen.
    if (breath_minutes > 0 && !recorder.active() && Agent::get().state() == AgentState::Idle && !Agent::get().busy() &&
        !Agent::get().has_pending() && !SpeechQueue::get().active()) {
        if (menu.open()) menu.close();
        if (chat.typing()) chat.typing_end();
        wake_screen(now);
        breath.start(breath_pattern, breath_minutes);
        breath_minutes = 0;
        if (banner.visible()) banner.raise();         // the exercise came forward; an alarm stays in front
    }
    if (settings_dirty_at && now - settings_dirty_at > 1500) {
        settings_dirty_at = 0;
        Settings::save();
    }
    if (now - last_second >= 1000) {
        last_second = now;
        check_alerts();
        const bool active = recorder.active() || Agent::get().state() != AgentState::Idle;
        // Belt and braces: whatever happened to a request, music is never left waiting for
        // an answer that is not coming.
        if (!active && !Agent::get().busy() && !Agent::get().has_pending() && !SpeechQueue::get().active() && !connecting_phone)
            audio::Music::get().hold(audio::Music::kExchange | audio::Music::kSpeech, false);
        tick_screen(now, active || Agent::get().busy() || Agent::get().has_pending() || SpeechQueue::get().active());
    }
    if (now - last_model_try >= model_retry_ms) {   // covers starting up before Wi-Fi has connected
        last_model_try = now;
        auto_select_models();
    }
    if (now - last_indicators >= 5000 || last_indicators == 0) {
        last_indicators = now ? now : 1;
        update_indicators();
    }
}

}  // namespace ui
