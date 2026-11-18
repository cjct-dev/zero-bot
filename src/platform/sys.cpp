// Device helpers that do not depend on the graphics library.
#include <chrono>
#include <fstream>

#include "platform/display.h"

namespace display {

unsigned long now_ms() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return static_cast<unsigned long>(duration_cast<milliseconds>(steady_clock::now() - t0).count());
}

void set_brightness(int percent) {
#if !ZB_DESKTOP
    if (percent < 5) percent = 5;
    if (percent > 100) percent = 100;
    std::ofstream f("/sys/class/backlight/backlight/brightness");
    if (f) f << percent;
#else
    (void)percent;
#endif
}

void backlight_off() {
#if !ZB_DESKTOP
    // 0 switches the PWM backlight off (the launcher's screen was found at 0 too). Not bl_power: the
    // launcher writes only the brightness, so a bl_power left off by an app that crashed would keep
    // the screen dark until a restart.
    std::ofstream f("/sys/class/backlight/backlight/brightness");
    if (f) f << 0;
#endif
}

}  // namespace display
