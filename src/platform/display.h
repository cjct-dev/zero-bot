#pragma once
#include <string>

namespace display {
enum class Mode { Device, Window, Headless };
constexpr int kWidth = 320;
constexpr int kHeight = 170;

void init(Mode mode);
bool save_png(const std::string &path);      // snapshot of the active screen
void set_brightness(int percent);            // device backlight; no-op elsewhere
unsigned long now_ms();
}  // namespace display
