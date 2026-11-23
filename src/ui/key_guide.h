// The keys: what each one does on the chat screen, and the guide that lists them all (H, or
// Settings → Keys). No screen code here, so it is tested without one (tests/unit/key_guide_test.cpp).
#pragma once
#include <string>
#include <vector>

#include "platform/keys.h"

namespace keyguide {

// One line of the guide: the keys as printed on the keyboard, and what they do.
struct Row {
    std::string keys, action;
};
struct Section {
    std::string title;
    std::vector<Row> rows;
};

// Every key action, in the order the guide shows them. The last section is the keys in settings.
// Opened from the first-run setup guide, that section says how to close the guide instead: SPACE
// does not close it there (a new user presses it to choose), only TAB does.
const std::vector<Section> &sections(bool in_setup_guide = false);

// What a key does on the chat screen when nothing is being typed or recorded. TAB, ESC and the
// fn layer's control keys are handled before this is asked; a release does nothing here.
enum class ChatKey { None, Settings, Guide, ScrollUp, ScrollDown, PageUp, PageDown, Type, DeleteTap, TypeHint,
                     // Q, W, E, A, S, D: what fn+Q, fn+W, fn+E, fn+A, fn+S and fn+D do
                     PlayPause, PreviousTrack, NextTrack, Mute, VolumeDown, VolumeUp };
ChatKey chat_key(const KeyEvent &ev);

bool is_guide_key(const KeyEvent &ev);      // H or ?, pressed (in settings too, outside a text field)
bool is_settings_key(const KeyEvent &ev);   // SPACE, pressed: opens settings, and closes them again

// DIAGNOSTICS (temporary, ZB-051; see core/activity.h): on the chat screen SPACE once opens
// settings and SPACE twice saves the conversation. The single press is acted on once the window
// has passed without a second, or at once when another key comes first (cancel), so a double tap
// never opens settings on the way and "SPACE, Down" still reaches the settings. The times
// are given (milliseconds from any steady clock), so this is tested without a screen.
class DoubleTap {
public:
    explicit DoubleTap(unsigned long window_ms) : window_(window_ms) {}
    // A press at `now`: true when it is the second of a double tap (the first is then used up).
    bool press(unsigned long now);
    // True, once, when a single press has waited out the window by `now`.
    bool single_due(unsigned long now);
    // Another key came first: no second press can follow now. True when a single press was
    // waiting (it is then the caller's to act on at once); either way nothing waits after this.
    bool cancel() {
        const bool was = pending_;
        pending_ = false;
        return was;
    }
    bool pending() const { return pending_; }

private:
    unsigned long window_, at_ = 0;
    bool pending_ = false;
};

// The bottom row of the chat screen while nothing is happening: the keys most wanted, and the one
// that shows the rest.
std::string idle_hint(bool configured);

}  // namespace keyguide
