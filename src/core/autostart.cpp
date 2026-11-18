#include "core/autostart.h"

#include <string>
#include <thread>
#include <unistd.h>

#include "core/util.h"
#include "platform/proc.h"

namespace autostart {

namespace {
// The launcher is the user service that owns the screen. Running the app from its
// ExecStartPre means the app has the screen first, and the launcher follows when the app exits.
const char *kLauncherUnit = "/usr/lib/systemd/user/APPLaunch.service";

std::string dropin_dir() { return util::home_dir() + "/.config/systemd/user/APPLaunch.service.d"; }
std::string dropin_path() { return dropin_dir() + "/zero-bot.conf"; }

std::string self_path() {
    char buf[512];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    return n > 0 ? std::string(buf, static_cast<size_t>(n)) : "/usr/bin/zero_bot";
}
}  // namespace

bool supported() { return access(kLauncherUnit, R_OK) == 0; }

bool enabled() { return access(dropin_path().c_str(), R_OK) == 0; }

bool set(bool on) {
    if (on) {
        // "-" lets the launcher start even if the app fails; no start timeout because the app
        // stays open for as long as the user wants it.
        const std::string text = "# Written by Zero Bot (Settings > Start at power-on).\n[Service]\nExecStartPre=-" +
                                 self_path() + " --boot\nTimeoutStartSec=infinity\n";
        if (!util::write_file_atomic(dropin_path(), text, 0644)) return false;
    } else if (unlink(dropin_path().c_str()) != 0 && enabled()) {
        return false;
    }
    // The setting is the file above, written by now; the reload that tells the service manager
    // can take seconds and is called from a settings row on the UI thread, so it runs beside it.
    std::thread([] { proc::run({"systemctl", "--user", "daemon-reload"}, "", 8000); }).detach();
    return true;
}

}  // namespace autostart
