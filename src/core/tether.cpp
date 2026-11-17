#include "core/tether.h"

#include <fstream>
#include <sstream>
#include <vector>

#include "core/util.h"
#include "platform/proc.h"

namespace tether {

namespace {
std::vector<std::string> profiles() {
    std::vector<std::string> out;
    const proc::Result res = proc::run({"nmcli", "-t", "-f", "TYPE,NAME", "con", "show"}, "", 5000);
    for (auto &line : util::split(res.out, '\n'))
        if (line.compare(0, 10, "bluetooth:") == 0) out.push_back(line.substr(10));
    return out;
}
}  // namespace

std::string default_link() {
    std::ifstream f("/proc/net/route");
    std::string line;
    std::getline(f, line);  // header
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string iface, dest;
        ss >> iface >> dest;
        if (dest == "00000000") return iface;
    }
    return "";
}

bool online() { return !default_link().empty(); }

bool via_phone() { return default_link().compare(0, 4, "bnep") == 0; }

bool available() { return !profiles().empty(); }

std::string connect() {
    const auto names = profiles();
    if (names.empty()) return "no phone is paired for Bluetooth internet";
    std::string last = "the phone did not answer";
    for (auto &name : names) {
        const proc::Result res = proc::run({"nmcli", "--wait", "20", "con", "up", name}, "", 25000, 2048, true);
        if (res.ok()) return "";
        if (res.out.find("Not authorized") != std::string::npos) last = "this user may not control networking";
    }
    return last + ". Check that Bluetooth tethering is switched on in the phone's hotspot settings.";
}

void disconnect() {
    for (auto &name : profiles()) proc::run({"nmcli", "con", "down", name}, "", 8000);
}

}  // namespace tether
