// TP-Link Kasa devices on the older local protocol (see kasa.h for what a caller needs to know).
//
// Written for this project. TP-Link has never published this protocol; the facts used here (the
// scrambling, the framing, the command and reply shapes, the energy units of the two hardware
// generations, which devices moved to the newer login) are described with their sources in the
// project's notes. No code was copied from any implementation of it.
#include "smarthome/drivers/kasa.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <set>

#include "core/util.h"
#include "smarthome/home.h"

namespace kasa {

const char *const kNewLogin = "this Kasa device moved to TP-Link's newer login";
const char *const kNoAnswer = "the Kasa device did not answer";
const char *const kRefused = "the Kasa device refused the connection";
// "answers" is one of the words that make the app look for the device at a new address.
const char *const kOtherDevice = "another Kasa device now answers at this one's saved address";

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kListMs = 4000;          // enumerate and read
constexpr int kDefaultPort = 9999;
constexpr size_t kMaxSockets = 16;     // the largest strips have six
constexpr size_t kMaxChildId = 64;     // a socket's id is the strip's id and two digits: 42 characters

const char *const kRelay = "system";
const char *const kDimmer = "smartlife.iot.dimmer";
const char *const kBulb = "smartlife.iot.smartbulb.lightingservice";
const char *const kLightStrip = "smartlife.iot.lightStrip";
const char *const kEnergy = "emeter";

const char *const kUnreadable = "the Kasa device's reply could not be understood";
const char *const kNotSupported = "the Kasa device does not support that";
const char *const kCommandRefused = "the Kasa device refused the command";
const char *const kNotThisKind = "Zero Bot cannot control this kind of Kasa device";
const char *const kStripWhole = "this Kasa power strip's sockets are switched one at a time; read the device again in Settings";

// TP-Link's discovery query for UDP 20002, the form that carries no key: a 16-byte header with
// no body (the same bytes the app's discovery broadcasts).
const unsigned char kProbe[16] = {0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0x00, 0x46, 0x3c, 0xb5, 0xd3};

std::atomic<int> g_probe_port{20002};

std::string new_login_error() {
    return std::string(kNewLogin) +
           " (the one Tapo plugs use) with a firmware update, and Zero Bot cannot use that login with Kasa devices";
}

std::string upper(std::string s) {
    for (char &c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string text(const json &j, const char *key) { return util::jget(j, key, std::string()); }

// A whole number from a reply, or `fallback` when the value is missing or not a finite number.
long number(const json &j, const char *key, long fallback) {
    if (!j.is_object()) return fallback;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return fallback;
    const double v = it->get<double>();
    if (!std::isfinite(v) || v < -1e9 || v > 1e9) return fallback;
    return static_cast<long>(std::lround(v));
}

bool flag(const json &j, const char *key) { return number(j, key, 0) == 1; }

int clamp(long v, int lo, int hi) { return static_cast<int>(std::max<long>(lo, std::min<long>(hi, v))); }

// Socket ids go back to the strip inside every command, so only plain ones are used.
bool valid_child_id(const std::string &id) {
    if (id.empty() || id.size() > kMaxChildId) return false;
    return std::all_of(id.begin(), id.end(), [](char c) { return isalnum(static_cast<unsigned char>(c)) != 0; });
}

std::string type_of(const json &sysinfo) {
    std::string t = text(sysinfo, "type");
    if (t.empty()) t = text(sysinfo, "mic_type");
    return upper(util::trim(t));
}

// The range of white a bulb takes, in kelvin, by model (the model name starts with these).
struct WhiteRange {
    const char *model;
    int lo, hi;
};
const WhiteRange kWhiteRanges[] = {
    {"KL120(US)", 2700, 5000}, {"KL120", 2700, 6500}, {"LB120", 2700, 6500}, {"KL125", 2500, 6500},
    {"KL130", 2500, 9000},     {"KL135", 2500, 9000}, {"LB130", 2500, 9000}, {"LB230", 2500, 9000},
    {"KB130", 2500, 9000},     {"KL400", 2500, 9000}, {"KL420", 2500, 9000}, {"KL430", 2500, 9000},
};

void white_range(const std::string &model, int &lo, int &hi) {
    lo = 2700;   // what every white-adjustable model takes
    hi = 5000;
    const std::string m = upper(model);
    for (const auto &r : kWhiteRanges)
        if (m.compare(0, strlen(r.model), r.model) == 0) {
            lo = r.lo;
            hi = r.hi;
            return;
        }
}

bool only_lighting(const home::Change &c) {
    return !c.target_c && !c.mode && !c.position && !c.fan && !c.volume && !c.volume_step && !c.mute && !c.transport &&
           !c.source && !c.key;
}

// ---------------------------------------------------------------- the network
int left_ms(Clock::time_point until) {
    const auto d = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count();
    return d <= 0 ? 0 : static_cast<int>(std::min<long long>(d, 60000));
}

struct Fd {
    int fd = -1;
    ~Fd() {
        if (fd >= 0) close(fd);
    }
};

// Waits for `events` on `fd` until the deadline. False when the time ran out.
bool wait_for(int fd, short events, Clock::time_point until) {
    for (;;) {
        pollfd p{fd, events, 0};
        const int r = poll(&p, 1, left_ms(until));
        if (r > 0) return true;
        if (r == 0) return false;
        if (errno != EINTR) return false;
    }
}

enum class Got { Ok, Late, Closed };

// Reads exactly n bytes onto `into`.
Got read_exact(int fd, size_t n, std::string &into, Clock::time_point until) {
    char buf[4096];
    while (into.size() < n) {
        if (!wait_for(fd, POLLIN, until)) return Got::Late;
        const ssize_t k = recv(fd, buf, std::min(sizeof(buf), n - into.size()), 0);
        if (k > 0) into.append(buf, static_cast<size_t>(k));
        else if (k == 0) return Got::Closed;
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return Got::Closed;
    }
    return Got::Ok;
}

enum class Outcome { Ok, Refused, NoAnswer, Unreadable, BadAddress };

// One request on its own connection: some of these devices close it after one reply.
Outcome exchange(const std::string &ip, int port, const json &request, json &reply, Clock::time_point until) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    if (port <= 0 || port > 65535 || inet_pton(AF_INET, ip.c_str(), &to.sin_addr) != 1) return Outcome::BadAddress;
    to.sin_port = htons(static_cast<uint16_t>(port));
    Fd s;
    s.fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (s.fd < 0) return Outcome::NoAnswer;
    if (connect(s.fd, reinterpret_cast<sockaddr *>(&to), sizeof(to)) != 0) {
        if (errno == ECONNREFUSED) return Outcome::Refused;
        if (errno != EINPROGRESS) return Outcome::NoAnswer;
        if (!wait_for(s.fd, POLLOUT, until)) return Outcome::NoAnswer;
        int soerr = 0;
        socklen_t len = sizeof(soerr);
        if (getsockopt(s.fd, SOL_SOCKET, SO_ERROR, &soerr, &len) != 0) return Outcome::NoAnswer;
        if (soerr == ECONNREFUSED) return Outcome::Refused;
        if (soerr != 0) return Outcome::NoAnswer;
    }
    const std::string out = detail::frame(request.dump());
    for (size_t sent = 0; sent < out.size();) {
        const ssize_t n = send(s.fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if (!wait_for(s.fd, POLLOUT, until)) return Outcome::NoAnswer;
        } else {
            return Outcome::NoAnswer;
        }
    }
    std::string head;
    const Got h = read_exact(s.fd, 4, head, until);
    if (h != Got::Ok) return head.empty() ? Outcome::NoAnswer : (h == Got::Late ? Outcome::NoAnswer : Outcome::Unreadable);
    const uint32_t size = (static_cast<uint32_t>(static_cast<unsigned char>(head[0])) << 24) |
                          (static_cast<uint32_t>(static_cast<unsigned char>(head[1])) << 16) |
                          (static_cast<uint32_t>(static_cast<unsigned char>(head[2])) << 8) |
                          static_cast<uint32_t>(static_cast<unsigned char>(head[3]));
    if (size == 0 || size > detail::kMaxReply) return Outcome::Unreadable;
    std::string body;
    const Got b = read_exact(s.fd, size, body, until);
    if (b == Got::Late) return Outcome::NoAnswer;
    if (b != Got::Ok) return Outcome::Unreadable;
    return detail::parse_reply(detail::decrypt(body), reply) ? Outcome::Ok : Outcome::Unreadable;
}

// After port 9999 refused: asks the device's discovery port whether it now needs the newer
// login. Only its own answer counts (the socket is connected to its address and port); `mac` is
// the MAC that answer gives.
bool probe_says_new_login(const std::string &ip, Clock::time_point until, std::string &mac) {
    const Clock::time_point stop = std::min(until, Clock::now() + std::chrono::milliseconds(700));
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(static_cast<uint16_t>(detail::probe_port()));
    if (inet_pton(AF_INET, ip.c_str(), &to.sin_addr) != 1 || left_ms(stop) == 0) return false;
    Fd s;
    s.fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (s.fd < 0 || connect(s.fd, reinterpret_cast<sockaddr *>(&to), sizeof(to)) != 0) return false;
    if (send(s.fd, kProbe, sizeof(kProbe), MSG_NOSIGNAL) != static_cast<ssize_t>(sizeof(kProbe))) return false;
    std::string buf(9000, '\0');
    while (wait_for(s.fd, POLLIN, stop)) {
        const ssize_t n = recv(s.fd, &buf[0], buf.size(), 0);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n <= 16) return false;   // nothing listens there, or no answer of the expected shape
        return detail::reply_says_new_login(buf.substr(16, static_cast<size_t>(n) - 16), &mac);
    }
    return false;
}

// ---------------------------------------------------------------- energy
// The request for a plug's readings, and the day of the month it asks about.
json energy_query(int &day) {
    const time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    day = local.tm_mday;
    return {{"get_realtime", json::object()},
            {"get_daystat", {{"year", local.tm_year + 1900}, {"month", local.tm_mon + 1}}}};
}

const json &member(const json &j, const char *key) {
    static const json none;
    if (!j.is_object()) return none;
    const auto it = j.find(key);
    return it == j.end() ? none : *it;
}

// ---------------------------------------------------------------- the driver
class KasaDriver : public home::Driver {
public:
    const char *kind() const override { return "tplink"; }

    // "tapo": a Kasa device that moved to the newer login, saved by a version of the app that
    // filed every 20002 reply under Tapo. The Tapo driver no longer claims these.
    bool claims(const std::string &device_kind, const std::map<std::string, std::string> &info) const override {
        if (device_kind != "tplink" && device_kind != "tapo") return false;
        const auto it = info.find("device_type");
        return it != info.end() && detail::switched_type(it->second);
    }

    bool enumerate(home::Device &dev, std::vector<home::Thing> &out, std::string &err) override {
        out.clear();
        const Clock::time_point until = Clock::now() + std::chrono::milliseconds(kListMs);
        json reply;
        if (!ask(dev, {{kRelay, {{"get_sysinfo", json::object()}}}}, reply, until, err)) return false;
        json sysinfo;
        if (!sysinfo_in(dev, reply, sysinfo, err)) return false;
        if (!detail::things_from(sysinfo, out, err)) return false;
        const std::string name = home::clean_name(text(sysinfo, "alias"));
        if (!name.empty()) dev.name = name;
        const std::string model = home::clean_name(text(sysinfo, "model"));
        if (!model.empty()) dev.model = model;
        return true;
    }

    // Asked for particular things, a plug or socket that measures power is also asked for its
    // readings; asked for "all of them", only what is on and how bright.
    bool read(const home::Device &dev, std::vector<home::Thing> &things, std::string &err) override {
        const bool detail = !things.empty();
        if (!detail) things = dev.things;
        const Clock::time_point until = Clock::now() + std::chrono::milliseconds(kListMs);
        auto metered = [](const home::Thing &t) { return (t.caps & home::kSensor) != 0; };
        auto child_of = [](const home::Thing &t) { return util::jget(t.ref, "child", std::string()); };
        const bool own_meter = detail && std::any_of(things.begin(), things.end(), [&](const home::Thing &t) {
            return metered(t) && child_of(t).empty();
        });
        json request = {{kRelay, {{"get_sysinfo", json::object()}}}};
        int day = 0;
        if (own_meter) request[kEnergy] = energy_query(day);
        json reply;
        if (!ask(dev, request, reply, until, err)) return false;
        json sysinfo;
        if (!sysinfo_in(dev, reply, sysinfo, err)) return false;
        for (auto &t : things) {
            detail::state_from(sysinfo, t);
            if (own_meter && metered(t) && child_of(t).empty()) {
                const json &em = member(reply, kEnergy);
                detail::energy_into(member(em, "get_realtime"), member(em, "get_daystat"), day, t.state);
            }
        }
        if (!detail) return true;
        // A strip's sockets each have a meter of their own, asked for one at a time. Readings are
        // extra: running short of time, or a socket that does not answer, still leaves the states.
        std::set<std::string> asked;
        for (auto &t : things) {
            const std::string child = child_of(t);
            if (!metered(t) || !valid_child_id(child) || !asked.insert(child).second || left_ms(until) < 400) continue;
            json q = {{"context", {{"child_ids", json::array({child})}}}, {kEnergy, energy_query(day)}};
            json r;
            std::string ignored;
            if (!ask(dev, q, r, until, ignored)) break;
            const json &em = member(r, kEnergy);
            detail::energy_into(member(em, "get_realtime"), member(em, "get_daystat"), day, t.state);
        }
        return true;
    }

    bool apply(const home::Device &dev, const home::Thing &thing, const home::Change &change, int deadline_ms,
               std::string &err) override {
        json request;
        if (!detail::request_for(thing, change, request, err)) return false;
        return send_change(dev, request, deadline_ms, err);
    }

    // Several sockets of one strip: one request names them all.
    bool apply_many(const home::Device &dev, const std::vector<const home::Thing *> &things, const home::Change &change,
                    int deadline_ms, std::string &err) override {
        json request;
        err.clear();
        if (!detail::request_for_sockets(things, change, request, err)) return false;
        return send_change(dev, request, deadline_ms, err);
    }

private:
    // A Kasa device takes any command from anyone, so before a change the device at the saved
    // address is asked who it is. After the router hands that address to another Kasa device, a
    // plug's "on" would otherwise switch that one instead: every socket of a strip, or a heater
    // whose "Ask before switching on" is never asked because the command was for another thing.
    bool send_change(const home::Device &dev, const json &request, int deadline_ms, std::string &err) const {
        const Clock::time_point until = Clock::now() + std::chrono::milliseconds(std::max(1, deadline_ms));
        json reply;
        if (!detail::mac_digits(dev.mac).empty()) {
            json sysinfo;
            if (!ask(dev, {{kRelay, {{"get_sysinfo", json::object()}}}}, reply, until, err) ||
                !sysinfo_in(dev, reply, sysinfo, err))
                return false;
            // A thing saved without a socket (read once when the strip listed none) is never
            // switched as a whole strip.
            if (!request.contains("context") && sysinfo.contains("children")) {
                err = kStripWhole;
                return false;
            }
            reply = json();
        }
        if (!ask(dev, request, reply, until, err)) return false;
        return detail::reply_ok(request, reply, err);
    }

    // Sends one request. Nothing is sent to a device that discovery said needs the newer login.
    bool ask(const home::Device &dev, const json &request, json &reply, Clock::time_point until, std::string &err) const {
        if (detail::needs_new_login(dev.info)) {
            err = new_login_error();
            return false;
        }
        const int port = detail::port_for(dev);
        switch (exchange(dev.ip, port, request, reply, until)) {
        case Outcome::Ok:
            return true;
        case Outcome::Refused: {
            // The answer may be from another Kasa device that moved to the newer login and now has
            // this one's address: then this one has not moved, and is looked for again.
            std::string mac;
            if (!probe_says_new_login(dev.ip, until, mac)) err = kRefused;
            else if (!detail::mac_digits(dev.mac).empty() && !detail::mac_digits(mac).empty() &&
                     detail::mac_digits(dev.mac) != detail::mac_digits(mac))
                err = kOtherDevice;
            else err = new_login_error();
            return false;
        }
        case Outcome::NoAnswer:
            err = kNoAnswer;
            return false;
        case Outcome::Unreadable:
            err = kUnreadable;
            return false;
        case Outcome::BadAddress:
            err = "the Kasa device's saved address is not usable";
            return false;
        }
        return false;
    }

    // get_sysinfo from a reply, from the device that was saved (see send_change).
    static bool sysinfo_in(const home::Device &dev, const json &reply, json &sysinfo, std::string &err) {
        const json &s = member(member(reply, kRelay), "get_sysinfo");
        if (!s.is_object()) {
            err = kUnreadable;
            return false;
        }
        if (number(s, "err_code", 0) != 0) {
            err = kCommandRefused;
            return false;
        }
        if (!detail::same_device(dev.mac, s)) {
            err = kOtherDevice;
            return false;
        }
        sysinfo = s;
        return true;
    }
};

}  // namespace

// ---------------------------------------------------------------- building blocks
namespace detail {

std::string encrypt(const std::string &plain) {
    std::string out(plain.size(), '\0');
    unsigned char key = 171;
    for (size_t i = 0; i < plain.size(); ++i) {
        key = static_cast<unsigned char>(static_cast<unsigned char>(plain[i]) ^ key);
        out[i] = static_cast<char>(key);
    }
    return out;
}

std::string decrypt(const std::string &cipher) {
    std::string out(cipher.size(), '\0');
    unsigned char key = 171;
    for (size_t i = 0; i < cipher.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(cipher[i]);
        out[i] = static_cast<char>(c ^ key);
        key = c;
    }
    return out;
}

std::string frame(const std::string &plain) {
    const uint32_t n = static_cast<uint32_t>(plain.size());
    std::string out;
    out += static_cast<char>((n >> 24) & 0xff);
    out += static_cast<char>((n >> 16) & 0xff);
    out += static_cast<char>((n >> 8) & 0xff);
    out += static_cast<char>(n & 0xff);
    return out + encrypt(plain);
}

bool too_deep(const std::string &t, int max) {
    int depth = 0;
    bool in_string = false, escaped = false;
    for (const char c : t) {
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
        } else if (c == '"') {
            in_string = true;
        } else if (c == '[' || c == '{') {
            if (++depth > max) return true;
        } else if (c == ']' || c == '}') {
            --depth;
        }
    }
    return false;
}

bool parse_reply(const std::string &plain, json &out) {
    if (plain.size() > kMaxReply || too_deep(plain)) return false;
    json j = json::parse(plain, nullptr, false);
    if (!j.is_object()) return false;
    out = std::move(j);
    return true;
}

bool switched_type(const std::string &device_type) {
    const std::string t = upper(util::trim(device_type));
    return t == "IOT.SMARTPLUGSWITCH" || t == "IOT.SMARTBULB";
}

bool needs_new_login(const std::map<std::string, std::string> &info) {
    const auto it = info.find("scheme");
    if (it == info.end()) return false;
    const std::string scheme = upper(util::trim(it->second));
    return !scheme.empty() && scheme != "XOR";
}

bool reply_says_new_login(const std::string &json_text, std::string *mac) {
    if (mac) mac->clear();
    if (json_text.size() > 9000 || too_deep(json_text)) return false;
    const json j = json::parse(json_text, nullptr, false);
    const json &r = member(j, "result");
    if (!r.is_object()) return false;
    const std::string type = upper(util::trim(text(r, "device_type")));
    if (type.compare(0, 4, "IOT.") != 0) return false;
    const std::string scheme = upper(util::trim(text(member(r, "mgt_encrypt_schm"), "encrypt_type")));
    if (scheme.empty() || scheme == "XOR") return false;
    if (mac) *mac = mac_digits(text(r, "mac"));
    return true;
}

bool things_from(const json &sysinfo, std::vector<home::Thing> &out, std::string &err) {
    out.clear();
    if (!sysinfo.is_object()) {
        err = kUnreadable;
        return false;
    }
    const std::string type = type_of(sysinfo);
    const bool metered = upper(text(sysinfo, "feature")).find("ENE") != std::string::npos;
    if (type == "IOT.SMARTPLUGSWITCH") {
        // A power strip is told by its "children", even an empty list: read as one plug, it would
        // get a command without a socket, which may switch every socket at once, each without the
        // question its owner set for it.
        const json &children = member(sysinfo, "children");
        if (sysinfo.contains("children")) {
            if (!children.is_array()) {
                err = kUnreadable;
                return false;
            }
            std::set<std::string> seen;
            for (const json &c : children) {
                if (out.size() >= kMaxSockets) break;
                if (!c.is_object()) continue;
                const std::string id = text(c, "id");
                if (!valid_child_id(id) || !seen.insert(id).second) continue;
                home::Thing t;
                t.id = id;
                t.name = home::clean_name(text(c, "alias"));
                if (t.name.empty()) t.name = "Socket " + std::to_string(out.size() + 1);
                t.type = "plug";
                t.caps = home::kPower;
                if (metered) t.caps |= home::kSensor;
                t.ref = {{"child", id}};
                state_from(sysinfo, t);
                out.push_back(t);
            }
            if (out.empty()) {
                err = kUnreadable;
                return false;
            }
            return true;
        }
        home::Thing t;
        t.id = "main";
        t.name = home::clean_name(text(sysinfo, "alias"));
        if (member(sysinfo, "brightness").is_number()) {
            t.type = "light";   // a dimmer: a wall dimmer or a plug-in one, both drive lights
            t.caps = home::kPower | home::kBrightness;
            t.ref = {{"dimmer", true}};
        } else {
            t.type = util::lower(text(sysinfo, "dev_name")).find("switch") != std::string::npos ? "switch" : "plug";
            t.caps = home::kPower;
            t.ref = json::object();
        }
        if (metered) t.caps |= home::kSensor;
        if (t.name.empty()) t.name = "Kasa " + t.type;
        state_from(sysinfo, t);
        out.push_back(t);
        return true;
    }
    if (type == "IOT.SMARTBULB") {
        home::Thing t;
        t.id = "main";
        t.name = home::clean_name(text(sysinfo, "alias"));
        if (t.name.empty()) t.name = "Kasa light";
        t.type = "light";
        t.caps = home::kPower;
        if (flag(sysinfo, "is_dimmable")) t.caps |= home::kBrightness;
        if (flag(sysinfo, "is_variable_color_temp")) t.caps |= home::kColorTemp;
        if (flag(sysinfo, "is_color")) t.caps |= home::kColor;
        int lo = 0, hi = 0;
        white_range(text(sysinfo, "model"), lo, hi);
        // Light strips (they report their length) take the same settings from another service.
        t.ref = {{"light", sysinfo.contains("length") ? "strip" : "bulb"}, {"kelvin_min", lo}, {"kelvin_max", hi}};
        state_from(sysinfo, t);
        out.push_back(t);
        return true;
    }
    err = kNotThisKind;
    return false;
}

void state_from(const json &sysinfo, home::Thing &t) {
    const std::string child = util::jget(t.ref, "child", std::string());
    if (!child.empty()) {
        const json &children = member(sysinfo, "children");
        if (!children.is_array()) return;
        for (const json &c : children)
            if (c.is_object() && text(c, "id") == child) {
                t.state = {{"on", number(c, "state", 0) == 1}};
                return;
            }
        return;
    }
    if (t.ref.is_object() && t.ref.contains("light")) {
        const json &ls = member(sysinfo, "light_state");
        if (!ls.is_object()) return;
        json s = {{"on", number(ls, "on_off", 0) == 1}};
        if (s["on"].get<bool>()) {
            if (t.caps & home::kBrightness) {
                const long b = number(ls, "brightness", -1);
                if (b >= 0) s["brightness"] = clamp(b, 0, 100);
            }
            const long k = number(ls, "color_temp", -1);
            if ((t.caps & home::kColorTemp) && k >= 1000 && k <= 20000) {
                s["kelvin"] = static_cast<int>(k);
            } else if ((t.caps & home::kColor) && k == 0) {
                const long h = number(ls, "hue", -1), sat = number(ls, "saturation", -1);
                if (h >= 0 && sat >= 0) {
                    s["hue"] = clamp(h, 0, 360);
                    s["saturation"] = clamp(sat, 0, 100);
                }
            }
        }
        t.state = s;
        return;
    }
    json s = {{"on", number(sysinfo, "relay_state", 0) == 1}};
    if (t.caps & home::kBrightness) {
        const long b = number(sysinfo, "brightness", -1);
        if (b >= 0) s["brightness"] = clamp(b, 0, 100);
    }
    t.state = s;
}

void energy_into(const json &realtime, const json &daystat, int day, json &state) {
    if (!state.is_object()) state = json::object();
    // Older hardware reports watts and kilowatt-hours ("power", "energy"); newer, milliwatts and
    // watt-hours ("power_mw", "energy_wh").
    if (realtime.is_object() && number(realtime, "err_code", 0) == 0) {
        double w = -1;
        if (member(realtime, "power_mw").is_number()) w = member(realtime, "power_mw").get<double>() / 1000.0;
        else if (member(realtime, "power").is_number()) w = member(realtime, "power").get<double>();
        if (std::isfinite(w) && w > -1 && w <= 100000) state["power_w"] = std::round(std::max(0.0, w) * 10) / 10;
    }
    const json &days = member(daystat, "day_list");
    if (daystat.is_object() && number(daystat, "err_code", 0) == 0 && days.is_array()) {
        for (const json &d : days) {
            if (number(d, "day", -1) != day) continue;
            double wh = -1;
            if (member(d, "energy_wh").is_number()) wh = member(d, "energy_wh").get<double>();
            else if (member(d, "energy").is_number()) wh = member(d, "energy").get<double>() * 1000.0;
            if (std::isfinite(wh) && wh >= 0 && wh <= 1000000) state["energy_today_wh"] = std::lround(wh);
            break;
        }
    }
}

bool request_for(const home::Thing &thing, const home::Change &change, json &request, std::string &err) {
    request = json::object();
    const bool light = thing.ref.is_object() && thing.ref.contains("light");
    const bool dimmer = util::jget(thing.ref, "dimmer", false);
    if (!only_lighting(change)) {
        err = light || dimmer ? "this Kasa light can only be switched on and off, dimmed or given a colour"
                              : "this Kasa " + std::string(thing.type == "switch" ? "switch" : "plug") +
                                    " can only be switched on and off";
        return false;
    }
    std::optional<bool> on = change.on;
    std::optional<int> brightness = change.brightness;
    if (brightness) {
        if (!(thing.caps & home::kBrightness)) {
            err = "it cannot be dimmed";
            return false;
        }
        brightness = std::max(0, std::min(100, *brightness));
        if (*brightness == 0) {   // as dark as it goes is off
            brightness.reset();
            on = false;
        }
    }
    if (light) {
        if (change.kelvin && change.hs) {
            err = "a light takes a colour or a shade of white, not both at once";
            return false;
        }
        if (change.kelvin && !(thing.caps & home::kColorTemp)) {
            err = "its shade of white cannot be changed";
            return false;
        }
        if (change.hs && !(thing.caps & home::kColor)) {
            err = "it can only be set to shades of white";
            return false;
        }
        json st = json::object();
        const bool values = brightness || change.kelvin || change.hs;
        if (!on && !values) {
            err = "there is nothing to change";
            return false;
        }
        if (brightness) st["brightness"] = *brightness;
        if (change.kelvin) {
            const int lo = util::jget(thing.ref, "kelvin_min", 2700), hi = util::jget(thing.ref, "kelvin_max", 5000);
            st["color_temp"] = std::max(lo, std::min(hi, *change.kelvin));
        }
        if (change.hs) {
            st["hue"] = std::max(0, std::min(360, change.hs->first));
            st["saturation"] = std::max(0, std::min(100, change.hs->second));
            st["color_temp"] = 0;   // 0 selects the colour rather than a white
        }
        st["on_off"] = on && !*on ? 0 : 1;
        // Switched on with nothing else asked for, the bulb comes back as its owner set it to;
        // with values, or switched off, it takes exactly what is sent.
        st["ignore_default"] = values || !st["on_off"].get<int>() ? 1 : 0;
        const bool strip = util::jget(thing.ref, "light", std::string()) == "strip";
        request[strip ? kLightStrip : kBulb] = {{strip ? "set_light_state" : "transition_light_state", st}};
        return true;
    }
    if (change.kelvin || change.hs) {
        err = "its colour cannot be changed";
        return false;
    }
    if (!on && !brightness) {
        err = "there is nothing to change";
        return false;
    }
    if (brightness) {
        if (!dimmer) {
            err = "it cannot be dimmed";
            return false;
        }
        request[kDimmer] = {{"set_brightness", {{"brightness", *brightness}}}};
    }
    if (on) request[kRelay] = {{"set_relay_state", {{"state", *on ? 1 : 0}}}};
    const std::string child = util::jget(thing.ref, "child", std::string());
    if (thing.ref.is_object() && thing.ref.contains("child")) {
        if (!valid_child_id(child)) {
            request = json::object();
            err = "this socket's saved id is not usable; read the device again in Settings";
            return false;
        }
        request["context"] = {{"child_ids", json::array({child})}};
    }
    return true;
}

bool request_for_sockets(const std::vector<const home::Thing *> &things, const home::Change &change, json &request,
                         std::string &err) {
    request = json::object();
    const bool only_power = change.on && !change.brightness && !change.kelvin && !change.hs && only_lighting(change);
    if (things.size() < 2 || !only_power) return false;
    std::vector<std::string> ids;
    for (const home::Thing *t : things) {
        const std::string child = t ? util::jget(t->ref, "child", std::string()) : std::string();
        if (!valid_child_id(child)) return false;   // not all sockets: one at a time instead
        if (std::find(ids.begin(), ids.end(), child) == ids.end()) ids.push_back(child);
    }
    (void)err;
    request = {{"context", {{"child_ids", ids}}}, {kRelay, {{"set_relay_state", {{"state", *change.on ? 1 : 0}}}}}};
    return true;
}

bool reply_ok(const json &request, const json &reply, std::string &err) {
    for (auto m = request.begin(); m != request.end(); ++m) {
        if (m.key() == "context" || !m->is_object()) continue;
        const json &module = member(reply, m.key().c_str());
        if (!module.is_object()) {
            err = kUnreadable;
            return false;
        }
        for (auto call = m->begin(); call != m->end(); ++call) {
            const json &answer = member(module, call.key().c_str());
            const json &code_of = answer.is_object() ? answer : module;   // "module not support" comes without the method
            const json &code = member(code_of, "err_code");
            if (!answer.is_object() && code.is_null()) {
                err = kUnreadable;
                return false;
            }
            if (!code.is_null() && !code.is_number_integer()) {
                err = kUnreadable;
                return false;
            }
            const long c = number(code_of, "err_code", 0);
            if (c == -1 || c == -2) {
                err = kNotSupported;
                return false;
            }
            if (c != 0) {
                err = kCommandRefused;
                return false;
            }
        }
    }
    return true;
}

std::string mac_digits(const std::string &raw) {
    std::string hex;
    for (const char c : raw) {
        if (c == ':' || c == '-') continue;
        if (!isxdigit(static_cast<unsigned char>(c)) || hex.size() >= 12) return "";
        hex += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return hex.size() == 12 ? hex : "";
}

bool same_device(const std::string &saved_mac, const json &sysinfo) {
    const std::string saved = mac_digits(saved_mac);
    const std::string mac = mac_digits(text(sysinfo, "mac")), mic = mac_digits(text(sysinfo, "mic_mac"));
    if (saved.empty() || (mac.empty() && mic.empty())) return true;   // nothing to compare
    return saved == mac || saved == mic;
}

// Discovery gives kind "tplink" the port that answered (9999) or 0. A device an earlier version
// filed under Tapo kept the newer login's HTTP port, which is not this protocol's.
int port_for(const home::Device &dev) { return dev.kind == "tplink" && dev.port > 0 ? dev.port : kDefaultPort; }

void set_probe_port(int port) { g_probe_port = port; }
int probe_port() { return g_probe_port; }

}  // namespace detail
}  // namespace kasa

std::unique_ptr<home::Driver> make_kasa_driver() { return std::make_unique<kasa::KasaDriver>(); }
