// What the system says about the network right now: the Wi-Fi network joined (NetworkManager's
// list) and the default route (/proc/net/route). Read-only; nothing here changes the network.
#pragma once
#include <string>

namespace net_status {

// The Wi-Fi network in use, from `nmcli -t -f ACTIVE,SSID,SIGNAL dev wifi list --rescan no`: the
// list NetworkManager already has (asking it to scan again takes seconds, and on this radio a scan
// also interrupts Bluetooth audio). When several rows are active, the last one counts.
struct WiFi {
    bool connected = false;     // a row starts "yes:"
    std::string entry;          // that row after "yes:", as nmcli prints it: "SSID:SIGNAL" (a ':' in the SSID is "\:")
    std::string ssid, signal;   // the row split at every ':', fields 1 and 2 ("" when missing)
    bool has_signal = false;    // the row has a field 2
};
WiFi parse_wifi(const std::string &nmcli_out);
WiFi wifi(int timeout_ms);      // runs nmcli (a subprocess: not on the UI thread)

// The default route (destination 0.0.0.0) in `table`, in /proc/net/route's format: the first such
// row, or with `via_gateway` the first one flagged as going through a gateway (RTF_GATEWAY), with
// that gateway's address. Both empty when there is none. The two differ for a default route with no
// gateway (a VPN or PPP link): tether::default_link takes it (the app is online), discovery does not
// (no router to identify the network by).
struct Route {
    std::string iface;     // "wlan0", "bnep0", ...
    std::string gateway;   // dotted IPv4 address; only with via_gateway
};
Route default_route(bool via_gateway, const std::string &table = "/proc/net/route");

}  // namespace net_status
