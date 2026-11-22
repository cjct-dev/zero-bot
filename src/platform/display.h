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

// The backlight a crash puts back. The launcher does not restore it when an app ends, so an app that
// crashed while the screen was off left it dark (dimmed: at 10 %) until a brightness key or a
// reboot. While `percent` is set (5 to 100), a fatal signal (SIGSEGV, SIGBUS, SIGFPE, SIGILL,
// SIGABRT, SIGTRAP, SIGSYS, SIGQUIT, SIGHUP) writes it to the backlight before the process ends as
// it would have; -1 when the screen is lit as set (nothing to put back). The first call installs
// the handler (not for a signal the app was started with ignored, such as a hang-up under nohup:
// that stays ignored). SIGKILL cannot be caught: after one, the backlight stays as it was.
void restore_after_crash(int percent);
namespace detail {
// The file a crash writes the brightness to: the device's backlight (none on a desktop build); the
// test points it at a file of its own.
void set_backlight_file(const char *path);
}

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
