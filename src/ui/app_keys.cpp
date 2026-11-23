// The app's keys: what each does on the chat screen, while typing or listening, in settings, over an
// alarm or the breathing exercise. Which key means what on the chat screen is keyguide::chat_key's.
#include "ui/app_impl.h"

#include <algorithm>
#include <functional>
#include <string>

#include "agent/agent.h"
#include "core/activity.h"
#include "core/radio.h"
#include "core/settings.h"
#include "core/util.h"
#include "platform/audio.h"
#include "platform/display.h"
#include "platform/sys.h"
#include "ui/key_guide.h"
#include "ui/settings_pages.h"
#include "ui/theme.h"

namespace ui {

// ------------------------------------------------------------ keys
void App::open_menu() {
    SettingsHooks hooks;
    hooks.new_conversation = [this] { new_conversation(); };
    hooks.exit_app = [] { g_exit = true; };
    hooks.run_setup = [this] { menu.close(); run_setup(); };
    open_settings(menu, hooks);
}

// DIAGNOSTICS (temporary, ZB-051; core/activity.h): SPACE twice saves the conversation so far,
// from its start, to help diagnose and improve the assistant. Nothing is saved any other way.
void App::save_conversation() {
    size_t dropped = 0;
    const json conversation = Agent::get().whole_conversation(&dropped);
    show_level(activity::saved_toast(activity::save_conversation(conversation, dropped)));
}

// DIAGNOSTICS (temporary): one SPACE opens settings once the time for a second has passed,
// unless something else has taken the screen meanwhile.
bool App::settings_may_open() {
    return !menu.open() && !recorder.active() && !chat.typing() && !breath.active() && !alarm.ringing();
}
void App::settle_keys(unsigned long now) {
    if (space_tap.single_due(now) && settings_may_open()) open_menu();
}

// Forgets what has been said so far, and what was still waiting for the phone. Calendar, reminders and
// saved memories are untouched.
void App::new_conversation() {
    Agent::get().reset_conversation();   // cancels first (drop_waiting_input)
    drop_waiting_input();
    chat.clear();
    chat.add_note("New conversation. What was said before is forgotten.", theme::kDim);
    audio::chime(1);
}

// Applies the newest volume off the UI thread; the mixer command takes a moment, and a held
// key asks for many steps in a row.
void App::apply_volume(int percent) { audio::set_volume_async(percent); }

std::string App::level_bar(const char *label, int percent) {
    std::string bar;
    for (int i = 0; i < 10; ++i) bar += i < (percent + 5) / 10 ? "\xE2\x96\xAE" : "\xE2\x96\xAF";   // ▮ ▯
    return std::string(label) + "  " + bar + "  " + std::to_string(percent) + "%";
}

void App::show_level(const std::string &text) {
    level_text = text;
    level_until = sys::now_ms() + 1500;
    if (menu.open()) { menu.toast(text); menu.refresh(); }
}

// The fn-layer control keys work everywhere: while talking, typing, in settings.
bool App::control_key(const KeyEvent &ev) {
    audio::Music &music = audio::Music::get();
    switch (ev.key) {
        case Key::VolumeUp:
        case Key::VolumeDown: {
            if (ev.state == 0) return true;
            const int step = ev.key == Key::VolumeUp ? 5 : -5;
            const int volume = Settings::update([step](Settings &s) {   // saved after a pause (settings_dirty_at)
                s.volume = std::max(0, std::min(100, (s.volume + (step > 0 ? 0 : 4)) / 5 * 5 + step));
            }, false).volume;
            muted = false;
            apply_volume(volume);
            settings_dirty_at = sys::now_ms();
            show_level(level_bar("Volume", volume));
            return true;
        }
        case Key::Mute: {
            if (ev.state != 1) return true;
            muted = !muted;
            const int volume = Settings::get().volume;
            apply_volume(muted ? 0 : volume);
            show_level(muted ? "Muted   ·   fn+A to unmute" : level_bar("Volume", volume));
            return true;
        }
        case Key::BrightnessUp:
        case Key::BrightnessDown: {
            if (ev.state == 0) return true;
            const int step = ev.key == Key::BrightnessUp ? 10 : -10;
            const int brightness = Settings::update([step](Settings &s) {
                s.brightness = std::max(10, std::min(100, (s.brightness + (step > 0 ? 0 : 9)) / 10 * 10 + step));
            }, false).brightness;
            display::set_brightness(brightness);
            settings_dirty_at = sys::now_ms();
            show_level(level_bar("Brightness", brightness));
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

// fn+E and fn+W while a station plays: the next or previous station in the user's list. Its
// stream is checked first, beside this thread, as the radio tool does: a playlist file is
// followed to the stream it names (the player cannot open one), and a station that will not
// play says why at once while the one playing goes on. Presses made meanwhile count on from
// the station chosen last (for as long as a check can take: a station chosen in settings
// meanwhile wins, and this one's answer then never comes).
void App::change_station(int step) {
    const std::vector<radio::Station> list = radio::stations();
    if (list.size() < 2) { show_level(list.empty() ? "No radio stations set up" : "No other station"); return; }
    const unsigned long now = sys::now_ms();
    const bool counting_on = !station_chosen.empty() && now - station_chosen_at < 30000;
    const int at = radio::find(list, counting_on ? station_chosen : audio::Music::get().title());
    const int n = static_cast<int>(list.size());
    const radio::Station next = list[static_cast<size_t>(((at < 0 ? 0 : at + step) % n + n) % n)];
    station_chosen = next.name;
    station_chosen_at = now;
    show_level(next.name);
    radio::check_async(next, [this, name = next.name](const radio::Check &c) {
        station_chosen.clear();
        audio::Music &music = audio::Music::get();
        if (!music.live()) return;   // stopped meanwhile (ESC, or the assistant was asked to)
        if (!c.error.empty()) { chat.add_note(name + " did not play: " + c.error + ".", theme::kListen); return; }
        music.play_live(c.url, name);
        last_indicators = 0;         // the status bar shows the new name at the next tick
    });
}

// A key goes to the first of these that takes it: a dark screen (woken), the press already spent on
// waking it or on an alarm, a ringing alarm, the fn layer's controls, the breathing exercise, ESC,
// settings, TAB, typing, and last the chat screen's own keys.
void App::handle_key(const KeyEvent &ev) {
    const unsigned long now = sys::now_ms();
    // A key on a dark screen only wakes it, whatever is showing (the chat, typing, settings, the
    // key guide), and its repeats and its release go no further either.
    if (ev.state != 0 && wake_screen(now)) {
        spent_key = ev.key;
        spent_text = ev.text;
        return;
    }
    // DIAGNOSTICS (temporary): a SPACE waiting to open settings is a single press once another
    // key comes, so settings open now and the key goes to them, as both did before SPACE SPACE
    // existed ("SPACE, Down, ENTER" still works however quickly it is typed).
    if (ev.state == 1 && ev.key != Key::None && !keyguide::is_settings_key(ev) && space_tap.cancel() && settings_may_open())
        open_menu();
    if (spent(ev) || alarm_key(ev)) return;
    // A modifier tapped on its own, or a key the app has no use for: it has woken the screen or
    // kept it lit, and stopped an alarm ("any key"), above. Nothing more.
    if (ev.key == Key::None) return;

    if (control_key(ev) || breath_key(ev)) return;
    if (ev.key == Key::Esc) { escape_key(ev, now); return; }
    if (menu.open()) { settings_key(ev); return; }
    if (ev.key == Key::Tab) { talk_key(ev, now); return; }
    if (recorder.active() || ev.state == 0) return;
    if (chat.typing()) { typing_key(ev); return; }
    chat_screen_key(ev, now);
}

// The repeats and the release of the press that woke the screen or silenced an alarm: they do nothing.
// A new press ends that.
bool App::spent(const KeyEvent &ev) {
    if (ev.state == 1) {
        spent_key = Key::None;
        return false;
    }
    if (spent_key == Key::None || ev.key != spent_key || ev.text != spent_text) return false;
    if (ev.state == 0) spent_key = Key::None;
    return true;
}

// A ringing alarm takes the next key press, whatever the key and whatever is on screen (the
// chat, a menu, the breathing exercise), and the press goes no further. TAB also starts
// listening, so that "give it five more minutes" is one press away.
bool App::alarm_key(const KeyEvent &ev) {
    if (ev.state != 1 || !alarm.ringing()) return false;
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
    return true;
}

// A breathing exercise takes the whole screen. ESC ends it; TAB ends it and starts listening,
// since wanting to say something is a fair reason to stop. Other keys are left alone so a
// stray press does not break the calm.
bool App::breath_key(const KeyEvent &ev) {
    if (!breath.active()) return false;
    if (ev.state != 1) return true;
    if (ev.key == Key::Esc) { breath.stop(); esc_armed = false; return true; }
    if (ev.key != Key::Tab) return true;
    breath.stop();
    return false;   // TAB goes on, to start listening
}

// ESC: a short press backs out of whatever is happening, a long hold leaves the app.
void App::escape_key(const KeyEvent &ev, unsigned long now) {
    if (ev.state == 1) { esc_down = now; esc_armed = true; }
    if (ev.state == 0 && esc_armed) {
        esc_armed = false;
        if (now - esc_down < kEscHoldMs) short_escape();
    }
}

// A key while settings are open. TAB closes them.
void App::settings_key(const KeyEvent &ev) {
    if (ev.key == Key::Tab) { if (ev.state == 1) menu.close(); return; }
    // Outside a text field, SPACE closes settings as it opened them, and H shows the keys.
    // Not the first-run guide, which SPACE did not open: a new user presses it to choose.
    if (!menu.editing() && keyguide::is_settings_key(ev)) {
        if (menu.in_guide()) menu.toast("Press ENTER to choose");
        else menu.close();
        return;
    }
    if (!menu.editing() && keyguide::is_guide_key(ev)) { open_key_guide(menu, true); return; }
    menu.key(ev);
}

// TAB starts listening and, pressed again, sends what was said; pressed twice quickly it opens
// settings instead.
void App::talk_key(const KeyEvent &ev, unsigned long now) {
    if (ev.state != 1) return;
    if (recorder.active()) {
        if (now - rec_start < kDoubleTapMs) { finish_recording(false); open_menu(); }
        else finish_recording(true);
    } else {
        start_recording();
    }
}

// While typing: ENTER sends the text, Up and Down scroll the chat, the rest edit the line.
void App::typing_key(const KeyEvent &ev) {
    if (ev.key == Key::Enter) {
        if (ev.state != 1) return;
        const std::string text = util::trim(chat.typing_text());
        chat.typing_end();
        if (!text.empty()) when_online([text] { Agent::get().submit_text(text); });
    } else if (ev.key == Key::Up || ev.key == Key::Down) {
        chat.scroll(ev.key == Key::Up ? ChatView::kLineScroll : -ChatView::kLineScroll);
    } else {
        chat.typing_key(ev);
    }
}

// The fn layer's key that a plain Q, W, E, A, S or D stands for on the chat screen.
static Key fn_layer_key(keyguide::ChatKey k) {
    using keyguide::ChatKey;
    switch (k) {
        case ChatKey::PlayPause: return Key::MediaPlay;
        case ChatKey::PreviousTrack: return Key::MediaPrevious;
        case ChatKey::NextTrack: return Key::MediaNext;
        case ChatKey::Mute: return Key::Mute;
        case ChatKey::VolumeDown: return Key::VolumeDown;
        case ChatKey::VolumeUp: return Key::VolumeUp;
        default: return Key::None;
    }
}

// The chat screen's own keys (keyguide::chat_key says which is which).
void App::chat_screen_key(const KeyEvent &ev, unsigned long now) {
    using keyguide::ChatKey;
    const ChatKey k = keyguide::chat_key(ev);
    switch (k) {
        // Two taps of the delete key (not while typing, where it deletes text) start a new conversation.
        case ChatKey::DeleteTap:
            if (last_delete && now - last_delete < kDoubleTapMs) { last_delete = 0; new_conversation(); }
            else last_delete = now;
            break;
        case ChatKey::Type: chat.typing_begin(""); break;
        // SPACE (TAB twice also opens settings). DIAGNOSTICS (temporary, ZB-051): a second SPACE
        // soon after saves the conversation instead, so one SPACE opens settings only once the
        // double-tap window has passed (settle_keys).
        case ChatKey::Settings:
            if (space_tap.press(now)) save_conversation();
            break;
        case ChatKey::Guide: open_key_guide(menu, false); break;
        case ChatKey::ScrollUp: chat.scroll(ChatView::kLineScroll); break;
        case ChatKey::ScrollDown: chat.scroll(-ChatView::kLineScroll); break;
        case ChatKey::PageUp: chat.scroll(ChatView::kPageScroll); break;
        case ChatKey::PageDown: chat.scroll(-ChatView::kPageScroll); break;
        case ChatKey::TypeHint: show_level("Press ENTER to type"); break;
        // Q, W, E, A, S, D: the fn layer's media and volume keys, without fn.
        case ChatKey::PlayPause: case ChatKey::PreviousTrack: case ChatKey::NextTrack: case ChatKey::Mute:
        case ChatKey::VolumeDown: case ChatKey::VolumeUp: {
            KeyEvent fn = ev;
            fn.text.clear();
            fn.key = fn_layer_key(k);
            control_key(fn);
            break;
        }
        case ChatKey::None: break;
    }
}

void App::short_escape() {
    if (menu.open()) { KeyEvent ev; ev.key = Key::Esc; ev.state = 1; menu.key(ev); return; }
    if (recorder.active()) { finish_recording(false); return; }
    if (chat.typing()) { chat.typing_end(); return; }
    Agent &agent = Agent::get();
    if (agent.busy() || agent.state() != AgentState::Idle || !waiting_input.empty()) {
        agent.cancel();   // first (drop_waiting_input)
        drop_waiting_input();
        return;
    }
    if (audio::Music::get().playing()) audio::Music::get().stop();
}

}  // namespace ui
