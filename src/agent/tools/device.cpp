// The device tool: this device's own settings: volume, brightness, spoken replies, and its status.
#include "agent/tools/common.h"

#include <algorithm>
#include <fstream>

#include "core/settings.h"
#include "platform/audio.h"
#include "platform/display.h"
#include "platform/proc.h"

namespace tools {

namespace {

int read_int_file(const char *path, int fallback) {
    std::ifstream f(path);
    int v;
    return f >> v ? v : fallback;
}

std::string t_device(const json &a, Context &) {
    const std::string setting = util::lower(str_arg(a, "setting"));
    const Settings s = Settings::get();
    if (setting.empty() || setting == "status") {
        json out = {{"volume_percent", s.volume}, {"brightness_percent", s.brightness},
                    {"spoken_replies", s.speak_replies}, {"timezone", s.timezone.empty() ? util::system_timezone() : s.timezone}};
        const int batt = read_int_file("/sys/class/power_supply/bq27220-0/capacity", -1);
        if (batt >= 0) out["battery_percent"] = batt;
        // The list Network Manager already has: asking it to scan again takes seconds, and on this
        // radio a scan also interrupts Bluetooth audio.
        const proc::Result wifi = proc::run({"nmcli", "-t", "-f", "ACTIVE,SSID,SIGNAL", "dev", "wifi", "list", "--rescan", "no"}, "", 4000);
        for (auto &line : util::split(wifi.out, '\n'))
            if (line.compare(0, 4, "yes:") == 0) out["wifi"] = line.substr(4);
        return out.dump(-1, ' ', false, json::error_handler_t::replace);
    }
    double value = 0;
    const bool has_value = num_arg(a, "value", value);
    if (setting == "volume" || setting == "brightness") {
        if (!has_value) return fail("value is required: a number from 0 to 100");
        const int v = clamp_int(value, 0, 100);
        const Settings now = Settings::update([&](Settings &st) {
            if (setting == "volume") st.volume = v;
            else st.brightness = std::max(5, v);
        });
        if (setting == "volume") { if (!dry_run()) audio::set_volume(v); }
        else if (!dry_run()) display::set_brightness(now.brightness);
        return setting + " set to " + std::to_string(setting == "volume" ? now.volume : now.brightness) + "%";
    }
    if (setting == "spoken_replies") {
        // Said up front in "say", the outcome has to be what was asked for, never a toggle.
        bool on;
        if (a.contains("on") && (a["on"].is_boolean() || a["on"].is_string())) on = bool_arg(a, "on", true);
        else if (has_value) on = value != 0;
        else return fail("on is required for spoken_replies: true to speak answers aloud, false for text only");
        Settings::update([on](Settings &st) { st.speak_replies = on; });
        return std::string("spoken replies ") + (on ? "on" : "off");
    }
    return fail("setting must be status, volume, brightness or spoken_replies");
}

}  // namespace

Tool make_device() {
    return {"device", "Adjusting the device",
            "Read or change this device's own settings. setting=status reports battery, Wi-Fi, volume and brightness.",
            obj({{"setting", E("Which setting", {"status", "volume", "brightness", "spoken_replies"})},
                 {"value", N("For volume or brightness: 0 to 100")},
                 {"on", B("For spoken_replies: true to speak answers aloud, false for text only")}}, {"setting"}),
            t_device};
}

}  // namespace tools
