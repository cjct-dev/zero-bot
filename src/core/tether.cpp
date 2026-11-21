#include "core/tether.h"

#include <vector>

#include "core/util.h"
#include "platform/net_status.h"
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

// The first default route, whether or not it goes through a gateway: a VPN or PPP link with none still
// carries the app's requests (discovery, which identifies the network by its router, asks for one with).
std::string default_link() { return net_status::default_route(false).iface; }

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
