// When the screen dims and when it goes off: the backlight dims after 45 seconds unused, and the
// screen goes off (drawn black, backlight off, nothing drawn) after the minutes set under
// Device > Screen off after, against burn-in. Anything the user should see wakes it. And when a scene
// of the art (ui/art) is in front of the chat: it comes by itself while something plays, after 20
// seconds unused, and keeps the screen on (dimmed) for as long as it is there. No screen code here, so
// it is tested without one (tests/unit/screen_idle_test.cpp); the app does what it says.
#pragma once
#include <string>
#include <vector>

namespace screen {

enum class Light { Lit, Dimmed, Off };   // in order: each is darker than the one before

constexpr unsigned long kDimAfterMs = 45000;   // idle time before the backlight dims
constexpr int kDimPercent = 10;                // the backlight while dimmed
constexpr unsigned long kSceneAfterMs = 20000; // idle time before a scene comes (back) in front of the chat

// The choices for "Screen off after", in minutes; 0 is never. The default is 5.
const std::vector<int> &off_choices();
// The choice after (dir 1) or before (-1) `minutes`, going round; a value that is not one of the
// choices (a hand-edited settings file) moves to the nearest choice in that direction.
int step_off_choice(int minutes, int dir);
std::string off_label(int minutes);            // "Never", "1 minute", "5 minutes"

// Device > Dynamic art (Settings::dynamic_art): "on", "off", or "plugged": only while the battery is not
// discharging (on USB, charging or full).
bool art_allowed(const std::string &mode, bool on_battery);
std::string art_label(const std::string &mode);               // "On", "Off", "When plugged in"
std::string step_art_mode(const std::string &mode, int dir);  // the next choice (dir 1) or the one before (-1), going round

class Idle {
public:
    // A key, a request, an alarm, a reminder: the screen is lit from `now` on. Returns what it was,
    // so that the app can switch the light back on, and a key that only woke the screen does nothing else.
    Light use(unsigned long now);
    // Called every second or so. `busy`: something the user is watching or waiting for (listening,
    // a request or its spoken answer, a timer's alarm, the breathing exercise) counts as use.
    // `off_after_min`: the setting, 0 or less for never. `scene`: a scene is in front of the chat, which
    // keeps the screen from going off (it dims, and stays dimmed). Returns what the screen should be now:
    // it only darkens here, as time passes; only use() lights it again.
    Light update(unsigned long now, bool busy, int off_after_min, bool scene = false);
    Light light() const { return light_; }
    unsigned long unused_for(unsigned long now) const { return now > last_use_ ? now - last_use_ : 0; }

private:
    unsigned long last_use_ = 0;
    Light light_ = Light::Lit;
};

// Whether a scene is in front of the chat now: one plays (`wanted`: something plays and the setting allows
// it), the screen is not off, nothing is in front of the chat (`blocked`: the menu, a text field, the
// microphone, the breathing exercise, a timer's alarm banner), and nothing has been used for kSceneAfterMs
// (a key, a request, a reminder: Idle::use). So any key takes it away at once, and it comes back by itself.
bool scene_in_front(const Idle &idle, unsigned long now, bool wanted, bool blocked);

}  // namespace screen
