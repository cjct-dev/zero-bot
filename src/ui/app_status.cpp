// The app's periodic work: the footer and the status bar, timers and their alarm, reminders, the
// screen's light (dimmed, off, lit), the art's scene of what plays, the main loop's tick (periodic) and
// how long the loop sleeps between ticks (sleep_ms).
#include "ui/app_impl.h"

#include <algorithm>
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
        // What the answer is doing: one that is heard is spoken, and TAB stops it; one that is only shown (a
        // typed request's, or any with spoken replies off: ZB-091) is Answering, and TAB does not stop it.
        case AgentState::Speaking:
            chat.footer_busy("Speaking   ·   TAB to interrupt", theme::kAccent);
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
    locate_home(online);
    const bool phone = link.compare(0, 4, "bnep") == 0;
    int battery = -1;
#if !ZB_DESKTOP
    const power::Battery gauge = power::battery();
    battery = gauge.percent;
    charging = gauge.charging;
    on_battery = gauge.discharging;
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
// the user is watching or waiting for. Music and radio play on with the screen off, but a scene in
// front of the chat keeps it on, dimmed.
void App::tick_screen(unsigned long now, bool busy) {
    const screen::Light was = idle.light();
    set_light(was, idle.update(now, busy, Settings::get().screen_off_min, art.in_front()));
}

// ------------------------------------------------------------ the art (ui/art)
// The scene of what plays now: the radio's mast while a station plays (not paused); nothing else has one yet.
App::Playing App::playing() {
    audio::Music &music = audio::Music::get();
    if (music.live() && !music.paused()) return {"radio", music.title()};
    return {};
}

// On every pass of the main loop: the breathing screen's scene, or its counter, as Dynamic art allows; the scene of
// what plays, when Dynamic art allows it; whether it is in front of the chat (screen_idle.h: after 20 s unused, never
// over what the user has in front of it); its next frame or dissolve step. Under the menu or the breathing screen, or
// on a dark screen, nobody would see it dissolve: it goes at once. A key (wake_screen) is a use, so the scene
// dissolves in the same pass that the key did its job in.
void App::follow_art(unsigned long now, const Playing &what) {
    const bool allowed = screen::art_allowed(Settings::get().dynamic_art, on_battery);
    breath.follow_art(allowed, now);
    const bool wanted = !what.scene.empty() && allowed;
    art.play(wanted ? what.scene : "", wanted ? what.title : "");
    const bool covered = menu.open() || breath.active();
    const bool blocked = covered || chat.typing() || recorder.active() || banner.visible();
    art.front(screen::scene_in_front(idle, now, wanted, blocked), now, covered || idle.light() == screen::Light::Off);
    art.tick(now);
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
    // Wait for the assistant to finish its sentence before the breathing exercise takes over the screen.
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
    if (now - last_second >= kSecondMs) {
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
    if (now - last_indicators >= kIndicatorsMs || last_indicators == 0) {
        last_indicators = now ? now : 1;
        update_indicators();
    }
    follow_art(now, playing());
}

// ------------------------------------------------------------ how long the main loop sleeps
// Something on screen moves or is awaited, so periodic() has work every tick: a request, its speech,
// the microphone, the chat's or a menu's own ticks, the breathing exercise, a timer's alarm, a level
// shown for a moment, settings waiting to be saved, ESC held, a SPACE that may be the first of two.
bool App::lively() {
    Agent &agent = Agent::get();
    return recorder.active() || agent.state() != AgentState::Idle || agent.busy() || agent.has_pending() ||
           SpeechQueue::get().active() || connecting_phone || chat.needs_tick() || menu.needs_tick() ||
           breath.active() || breath_minutes > 0 || alarm.ringing() || banner.visible() || level_until != 0 ||
           settings_dirty_at != 0 || esc_armed || space_tap.pending();
}

// How long the main loop may sleep before periodic() has work; a key or anything posted wakes it
// sooner (uiq::wait). While something is lively, and for kSettleMs after (what has just ended still
// shows: the footer goes back to its hints), until the next tick. Else until the next of the
// once-a-second, indicator and model work, and while a timer runs, the wall clock's next second (its
// countdown changes then, and its alarm rings on one). On the device that is one or two wake-ups a
// second instead of sixty-two (ZB-036). Whatever it is, no later than the art's next frame or dissolve
// step (4 a second while the radio's or the breathing scene shows, none while neither does).
unsigned long App::sleep_ms(unsigned long now) {
    if (lively()) quiet_since = 0;
    else if (!quiet_since) quiet_since = now;
    auto until = [now](unsigned long at) { return at > now ? at - now : 0; };
    const unsigned long art_next = std::min(art.ms_to_next(now), breath.ms_to_next(now));
    if (!quiet_since || now - quiet_since < kSettleMs) return std::min(until(last_tick + kTickMs), art_next);
    const unsigned long work = std::min(art_next, until(std::min({last_second + kSecondMs, last_indicators + kIndicatorsMs,
                                                                  last_model_try + model_retry_ms})));
    return Store::get().next_timer_end() ? std::min(work, timers::ms_to_next_second()) : work;
}

}  // namespace ui
