// The battery, as the fuel gauge reports it (bq27220, /sys/class/power_supply/bq27220-0/). There is
// none on a PC: the reading there is "unknown, not charging".
#pragma once
#include <string>

namespace power {

struct Battery {
    int percent = -1;        // charge left, 0 to 100; -1 when the gauge could not be read
    bool charging = false;   // the gauge says "Charging" (on USB and not yet full)
    // The gauge says "Discharging": on battery. On USB it says "Charging", or "Not charging" or "Full" once full.
    bool discharging = false;
};

// Reads the gauge's capacity and status files under `gauge` (the device's by default; a test
// passes a folder of its own). Two small file reads, cheap enough for the UI thread once a second.
Battery battery(const std::string &gauge = "/sys/class/power_supply/bq27220-0");

}  // namespace power
