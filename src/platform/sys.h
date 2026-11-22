// Small system helpers every build shares, the cli build included (no screen, no sound).
#pragma once

namespace sys {

// Milliseconds on the monotonic clock since the first call in this process: for ticks, timeouts and
// rate limits, never for the time of day (the wall clock can jump while the app runs). One count for
// the whole app, LVGL's tick included (display.cpp).
unsigned long now_ms();

}  // namespace sys
