// Infrared remote control: built-in TV codes for common brands plus learn-and-replay.
#pragma once
#include <atomic>
#include <string>
#include <vector>

namespace ir {

// Brands with built-in codes ("auto" sends the key for every brand in turn).
std::vector<std::string> brands();
// Keys understood for built-in brands: power, volume_up, volume_down, mute, channel_up,
// channel_down, input, up, down, left, right, ok, menu, back, home, 0..9.
std::vector<std::string> keys();

// Sends `key` `repeat` times (at most kMaxRepeat). Learned buttons (see learn) take priority over
// built-in codes. Each press takes about 0.1 s, or about 0.5 s with "auto" (every brand in turn);
// setting `cancel` stops between presses. Returns "" on success or a short error message.
constexpr int kMaxRepeat = 30;
std::string send(const std::string &brand, const std::string &key, int repeat = 1,
                 const std::atomic<bool> *cancel = nullptr);

// Captures one button press from a real remote pointed at the receiver and stores it
// under `name`. Blocks up to timeout_ms. Returns "" on success or an error message.
std::string learn(const std::string &name, int timeout_ms = 8000);
std::vector<std::string> learned();
bool forget(const std::string &name);
void forget_all();

}  // namespace ir
