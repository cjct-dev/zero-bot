#include "platform/net_status.h"

#include <arpa/inet.h>
#include <cstdlib>
#include <fstream>
#include <netinet/in.h>
#include <sstream>

#include "core/util.h"
#include "platform/proc.h"

namespace net_status {

WiFi parse_wifi(const std::string &nmcli_out) {
    WiFi w;
    for (auto &line : util::split(nmcli_out, '\n')) {
        if (line.compare(0, 4, "yes:") != 0) continue;
        w.connected = true;
        w.entry = line.substr(4);
        const auto f = util::split(line, ':');
        w.ssid = f.size() > 1 ? f[1] : "";
        w.has_signal = f.size() > 2;
        w.signal = w.has_signal ? f[2] : "";
    }
    return w;
}

WiFi wifi(int timeout_ms) {
    return parse_wifi(proc::run({"nmcli", "-t", "-f", "ACTIVE,SSID,SIGNAL", "dev", "wifi", "list", "--rescan", "no"}, "",
                                timeout_ms).out);
}

Route default_route(bool via_gateway, const std::string &table) {
    Route r;
    std::ifstream f(table);
    std::string line;
    std::getline(f, line);   // header
    while (std::getline(f, line)) {
        std::istringstream cols(line);
        std::string iface, dest, gw;
        if (!via_gateway) {
            cols >> iface >> dest;
            if (dest == "00000000") { r.iface = iface; break; }
            continue;
        }
        unsigned flags = 0;
        cols >> iface >> dest >> gw >> std::hex >> flags;
        if (dest != "00000000" || !(flags & 0x2)) continue;
        r.iface = iface;
        in_addr a;
        a.s_addr = static_cast<in_addr_t>(strtoul(gw.c_str(), nullptr, 16));   // already in network order
        char ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &a, ip, sizeof(ip))) r.gateway = ip;
        break;
    }
    return r;
}

}  // namespace net_status
