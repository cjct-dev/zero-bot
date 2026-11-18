// Unit tests for key_guide.cpp: what each key does on the chat screen, and that the key guide says
// so. No screen is needed (the --shots run checks that the guide's lines fit with the real fonts).
// Each check prints one line on failure; the exit status is non-zero when any fails.
#include "ui/key_guide.h"

#include <cstdio>
#include <set>
#include <string>

using keyguide::ChatKey;

namespace {

int g_passed = 0, g_failed = 0;

void check(bool ok, const std::string &what) {
    if (ok) ++g_passed;
    else { ++g_failed; printf("FAIL %s\n", what.c_str()); }
}

KeyEvent key(Key k, int state = 1) {
    KeyEvent ev;
    ev.key = k;
    ev.state = state;
    return ev;
}

KeyEvent text(const std::string &t, int state = 1) {
    KeyEvent ev = key(Key::Char, state);
    ev.text = t;
    return ev;
}

const char *name(ChatKey k) {
    switch (k) {
        case ChatKey::None: return "None";
        case ChatKey::Settings: return "Settings";
        case ChatKey::Guide: return "Guide";
        case ChatKey::ScrollUp: return "ScrollUp";
        case ChatKey::ScrollDown: return "ScrollDown";
        case ChatKey::PageUp: return "PageUp";
        case ChatKey::PageDown: return "PageDown";
        case ChatKey::Type: return "Type";
        case ChatKey::DeleteTap: return "DeleteTap";
        case ChatKey::TypeHint: return "TypeHint";
        case ChatKey::PreviousTrack: return "PreviousTrack";
        case ChatKey::NextTrack: return "NextTrack";
        case ChatKey::Mute: return "Mute";
        case ChatKey::VolumeDown: return "VolumeDown";
        case ChatKey::VolumeUp: return "VolumeUp";
    }
    return "?";
}

void expect(const KeyEvent &ev, ChatKey want, const std::string &what) {
    const ChatKey got = keyguide::chat_key(ev);
    check(got == want, what + ": got " + name(got) + ", expected " + name(want));
}

void test_chat_keys() {
    // SPACE opens settings: a press, not its release.
    expect(text(" "), ChatKey::Settings, "SPACE");
    expect(text(" ", 0), ChatKey::None, "SPACE let go");
    check(keyguide::is_settings_key(text(" ")) && !keyguide::is_settings_key(text(" ", 0)) &&
              !keyguide::is_settings_key(text("s")),
          "only a press of SPACE is the settings key");
    // H (or ?) shows the keys.
    for (const char *t : {"h", "H", "?"}) expect(text(t), ChatKey::Guide, std::string("guide key ") + t);
    check(!keyguide::is_guide_key(text("h", 0)) && !keyguide::is_guide_key(text("j")), "only H and ? are the guide key");
    // F and X scroll, held or not, as fn+F and fn+X (the arrows) do.
    expect(text("f"), ChatKey::ScrollUp, "F");
    expect(text("F"), ChatKey::ScrollUp, "shift+F");
    expect(text("x"), ChatKey::ScrollDown, "X");
    expect(text("X", 2), ChatKey::ScrollDown, "X held");
    expect(key(Key::Up), ChatKey::ScrollUp, "fn+F");
    expect(key(Key::Down, 2), ChatKey::ScrollDown, "fn+X held");
    expect(key(Key::PgUp), ChatKey::PageUp, "page up");
    expect(key(Key::PgDn), ChatKey::PageDown, "page down");
    // ENTER starts typing, once per press.
    expect(key(Key::Enter), ChatKey::Type, "ENTER");
    expect(key(Key::Enter, 2), ChatKey::None, "ENTER held");
    // The delete key counts taps (two make a new conversation); holding it does not.
    expect(key(Key::Backspace), ChatKey::DeleteTap, "del");
    expect(key(Key::Delete), ChatKey::DeleteTap, "fn+del");
    expect(key(Key::Backspace, 2), ChatKey::None, "del held");
    // Other letters and digits only say how to type.
    for (const char *t : {"q", "z", "c", "1", "!"}) expect(text(t), ChatKey::TypeHint, std::string("letter ") + t);
    expect(text("q", 0), ChatKey::None, "a letter let go");
    // W, E, A, S, D: the fn layer's media and volume keys without fn (the owner's request, 0.4.2).
    expect(text("w"), ChatKey::PreviousTrack, "W");
    expect(text("E"), ChatKey::NextTrack, "E");
    expect(text("a"), ChatKey::Mute, "A");
    expect(text("s"), ChatKey::VolumeDown, "S");
    expect(text("D"), ChatKey::VolumeUp, "D");
    expect(text("s", 2), ChatKey::VolumeDown, "S held repeats");
    expect(text("w", 2), ChatKey::None, "W held does not skip again");
    expect(text("a", 2), ChatKey::None, "A held does not toggle again");
    // TAB, ESC and the fn layer's control keys are handled before chat_key is asked.
    for (Key k : {Key::Tab, Key::Esc, Key::VolumeUp, Key::Mute, Key::MediaPlay, Key::Left, Key::Home})
        expect(key(k), ChatKey::None, "a key handled elsewhere");
}

// The guide's first section says what each key does on the chat screen: where that is something
// chat_key decides, it must be what chat_key does.
void test_guide_matches_keys() {
    const auto &sections = keyguide::sections();
    check(sections.size() >= 3, "the guide has sections for the chat, everywhere and settings");
    if (sections.empty()) return;
    struct Said { const char *keys; KeyEvent ev; ChatKey does; };
    const Said said[] = {
        {"SPACE", text(" "), ChatKey::Settings},
        {"SPACE SPACE", text(" "), ChatKey::Settings},   // each press; DoubleTap tells one from two
        {"H", text("h"), ChatKey::Guide},
        {"F  X", text("f"), ChatKey::ScrollUp},
        {"F  X", text("x"), ChatKey::ScrollDown},
        {"fn+F  fn+X", key(Key::Up), ChatKey::ScrollUp},
        {"ENTER", key(Key::Enter), ChatKey::Type},
        {"del del", key(Key::Backspace), ChatKey::DeleteTap},
    };
    for (const Said &s : said) {
        bool listed = false;
        for (const keyguide::Row &r : sections.front().rows) listed = listed || r.keys == s.keys;
        check(listed, std::string("the guide lists ") + s.keys + " on the chat screen");
        expect(s.ev, s.does, std::string("the key the guide calls ") + s.keys);
    }
    // Every key action has a line, the ones handled outside chat_key included.
    std::set<std::string> all;
    for (const keyguide::Section &sec : sections)
        for (const keyguide::Row &r : sec.rows) {
            all.insert(sec.title + ": " + r.keys);
            check(!r.keys.empty() && !r.action.empty(), "a line of the guide is complete");
        }
    const std::string chat = sections.front().title, anywhere = sections[1].title, settings = sections.back().title;
    for (const char *k : {"TAB", "ENTER", "SPACE", "SPACE SPACE", "del del", "ESC", "hold ESC", "F  X", "fn+F  fn+X", "H"})
        check(all.count(chat + ": " + k) == 1, std::string("the chat screen's keys include ") + k);
    for (const char *k : {"fn+S  fn+D", "fn+A", "fn+U  fn+I", "fn+Q", "fn+W  fn+E", "any key"})
        check(all.count(anywhere + ": " + k) == 1, std::string("the keys that work everywhere include ") + k);
    for (const char *k : {"F  X", "Z  C", "ENTER", "ESC  del", "SPACE  TAB"})
        check(all.count(settings + ": " + k) == 1, std::string("the keys in settings include ") + k);
    // Any key wakes a screen that has gone off (and does nothing else), and the guide says so.
    bool wakes = false;
    for (const keyguide::Row &r : sections[1].rows) wakes = wakes || (r.keys == "any key" && r.action.find("screen") != std::string::npos);
    check(wakes, "the keys that work everywhere say that any key turns the screen back on");
    // TAB twice still opens settings, and the guide says so beside SPACE.
    for (const keyguide::Row &r : sections.front().rows)
        if (r.keys == "SPACE") check(r.action.find("TAB twice") != std::string::npos, "SPACE's line mentions TAB twice");
}

// DIAGNOSTICS (temporary, ZB-051): SPACE once opens settings when the window has passed without a
// second press; SPACE twice saves the conversation, and settings never open on the way.
void test_double_tap() {
    const unsigned long w = 450;
    keyguide::DoubleTap tap(w);
    check(!tap.pending() && !tap.single_due(1000), "nothing waits before a press");
    check(!tap.press(1000) && tap.pending(), "a first press waits");
    check(!tap.single_due(1000 + w - 1), "a single press waits out the whole window");
    check(tap.single_due(1000 + w) && !tap.pending(), "then it is acted on (settings open)");
    check(!tap.single_due(1000 + 5 * w), "only once");

    check(!tap.press(5000) && tap.press(5000 + w - 1), "a second press inside the window is a double tap (save)");
    check(!tap.pending() && !tap.single_due(5000 + 10 * w), "and the first is used up: settings do not open as well");

    check(!tap.press(9000) && !tap.press(9000 + w), "a second press at the window's end is a new single press");
    check(tap.single_due(9000 + 2 * w), "which opens settings in its turn");

    check(!tap.press(12000) && tap.press(12100) && !tap.press(12200), "a third quick press starts again");
    check(tap.single_due(12200 + w), "as a single press");

    tap.press(15000);
    check(tap.cancel(), "another key first: the waiting press is handed back, to open settings at once");
    check(!tap.cancel(), "and only once");
    check(!tap.pending() && !tap.single_due(15000 + w), "nothing is left to act on when the window ends");
    check(!tap.press(15100), "and the next press is a first press, not a double tap");

    keyguide::DoubleTap held(w);
    check(!held.press(0) && held.single_due(w), "a press at time zero counts like any other");
}

void test_hints() {
    const std::string ready = keyguide::idle_hint(true), first = keyguide::idle_hint(false);
    for (const char *k : {"TAB talk", "ENTER type", "SPACE settings", "H keys"})
        check(ready.find(k) != std::string::npos, std::string("the bottom row names ") + k);
    check(ready.find("TAB TAB") == std::string::npos, "the bottom row names SPACE for settings, not TAB TAB");
    for (const char *k : {"SPACE settings", "H keys", "hold ESC"})
        check(first.find(k) != std::string::npos, std::string("before setup, the bottom row names ") + k);
}

}  // namespace

// A modifier tapped on its own is passed on (as a Key::None press that only wakes the screen); a
// chord is not, nor a modifier held while another key is pressed and let go.
void test_lone_modifier() {
    auto taps = [](std::initializer_list<std::pair<bool, bool>> codes) {   // (modifier, down) in order
        LoneModifier lone;
        int n = 0;
        for (const auto &c : codes) n += lone.step(c.first, c.second);
        return n;
    };
    check(taps({{true, true}, {true, false}}) == 1, "fn pressed and let go: one tap");
    check(taps({{true, true}, {false, true}, {false, false}, {true, false}}) == 0, "fn+S: a chord, no tap");
    check(taps({{true, true}, {false, true}, {true, false}, {false, false}}) == 0, "fn let go before S: no tap");
    check(taps({{true, false}}) == 0, "a release with no press (the app started meanwhile): no tap");
    check(taps({{true, true}, {true, true}, {true, false}, {true, false}}) == 1, "shift and fn together, let go: one tap");
    check(taps({{false, true}, {false, false}}) == 0, "a letter: not the reader's tap (it is a key of its own)");
    check(taps({{true, true}, {true, false}, {true, true}, {true, false}}) == 2, "tapped twice: two");
}

int main() {
    test_chat_keys();
    test_guide_matches_keys();
    test_double_tap();
    test_hints();
    test_lone_modifier();
    printf("keys: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
