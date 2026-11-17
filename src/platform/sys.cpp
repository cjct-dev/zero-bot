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

}  // namespace display
