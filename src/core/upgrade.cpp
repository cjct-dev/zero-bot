#include "core/upgrade.h"

#include <fstream>
#include <thread>
#include <unistd.h>

#include "core/util.h"
#include "platform/proc.h"

namespace upgrade {

bool remove_power_on_dropin(const std::string &home) {
    if (home.empty()) return false;
    const std::string dir = home + "/.config/systemd/user/APPLaunch.service.d";
    const std::string path = dir + "/zero-bot.conf";
    std::string first;
    {
        std::ifstream in(path);
        if (!in || !std::getline(in, first)) return false;
    }
    if (first.rfind("# Written by Zero Bot", 0) != 0) return false;   // someone else's file of that name
    if (unlink(path.c_str()) != 0) return false;
    rmdir(dir.c_str());   // only succeeds when nothing else is in it
    return true;
}

void tidy_at_start() {
    if (!remove_power_on_dropin(util::home_dir())) return;
    // Tells the launcher's service manager the drop-in is gone; it can take seconds.
    std::thread([] { proc::run({"systemctl", "--user", "daemon-reload"}, "", 8000); }).detach();
}

}  // namespace upgrade
