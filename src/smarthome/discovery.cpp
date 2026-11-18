#include "smarthome/discovery.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cstring>
#include <fstream>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <set>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

#include "core/util.h"
#include "platform/proc.h"
#include "platform/display.h"

namespace discovery {

namespace {

// Service types asked for by name. Only kinds of device a smart-home app would list: asking for
// everything would also turn up printers, file servers and people's laptops.
struct ServiceKind { const char *type; const char *kind; };
const ServiceKind kServices[] = {
    {"_hue._tcp.local", "hue_bridge"},        {"_hap._tcp.local", "homekit"},
    {"_matter._tcp.local", "matter"},         {"_matterc._udp.local", "matter"},
    {"_shelly._tcp.local", "shelly"},         {"_wled._tcp.local", "wled"},
    {"_esphomelib._tcp.local", "esphome"},    {"_googlecast._tcp.local", "cast"},
    {"_amzn-wplay._tcp.local", "fire_tv"},    {"_androidtvremote2._tcp.local", "android_tv"},
    {"_sonos._tcp.local", "sonos"},           {"_elg._tcp.local", "elgato"},
    {"_nanoleafapi._tcp.local", "nanoleaf"},  {"_tplink._tcp.local", "tplink"},
    {"_airplay._tcp.local", "airplay"},       // asked for only because it carries a TV's friendly name
};

// Most specific first: a device seen several ways is listed as the first of these it matches.
// Televisions and speakers come before HomeKit, which many of them also announce (for AirPlay 2
// and Apple Home): the HomeKit driver refuses those categories, and pairing one over HomeKit
// would use up its only HomeKit pairing for nothing. Nanoleaf stays after HomeKit while there is
// no Nanoleaf driver.
const char *const kKindOrder[] = {"tapo",    "hue_bridge", "sonos",   "fire_tv", "android_tv", "roku",   "cast",
                                  "homekit", "shelly",     "wled",    "esphome", "elgato",     "nanoleaf", "wemo",
                                  "matter",  "tplink",     "airplay"};

// TP-Link's own discovery query (what the Tapo app broadcasts): a 16-byte header with no body.
const unsigned char kTapoQuery[] = {0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x00, 0x46, 0x3c, 0xb5, 0xd3};

constexpr size_t kMaxPacket = 9000;
constexpr int kMaxRecords = 200;

// Text from the network is whatever its owner typed: keep it printable and short.
std::string clean(const std::string &raw, size_t max = 40) {
    std::string out;
    for (const unsigned char c : util::sanitize_utf8(raw))
        if (c >= 0x20 && c != 0x7f) out += static_cast<char>(c);
    return util::truncate_utf8(util::trim(out), max);
}

std::string lower_mac(std::string mac) {
    for (char &c : mac) c = c == '-' ? ':' : static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return mac;
}

uint64_t fnv1a(const std::string &s) {
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

// ---------------------------------------------------------------- DNS
// Reads a possibly compressed name. Returns false on anything malformed.
bool read_name(const unsigned char *buf, size_t len, size_t &off, std::string &out) {
    out.clear();
    size_t pos = off;
    bool jumped = false;
    for (int hops = 0; hops < 16; ) {
        if (pos >= len) return false;
        const unsigned char l = buf[pos];
        if (l == 0) { if (!jumped) off = pos + 1; return true; }
        if ((l & 0xC0) == 0xC0) {
            if (pos + 1 >= len) return false;
            if (!jumped) off = pos + 2;
            jumped = true;
            pos = static_cast<size_t>((l & 0x3F) << 8) | buf[pos + 1];
            ++hops;
            continue;
        }
        if (l > 63 || pos + 1 + l > len || out.size() + l > 255) return false;
        if (!out.empty()) out += '.';
        out.append(reinterpret_cast<const char *>(buf + pos + 1), l);
        pos += 1 + static_cast<size_t>(l);
    }
    return false;
}

void put_name(std::string &pkt, const std::string &name) {
    std::istringstream parts(name);
    for (std::string label; std::getline(parts, label, '.');) {
        pkt += static_cast<char>(label.size());
        pkt += label;
    }
    pkt += '\0';
}

std::string mdns_query(bool unicast_reply) {
    std::string pkt(12, '\0');
    const size_t n = sizeof(kServices) / sizeof(kServices[0]);
    pkt[5] = static_cast<char>(n);
    for (const auto &s : kServices) {
        put_name(pkt, s.type);
        pkt += '\0'; pkt += '\x0c';                              // PTR
        pkt += unicast_reply ? '\x80' : '\0'; pkt += '\x01';     // IN, with the "answer me directly" bit
    }
    return pkt;
}

struct Seen {
    // What mDNS told us, accumulated over all packets (records for one service can arrive apart).
    std::set<std::pair<std::string, std::string>> instances;         // (service type, instance name)
    std::map<std::string, std::pair<std::string, int>> srv;          // instance -> (host, port)
    std::map<std::string, std::map<std::string, std::string>> txt;   // instance -> key/value
    std::map<std::string, std::string> a;                            // host -> IPv4
    std::map<std::string, std::string> from;                         // instance -> address the answer came from
    std::map<std::string, json> tapo;                                // address -> discovery result
    std::map<std::string, std::map<std::string, std::string>> ssdp;  // address -> headers
    std::map<std::string, std::map<std::string, std::string>> dial;  // address -> headers of its DIAL answer
};

const char kDialService[] = "urn:dial-multiscreen-org:service:dial:1";

void parse_mdns(const unsigned char *buf, size_t len, const std::string &src, Seen &seen) {
    if (len < 12 || len > kMaxPacket) return;
    if (!(buf[2] & 0x80)) return;                                    // a query, not an answer
    const int qd = (buf[4] << 8) | buf[5];
    const int rr = ((buf[6] << 8) | buf[7]) + ((buf[8] << 8) | buf[9]) + ((buf[10] << 8) | buf[11]);
    size_t off = 12;
    std::string name;
    for (int i = 0; i < qd && i < kMaxRecords; ++i) {
        if (!read_name(buf, len, off, name) || off + 4 > len) return;
        off += 4;
    }
    for (int i = 0; i < rr && i < kMaxRecords; ++i) {
        if (!read_name(buf, len, off, name) || off + 10 > len) return;
        const int type = (buf[off] << 8) | buf[off + 1];
        const size_t rdlen = static_cast<size_t>((buf[off + 8] << 8) | buf[off + 9]);
        off += 10;
        if (off + rdlen > len) return;
        const size_t rd = off;
        off += rdlen;
        const std::string lname = util::lower(name);
        if (type == 12) {                                            // PTR: service type -> instance
            size_t p = rd;
            std::string target;
            if (!read_name(buf, len, p, target)) continue;
            for (const auto &s : kServices)
                if (lname == s.type) { seen.instances.insert({s.type, target}); seen.from[target] = src; }
        } else if (type == 33 && rdlen >= 7) {                       // SRV: instance -> host and port
            size_t p = rd + 6;
            std::string host;
            if (!read_name(buf, len, p, host)) continue;
            seen.srv[name] = {util::lower(host), (buf[rd + 4] << 8) | buf[rd + 5]};
            if (!seen.from.count(name)) seen.from[name] = src;
        } else if (type == 16) {                                     // TXT: key=value strings
            auto &kv = seen.txt[name];
            for (size_t p = rd; p < rd + rdlen && kv.size() < 40;) {
                const size_t l = buf[p];
                if (p + 1 + l > rd + rdlen) break;
                const std::string item(reinterpret_cast<const char *>(buf + p + 1), l);
                const size_t eq = item.find('=');
                if (eq != std::string::npos && eq > 0) kv[util::lower(item.substr(0, eq))] = clean(item.substr(eq + 1), 120);
                p += 1 + l;
            }
        } else if (type == 1 && rdlen == 4) {                        // A: host -> address
            char ip[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, buf + rd, ip, sizeof(ip))) seen.a[lname] = ip;
        }
    }
}

void parse_tapo(const unsigned char *buf, size_t len, const std::string &src, Seen &seen) {
    if (len <= 16 || len > kMaxPacket) return;
    const json j = json::parse(buf + 16, buf + len, nullptr, false);
    if (!j.is_object() || !j.contains("result") || !j["result"].is_object()) return;
    seen.tapo[src] = j["result"];
}

void parse_ssdp(const char *buf, size_t len, const std::string &src, Seen &seen) {
    if (len > kMaxPacket) return;
    std::istringstream lines(std::string(buf, len));
    std::map<std::string, std::string> h;
    for (std::string line; std::getline(lines, line) && h.size() < 30;) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        h[util::lower(util::trim(line.substr(0, colon)))] = clean(line.substr(colon + 1), 160);
    }
    if (h.empty()) return;
    // An answer to the DIAL search is kept apart: a TV usually answers ssdp:all first, and only the
    // DIAL answer carries its wake-up address.
    const auto st = h.find("st"), usn = h.find("usn");
    const std::string tail = std::string("::") + kDialService;
    const bool dial = (st != h.end() && util::lower(st->second) == kDialService) ||
                      (usn != h.end() && usn->second.size() >= tail.size() &&
                       util::lower(usn->second.substr(usn->second.size() - tail.size())) == tail);
    auto &into = dial ? seen.dial : seen.ssdp;
    if (!into.count(src)) into[src] = h;
}

// ---------------------------------------------------------------- sockets
int udp_socket(const in_addr &local, int port, bool reuse) {
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    const int one = 1;
    if (reuse) {
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    }
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &local, sizeof(local));   // never out of another interface
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr = reuse ? in_addr{htonl(INADDR_ANY)} : local;
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) { close(fd); return -1; }
    return fd;
}

void send_to(int fd, const void *data, size_t len, const char *ip, int port) {
    if (fd < 0) return;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, ip, &to.sin_addr);
    sendto(fd, data, len, 0, reinterpret_cast<sockaddr *>(&to), sizeof(to));
}

int rank_of(const std::string &kind) {
    const int n = sizeof(kKindOrder) / sizeof(kKindOrder[0]);
    for (int i = 0; i < n; ++i)
        if (kind == kKindOrder[i]) return i;
    return n;
}

// Turns everything heard into one entry per address.
std::vector<Device> merge(const Seen &seen) {
    std::map<std::string, Device> by_ip;
    std::map<std::string, std::pair<std::string, std::string>> friendly;   // address -> (name, model) from AirPlay
    auto take = [&](const std::string &ip, const std::string &kind, const char *method) -> Device & {
        Device &d = by_ip[ip];
        d.ip = ip;
        if (d.kind.empty() || rank_of(kind) < rank_of(d.kind)) d.kind = kind;
        if (std::find(d.found_by.begin(), d.found_by.end(), method) == d.found_by.end()) d.found_by.push_back(method);
        return d;
    };

    for (const auto &inst : seen.instances) {
        const std::string &type = inst.first, &name = inst.second;
        std::string kind = "other";
        for (const auto &s : kServices)
            if (type == s.type) kind = s.kind;
        std::string host, ip;
        int port = 0;
        if (const auto it = seen.srv.find(name); it != seen.srv.end()) { host = it->second.first; port = it->second.second; }
        if (const auto it = seen.a.find(host); it != seen.a.end()) ip = it->second;
        if (ip.empty())
            if (const auto it = seen.from.find(name); it != seen.from.end()) ip = it->second;
        if (ip.empty()) continue;
        std::map<std::string, std::string> txt;
        if (const auto it = seen.txt.find(name); it != seen.txt.end()) txt = it->second;
        // The instance name is "Name._type._tcp.local": the part before the service type is the label.
        const size_t cut0 = util::lower(name).find("." + type);
        if (kind == "airplay") {
            // Not a kind of its own here: it only lends its readable name and model to the device.
            friendly[ip] = {clean(cut0 == std::string::npos ? name : name.substr(0, cut0)), clean(txt.count("model") ? txt["model"] : "")};
            continue;
        }
        // A speaker also announces the groups it belongs to; the speaker itself is the device.
        const bool group = kind == "cast" && txt.count("md") && txt["md"] == "Google Cast Group";
        const bool lead = by_ip[ip].kind.empty() || rank_of(kind) < rank_of(by_ip[ip].kind) ||
                          (by_ip[ip].model == "Google Cast Group" && !group);
        Device &d = take(ip, kind, "mdns");
        if (!lead && !d.name.empty()) continue;   // a less specific service of a device already described
        d.host = host;
        d.port = port;
        std::string label = cut0 == std::string::npos ? name : name.substr(0, cut0);
        if (kind == "cast" && txt.count("fn")) label = txt["fn"];
        d.name = clean(label);
        if (txt.count("md")) d.model = clean(txt["md"]);
        for (const char *key : {"id", "sf", "ci", "bridgeid", "modelid", "ve"})
            if (txt.count(key)) d.info[key] = txt[key];
        if (kind == "homekit") {
            // ff: pair with the authentication chip's method when bit 0 is set; c#: goes up when
            // the accessory's database changes; pv: the protocol version; md: the model, by which
            // the HomeKit driver recognises a device that has a driver of its own (a Hue Bridge
            // whose own announcement was not heard).
            for (const char *key : {"ff", "c#", "pv", "md"})
                if (txt.count(key)) d.info[key] = txt[key];
        }
        if (kind == "fire_tv") {
            // The SRV port is Amazon's own discovery service, not the remote-control API the driver
            // uses (it knows that port), so it is not kept as the device's port. "c" is the TV's
            // wake-up address on newer Fire OS, "n" the name its owner gave it.
            d.port = 0;
            for (const char *key : {"c", "n"})
                if (txt.count(key)) d.info[key] = txt[key];
        }
        if (txt.count("bridgeid")) d.id = "hue:" + util::lower(txt["bridgeid"]);
        else if (txt.count("id")) d.id = kind + ":" + util::lower(txt["id"]);
    }

    for (const auto &t : seen.tapo) {
        const json &r = t.second;
        Device &d = take(t.first, "tapo", "tapo");
        d.model = clean(util::jget(r, "device_model", std::string()));
        d.name = "Tapo " + d.model;
        d.id = "tapo:" + util::lower(util::jget(r, "device_id", std::string()));
        d.mac = lower_mac(util::jget(r, "mac", std::string()));
        d.info["device_type"] = clean(util::jget(r, "device_type", std::string()));
        d.info["owner"] = clean(util::jget(r, "owner", std::string()), 64);
        if (r.contains("mgt_encrypt_schm") && r["mgt_encrypt_schm"].is_object()) {
            const json &enc = r["mgt_encrypt_schm"];
            d.info["scheme"] = clean(util::jget(enc, "encrypt_type", std::string()));
            d.port = util::jget(enc, "http_port", 80);
        }
        if (r.contains("tpap") && r["tpap"].is_object()) d.info["tpap"] = r["tpap"].dump();
    }

    for (const auto &s : seen.ssdp) {
        const auto &h = s.second;
        auto get = [&](const char *k) { const auto it = h.find(k); return it == h.end() ? std::string() : it->second; };
        const std::string server = util::lower(get("server"));
        std::string kind;
        if (!get("hue-bridgeid").empty()) kind = "hue_bridge";
        else if (server.find("sonos") != std::string::npos) kind = "sonos";
        else if (server.find("roku") != std::string::npos) kind = "roku";
        else if (server.find("wemo") != std::string::npos || server.find("belkin") != std::string::npos) kind = "wemo";
        if (kind.empty()) {
            // Routers, printers and the like: only worth noting when something else found the device.
            if (const auto it = by_ip.find(s.first); it != by_ip.end())
                if (std::find(it->second.found_by.begin(), it->second.found_by.end(), "ssdp") == it->second.found_by.end())
                    it->second.found_by.push_back("ssdp");
            continue;
        }
        const auto before = by_ip.find(s.first);
        const bool had_hue = before != by_ip.end() && before->second.kind == "hue_bridge";
        Device &d = take(s.first, kind, "ssdp");
        // A Hue Bridge's port is the one its own _hue._tcp record gives (else the default, 0). The
        // port of another service heard from it (HomeKit's 8080) is not the bridge's interface.
        if (kind == "hue_bridge" && !had_hue) d.port = 0;
        if (d.name.empty()) d.name = kind == "hue_bridge" ? "Hue Bridge" : clean(get("server"));
        if (!get("hue-bridgeid").empty()) { d.id = "hue:" + util::lower(get("hue-bridgeid")); d.info["bridgeid"] = util::lower(get("hue-bridgeid")); }
    }

    // Many televisions answer the DIAL search, so it never decides what a device is; it only adds
    // the wake-up address to a Fire TV that announced itself over mDNS from the same address.
    for (const auto &s : seen.dial) {
        const auto it = by_ip.find(s.first);
        if (it == by_ip.end() || it->second.kind != "fire_tv") continue;
        Device &d = it->second;
        if (const auto w = s.second.find("wakeup"); w != s.second.end() && !w->second.empty()) d.info["wakeup"] = w->second;
        if (std::find(d.found_by.begin(), d.found_by.end(), "ssdp") == d.found_by.end()) d.found_by.push_back("ssdp");
    }

    std::vector<Device> out;
    for (auto &entry : by_ip) {
        Device &d = entry.second;
        if (d.kind == "fire_tv" || d.kind == "android_tv" || d.kind == "matter" || d.kind == "roku") {
            // These announce themselves under serial-number-like names; use the readable one.
            if (const auto it = friendly.find(d.ip); it != friendly.end()) {
                if (!it->second.first.empty()) d.name = it->second.first;
                if (d.model.empty()) d.model = it->second.second;
            } else if (d.kind == "fire_tv") d.name = d.info.count("n") && !d.info["n"].empty() ? d.info["n"] : "Fire TV";
        }
        if (d.mac.empty()) d.mac = mac_of(d.ip);
        if (d.id.empty()) d.id = d.kind + ":" + (d.mac.empty() ? d.ip : d.mac);
        if (d.name.empty()) d.name = d.kind;
        out.push_back(d);
    }
    std::sort(out.begin(), out.end(), [](const Device &a, const Device &b) {
        return rank_of(a.kind) != rank_of(b.kind) ? rank_of(a.kind) < rank_of(b.kind) : a.name < b.name;
    });
    return out;
}

}  // namespace

std::vector<Device> devices_from(const std::vector<Packet> &packets) {
    Seen seen;
    for (const auto &p : packets) {
        const auto *bytes = reinterpret_cast<const unsigned char *>(p.bytes.data());
        if (p.proto == "mdns") parse_mdns(bytes, p.bytes.size(), p.src, seen);
        else if (p.proto == "ssdp") parse_ssdp(p.bytes.data(), p.bytes.size(), p.src, seen);
        else if (p.proto == "tapo") parse_tapo(bytes, p.bytes.size(), p.src, seen);
    }
    return merge(seen);
}

std::string mac_of(const std::string &ip) {
    std::ifstream arp("/proc/net/arp");
    std::string line;
    std::getline(arp, line);   // header
    while (std::getline(arp, line)) {
        std::istringstream cols(line);
        std::string addr, hw, flags, mac;
        cols >> addr >> hw >> flags >> mac;
        if (addr == ip && flags != "0x0" && mac != "00:00:00:00:00:00") return lower_mac(mac);
    }
    return "";
}

std::string Network::id() const {
    char buf[32];
    snprintf(buf, sizeof(buf), "net-%016llx", static_cast<unsigned long long>(fnv1a(ssid + "|" + gateway_mac)));
    return buf;
}

std::string Network::label() const { return ssid.empty() ? "Wired network" : ssid; }

Route default_route() {
    Route r;
    std::ifstream route("/proc/net/route");
    std::string line;
    std::getline(route, line);
    while (std::getline(route, line)) {
        std::istringstream cols(line);
        std::string iface, dest, gw;
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

Link link_kind(const std::string &iface, std::string &why_not, const std::string &sys) {
    const std::string dir = sys + "/" + iface;
    auto exists = [](const std::string &path) { return access(path.c_str(), F_OK) == 0; };
    // Wi-Fi: a wireless device, whatever it is called.
    if (exists(dir + "/phy80211") || exists(dir + "/wireless") || iface.compare(0, 2, "wl") == 0) return Link::WiFi;
    std::string type;
    if (util::read_file(dir + "/type", type) && util::trim(type) != "1") {   // not Ethernet framing: a VPN, PPP, ...
        why_not = "The internet connection is not Wi-Fi (" + iface + "), so there is no home network to look at.";
        return Link::Other;
    }
    // The drivers of USB network links that phones and mobile modems use; a USB Ethernet adapter
    // has a driver of its own (r8152, ax88179_178a, lan78xx, ...).
    char target[512];
    const ssize_t n = readlink((dir + "/device/driver").c_str(), target, sizeof(target) - 1);
    if (n > 0) {
        target[n] = 0;
        const char *slash = strrchr(target, '/');
        const std::string driver = slash ? slash + 1 : target;
        for (const char *tether : {"ipheth", "rndis_host", "cdc_ether", "cdc_ncm", "cdc_eem", "cdc_mbim", "cdc_subset",
                                   "huawei_cdc_ncm", "qmi_wwan"})
            if (driver == tether) {
                why_not = "The internet connection is a USB link (" + iface + "), such as a phone's, so there is no home network "
                          "to look at.";
                return Link::Other;
            }
    }
    if (iface.compare(0, 2, "en") == 0 || iface.compare(0, 3, "eth") == 0) return Link::Wired;
    // A phone's Bluetooth tether, a VPN or a USB link is somebody else's network or no network at all.
    why_not = "The internet connection is not Wi-Fi (" + iface + "), so there is no home network to look at.";
    return Link::Other;
}

Network current_network() {
    Network net;
    const Route route = default_route();
    net.iface = route.iface;
    net.gateway = route.gateway;
    if (net.iface.empty()) { net.why_not = "There is no network connection."; return net; }
    // A phone's Bluetooth tether, a VPN or a USB link is somebody else's network or no network at
    // all: only Wi-Fi and Ethernet count as "the network this device is on".
    const Link link = link_kind(net.iface, net.why_not);
    if (link == Link::Other) return net;
    const bool wifi = link == Link::WiFi;

    ifaddrs *list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs *i = list; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || net.iface != i->ifa_name) continue;
            const in_addr addr = reinterpret_cast<sockaddr_in *>(i->ifa_addr)->sin_addr;
            const in_addr mask = i->ifa_netmask ? reinterpret_cast<sockaddr_in *>(i->ifa_netmask)->sin_addr : in_addr{htonl(0xffffff00)};
            in_addr bcast;
            bcast.s_addr = addr.s_addr | ~mask.s_addr;
            char buf[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &addr, buf, sizeof(buf))) net.address = buf;
            if (inet_ntop(AF_INET, &bcast, buf, sizeof(buf))) net.broadcast = buf;
            break;
        }
        freeifaddrs(list);
    }
    if (net.address.empty()) { net.why_not = "The network connection has no address yet."; return net; }

    if (wifi) {
        const auto res = proc::run({"iw", "dev", net.iface, "link"}, "", 3000);
        std::istringstream lines(res.out);
        for (std::string l; std::getline(lines, l);) {
            const size_t at = l.find("SSID: ");
            if (at != std::string::npos) net.ssid = clean(l.substr(at + 6), 32);
        }
    }
    net.gateway_mac = mac_of(net.gateway);
    if (net.gateway_mac.empty()) {
        // The system learns the router's hardware address the first time it sends it anything.
        in_addr local;
        inet_pton(AF_INET, net.address.c_str(), &local);
        const int fd = udp_socket(local, 0, false);
        send_to(fd, "", 0, net.gateway.c_str(), 9);
        usleep(300 * 1000);
        if (fd >= 0) close(fd);
        net.gateway_mac = mac_of(net.gateway);
    }
    if (net.gateway_mac.empty()) { net.why_not = "The router could not be identified. Try again in a moment."; return net; }
    net.usable = true;
    return net;
}

bool scan(const Options &opt, std::vector<Device> &out, std::string &err, bool *heard_self) {
    out.clear();
    if (heard_self) *heard_self = false;
    const Network net = current_network();
    if (!net.usable) { err = net.why_not; return false; }
    in_addr local;
    inet_pton(AF_INET, net.address.c_str(), &local);

    // mDNS gets two sockets: the well-known port, which hears answers sent to the whole group,
    // and a private port, which devices answer directly. Each covers a case the other misses.
    const int m_group = udp_socket(local, 5353, true);
    const int m_direct = udp_socket(local, 0, false);
    const int ssdp = udp_socket(local, 0, false);
    const int tapo = udp_socket(local, 0, false);
    if (m_direct < 0 && tapo < 0) { err = "Could not open a network socket."; return false; }
    if (m_group >= 0) {
        ip_mreq join{};
        inet_pton(AF_INET, "224.0.0.251", &join.imr_multiaddr);
        join.imr_interface = local;
        setsockopt(m_group, IPPROTO_IP, IP_ADD_MEMBERSHIP, &join, sizeof(join));
    }
    for (const int fd : {m_group, m_direct}) {
        if (fd < 0) continue;
        const int ttl = 255;
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    }
    if (ssdp >= 0) { const int ttl = 2; setsockopt(ssdp, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)); }

    const std::string q_group = mdns_query(false), q_direct = mdns_query(true);
    const std::string search = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ssdp:all\r\n\r\n";
    // The search a Fire TV's own phone app makes: its answer can carry the TV's wake-up address.
    const std::string dial_search = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
                                                "MX: 1\r\nST: ") + kDialService + "\r\n\r\n";
    auto ask_mdns = [&] {
        send_to(m_group, q_group.data(), q_group.size(), "224.0.0.251", 5353);
        send_to(m_direct, q_direct.data(), q_direct.size(), "224.0.0.251", 5353);
    };
    auto ask_tapo = [&] {
        // Some access points pass only one of the two forms of broadcast.
        send_to(tapo, kTapoQuery, sizeof(kTapoQuery), "255.255.255.255", 20002);
        send_to(tapo, kTapoQuery, sizeof(kTapoQuery), net.broadcast.c_str(), 20002);
    };
    auto ask_ssdp = [&] {
        send_to(ssdp, search.data(), search.size(), "239.255.255.250", 1900);
        send_to(ssdp, dial_search.data(), dial_search.size(), "239.255.255.250", 1900);
    };
    struct Step { int at_ms; std::function<void()> run; bool done; };
    std::vector<Step> steps = {{0, ask_mdns, false},    {0, ask_tapo, false},    {0, ask_ssdp, false},
                               {1000, ask_mdns, false}, {1000, ask_tapo, false}, {1500, ask_ssdp, false},
                               {3000, ask_mdns, false}, {3000, ask_tapo, false}};

    Seen seen;
    std::set<std::string> told;
    const unsigned long start = display::now_ms();
    std::vector<unsigned char> buf(kMaxPacket);
    for (;;) {
        const int elapsed = static_cast<int>(display::now_ms() - start);
        if (elapsed >= opt.listen_ms || (opt.cancel && *opt.cancel)) break;
        for (auto &s : steps)
            if (!s.done && elapsed >= s.at_ms) { s.run(); s.done = true; }
        pollfd fds[4] = {{m_group, POLLIN, 0}, {m_direct, POLLIN, 0}, {ssdp, POLLIN, 0}, {tapo, POLLIN, 0}};
        if (poll(fds, 4, 100) <= 0) continue;
        bool news = false;
        for (int i = 0; i < 4; ++i) {
            if (fds[i].fd < 0 || !(fds[i].revents & POLLIN)) continue;
            for (int burst = 0; burst < 64; ++burst) {
                sockaddr_in from{};
                socklen_t flen = sizeof(from);
                const ssize_t n = recvfrom(fds[i].fd, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr *>(&from), &flen);
                if (n <= 0) break;
                char ip[INET_ADDRSTRLEN];
                if (!inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip))) continue;
                const std::string src = ip;
                if (src == net.address) {
                    // Our own question coming back from the group: proof that multicast reaches this device.
                    if (i == 0 && heard_self) *heard_self = true;
                    continue;
                }
                const size_t len = static_cast<size_t>(n);
                if (i <= 1) parse_mdns(buf.data(), len, src, seen);
                else if (i == 2) parse_ssdp(reinterpret_cast<const char *>(buf.data()), len, src, seen);
                else parse_tapo(buf.data(), len, src, seen);
                news = true;
            }
        }
        if (news && opt.on_device)
            for (const Device &d : merge(seen))
                if (told.insert(d.id).second) opt.on_device(d);
    }
    for (const int fd : {m_group, m_direct, ssdp, tapo})
        if (fd >= 0) close(fd);
    out = merge(seen);
    return true;
}

}  // namespace discovery
