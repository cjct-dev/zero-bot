#pragma once
#include <string>

namespace display {
enum class Mode { Device, Window, Headless };
constexpr int kWidth = 320;
constexpr int kHeight = 170;

void init(Mode mode);
bool save_png(const std::string &path);      // snapshot of the active screen
void set_brightness(int percent);            // device backlight (5 to 100); no-op elsewhere
void backlight_off();                        // device backlight off until set_brightness; no-op elsewhere
unsigned long now_ms();

// The screen off: drawn black once, then nothing more is drawn or sent to the panel (changes are
// kept, not drawn), and the backlight off. screen_on draws everything as it now is before the
// backlight comes back at `percent` (it sets the brightness when the screen was not off, too).
void screen_off();
void screen_on(int percent);
bool screen_is_off();
unsigned long refreshes();                   // LVGL refresh cycles so far: for the check that none run while off
// For the checks in --shots (headless only; false elsewhere): the last frame drawn for the panel is
// all black, or shows exactly what the screen's objects show now.
bool frame_black();
bool frame_current();
}  // namespace display
