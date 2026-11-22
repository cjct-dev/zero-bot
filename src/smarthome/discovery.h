// Finds smart-home devices the way their own apps do: by asking the local network who is there.
// Only discovery announcements are used (mDNS/DNS-SD, SSDP, TP-Link's and WiZ's broadcast queries); no
// address is contacted unless it answered one of those, and nothing is logged in to or changed.
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "platform/net_status.h"

namespace discovery {

// The network the device is on right now.
struct Network {
    std::string iface;         // interface holding the default route
    std::string address, broadcast;
    std::string gateway, gateway_mac;
    std::string ssid;          // "" on a wired link
    bool usable = false;       // false on a phone tether or with no network: never scanned
    std::string why_not;       // reason shown to the user when not usable
    // Stable name for "this network": the SSID alone would match every network of that name, and
    // the access point's address changes between mesh nodes, so it is the SSID plus the router.
    std::string id() const;
    std::string label() const;
};
Network current_network();

// The interface and gateway of the default route, the first one through a gateway (a default route
// with none, a VPN or PPP link, has no router to identify the network by): net_status::default_route
// with via_gateway, one read of /proc/net/route, cheap enough to check before every use of a
// remembered Network.
using Route = net_status::Route;
Route default_route();

// What kind of link an interface is, from what the kernel reports about it under `sys`
// (/sys/class/net) and not from its name alone: a phone's USB tether is named like a wired port
// (an iPhone's is eth0, others enx... or usb0), and only its driver tells it apart. `why_not`
// says why a link that is neither Wi-Fi nor Ethernet is not a home network.
enum class Link { WiFi, Wired, Other };
Link link_kind(const std::string &iface, std::string &why_not, const std::string &sys = "/sys/class/net");

// One device, merged from everything that answered from its address.
struct Device {
    std::string id;            // strongest identifier the protocols gave, prefixed by its kind
    std::string kind;          // "tapo", "homekit", "hue_bridge", "cast", "matter", "shelly", ... or "other"
    std::string name, model;
    std::string ip, mac, host;
    int port = 0;
    std::vector<std::string> found_by;           // "mdns", "ssdp", "tapo", "kasa", "wiz"
    std::map<std::string, std::string> info;     // protocol details (TXT keys, encryption scheme, ...)
};

struct Options {
    int listen_ms = 6000;
    // The network the user agreed to have scanned (Network::id()): when set, a scan finding the device
    // on another one refuses to run.
    std::string network_id;
    std::atomic<bool> *cancel = nullptr;
    std::function<void(const Device &)> on_device;   // called as each new device is first seen
};

// Runs one scan on the current network. Returns false with a reason when it cannot (no usable
// network, sockets refused). `heard_self` reports whether this device heard its own multicast
// query, which tells a local firewall apart from a network that isolates its clients.
bool scan(const Options &opt, std::vector<Device> &out, std::string &err, bool *heard_self = nullptr);

// What scan() would report had it heard these datagrams (proto "mdns", "ssdp", "tapo" for TP-Link's
// 20002 answers, "kasa" for the older Kasa protocol's 9999 answers, still scrambled, or "wiz" for
// WiZ's 38899 answers), for tests.
//
// TP-Link devices are routed between two drivers here. A 20002 answer is kind "tapo" (the Tapo
// driver) unless its device_type begins "IOT.": that is a Kasa device moved to the newer login,
// kind "tplink" with info "scheme" naming that login (the Kasa driver, which says it cannot use
// it). A 9999 answer is kind "tplink" with scheme "XOR". Both give the id "tplink:<mac>", so a
// device keeps its id when a firmware update moves it from one to the other.
struct Packet {
    std::string proto, src, bytes;
};
std::vector<Device> devices_from(const std::vector<Packet> &packets);

// The hardware address the system has recorded for an IP it has exchanged packets with.
std::string mac_of(const std::string &ip);

}  // namespace discovery
