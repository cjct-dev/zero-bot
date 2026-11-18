// WiZ lights, plugs and fans over UDP 38899. See wiz.h for the protocol in brief.
//
// Ported from sbidy/pywizlight (MIT licence, Copyright (c) 2020 Stephan Traub; the notice is in
// licenses/THIRD-PARTY-NOTICES.md):
//   bulblibrary.py  how a module name says what a device can do            -> classify()
//   bulb.py         the methods, the pilot fields, the lowest dimming,
//                   the resend schedule, getPower in milliwatts            -> pilot_for(), fill_state(), call()
//   rgbcw.py        hue/saturation to and from the r, g, b and white LEDs  -> hs_to_rgbw(), rgbw_to_hs()
//   utils.py        signed requests for firmware that demands them         -> datagram()
#include "smarthome/drivers/wiz.h"

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/timex.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <map>
#include <mutex>
#include <set>

#include <openssl/evp.h>

#include "smarthome/home.h"

namespace wiz {

const char *const kNoAnswer = "The WiZ device did not answer.";
const char *const kGarbled = "The WiZ device replied with something that could not be understood.";
const char *const kNeedsKey =
    "This WiZ device only accepts signed commands. Type the WiZ signing key in Settings, Smart home, on the device's page.";
const char *const kKeyRefused =
    "A WiZ device did not accept the signing key, so it is not sent again. Type the key again in Settings, Smart home, "
    "on the device's page.";
const char *const kClockNotSet = "The clock here is not set yet, so a signed command cannot be sent. Try again in a minute.";
const char *const kRefused = "The WiZ device refused the command.";

using home::Change;
using home::Device;
using home::Thing;

namespace {

constexpr int kPort = 38899;
constexpr int kFirstResendMs = 250;    // then half as long again each time
constexpr int kMaxSends = 6;
constexpr size_t kMaxDatagram = 8192;
// 2025-01-01: a clock earlier than this has not been set since the board started, and a request
// signed with it would be refused as stale.
constexpr long long kEarliestSigningTime = 1735689600;
// How long after start an unsynchronised clock is distrusted: long enough for a time service to
// set it on any working connection, short enough not to block a board that has none for good.
constexpr long long kClockSettleS = 600;
const char *const kFamily = "wiz";
// Said when nothing answers the first request to a device, which is also how a device with "Allow
// local communication" switched off behaves.
const char *const kNoAnswerSetup =
    "The WiZ device did not answer. Check that it is switched on, and that Allow local communication is on in the WiZ "
    "app's settings.";

long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

long long system_unix_time() { return static_cast<long long>(time(nullptr)); }
long long (*g_unix_clock)() = system_unix_time;
long long unix_now() { return g_unix_clock(); }

// Whether the kernel holds the wall clock as set by a time service (what timedatectl calls
// "System clock synchronized"). Only read, never changed.
bool kernel_clock_synced() {
    timex tx{};
    const int state = ntp_adjtime(&tx);
    return state >= 0 && state != TIME_ERROR && !(tx.status & STA_UNSYNC);
}

long long uptime_s() {
    timespec t{};
    return clock_gettime(CLOCK_BOOTTIME, &t) == 0 ? static_cast<long long>(t.tv_sec) : 0;
}

// Values from a device are whatever it sent: read them without ever throwing.
bool jnum(const json &o, const char *key, double &out) {
    if (!o.is_object()) return false;
    const auto it = o.find(key);
    if (it == o.end()) return false;
    if (it->is_boolean()) out = it->get<bool>() ? 1 : 0;
    else if (it->is_number()) out = it->get<double>();
    else return false;
    return std::isfinite(out);
}
int jint(const json &o, const char *key, int fallback) {
    double v;
    if (!jnum(o, key, v) || v < -2.0e9 || v > 2.0e9) return fallback;
    return static_cast<int>(std::lround(v));
}
bool jbool(const json &o, const char *key) {
    double v;
    return jnum(o, key, v) && v != 0;
}
std::string jstr(const json &o, const char *key) {
    if (!o.is_object()) return {};
    const auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool is_hex(const std::string &s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::string hex_of(const std::string &bytes) {
    static const char *digits = "0123456789abcdef";
    std::string out;
    for (const unsigned char c : bytes) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

std::string hmac_sha256_b64(const std::string &key, const std::string &text) {
    unsigned char mac[EVP_MAX_MD_SIZE];
    size_t len = 0;
    if (!EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, key.data(), key.size(),
                   reinterpret_cast<const unsigned char *>(text.data()), text.size(), mac, sizeof mac, &len))
        return {};
    std::string out((len + 2) / 3 * 4 + 1, '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(&out[0]), mac, static_cast<int>(len));
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return out;
}

enum class Got { Reply, Nothing, Garbage };

// Sends one datagram and waits for the JSON object that answers it: one whose "method" is ours,
// or an error that names no method. Sent again at 250 ms and then at growing intervals until the
// reply or the deadline (steady clock, ms). A socket of its own, connected, so that only the
// device's own datagrams are seen; it is closed before this returns. Other datagrams (a late
// answer to an earlier request, a push) are skipped.
Got udp_exchange(const std::string &ip, int port, const std::string &method, const std::string &datagram, long long deadline,
                 json &reply) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;   // an address, never a name to look up
    if (port <= 0 || port > 65535 || getaddrinfo(ip.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
        return Got::Nothing;
    const int fd = socket(res->ai_family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    const bool connected = fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) == 0;
    freeaddrinfo(res);
    if (!connected) {
        if (fd >= 0) close(fd);
        return Got::Nothing;
    }

    long long next_send = now_ms();
    int interval = kFirstResendMs, sends = 0;
    Got got = Got::Nothing;
    std::string buf(kMaxDatagram, '\0');
    for (;;) {
        const long long now = now_ms();
        if (now >= deadline) break;
        if (sends < kMaxSends && now >= next_send) {
            (void)!send(fd, datagram.data(), datagram.size(), MSG_NOSIGNAL);   // a refusal shows up on a read
            sends++;
            next_send = now + interval;
            interval += interval / 2;
        }
        const long long until = sends < kMaxSends ? std::min(next_send, deadline) : deadline;
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, static_cast<int>(std::max(1LL, until - now))) <= 0) continue;
        const ssize_t n = recv(fd, &buf[0], buf.size(), MSG_TRUNC);
        if (n <= 0) continue;   // nothing listening there (yet): keep to the schedule
        if (static_cast<size_t>(n) > buf.size()) {   // longer than any answer: not one
            got = Got::Garbage;
            continue;
        }
        json j = json::parse(buf.begin(), buf.begin() + n, nullptr, false);
        if (!j.is_object()) {
            got = Got::Garbage;
            continue;
        }
        const std::string m = jstr(j, "method");
        if (m == method || (!j.contains("method") && j.contains("error"))) {
            reply = std::move(j);
            got = Got::Reply;
            break;
        }
    }
    close(fd);
    return got;
}

// Devices that turned down an unsigned command in this run, by device id: they want a key even
// before a read has marked their things as signed.
std::mutex g_mtx;
std::set<std::string> g_needs_key;

std::string memory_id(const Device &dev) { return dev.id.empty() ? dev.ip + ":" + std::to_string(dev.port) : dev.id; }
bool needs_key_now(const Device &dev) {
    std::lock_guard<std::mutex> lock(g_mtx);
    return g_needs_key.count(memory_id(dev)) > 0;
}

enum class Outcome { Ok, NoAnswer, Garbled, NoSuchMethod, NeedsKey, KeyRefused, ClockNotSet, Refused };

const char *sentence(Outcome o) {
    switch (o) {
        case Outcome::NoAnswer: return kNoAnswer;
        case Outcome::Garbled: return kGarbled;
        case Outcome::NeedsKey: return kNeedsKey;
        case Outcome::KeyRefused: return kKeyRefused;
        case Outcome::ClockNotSet: return kClockNotSet;
        default: return kRefused;
    }
}

struct Target {
    std::string memory, ip;
    int port = kPort;
    bool signed_firmware = false;   // set when a reply carried a signature
};

// One request and its reply, unsigned unless a key (16 bytes) is given.
Outcome call(Target &t, const std::string &method, const std::string &params, const std::string &key, long long deadline,
             json &result) {
    result = json::object();
    json reply;
    const Got got = udp_exchange(t.ip, t.port, method, detail::datagram(method, params, key, unix_now()), deadline, reply);
    if (got == Got::Nothing) return Outcome::NoAnswer;
    if (got == Got::Garbage) return Outcome::Garbled;
    if (reply.contains("hmac")) t.signed_firmware = true;
    if (reply.contains("error")) {
        const int code = jint(reply["error"], "code", 0);
        if (code == -32601) return Outcome::NoSuchMethod;
        if (method != "setPilot") return Outcome::Refused;
        if (!key.empty()) return code == -32605 ? Outcome::KeyRefused : Outcome::Refused;
        // Unsigned: a "security error", or "invalid params" from firmware that signs its replies.
        if (code == -32605 || (code == -32602 && t.signed_firmware)) {
            std::lock_guard<std::mutex> lock(g_mtx);
            g_needs_key.insert(t.memory);
            return Outcome::NeedsKey;
        }
        return Outcome::Refused;
    }
    if (!reply.contains("result") || !reply["result"].is_object()) return Outcome::Garbled;
    result = reply["result"];
    return Outcome::Ok;
}

// "udp_signing_key" wherever it sits in the WiZ app's export, should someone paste the whole file.
std::string find_key_in(const json &j, int depth = 0) {
    if (depth > 8) return {};
    if (j.is_object()) {
        const std::string direct = jstr(j, "udp_signing_key");
        if (!direct.empty()) return direct;
    }
    if (j.is_object() || j.is_array())
        for (const auto &child : j) {
            const std::string found = find_key_in(child, depth + 1);
            if (!found.empty()) return found;
        }
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Building blocks (no network)
// ---------------------------------------------------------------------------------------------
namespace detail {

void set_unix_clock(long long (*clock)()) { g_unix_clock = clock ? clock : system_unix_time; }

bool signing_clock_ok(long long unix_time, bool synced, long long uptime_s) {
    if (unix_time < kEarliestSigningTime) return false;
    return synced || uptime_s >= kClockSettleS;
}

uint32_t Model::light_caps() const {
    if (!has_light()) return 0;
    uint32_t caps = home::kPower | home::kBrightness;
    if (cls == "RGB" || cls == "TW" || cls == "FANTW") caps |= home::kColorTemp;
    if (cls == "RGB") caps |= home::kColor;
    return caps;
}

// pywizlight bulblibrary.py BulbType.from_data(), with one difference: a module name with none of
// the known parts is not taken for a dimmable bulb, so that a kind of device this driver does not
// know is never offered.
bool classify(const std::string &module_name, int type_id, Model &m) {
    m.cls.clear();
    if (module_name.empty()) {
        if (type_id == 0) m.cls = "DW";   // firmware 1.8 names no module; type 0 is a dimmable white bulb
        return !m.cls.empty();
    }
    const size_t a = module_name.find('_');
    if (a == std::string::npos) return false;
    const size_t b = module_name.find('_', a + 1);
    const std::string id = module_name.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
    const auto has = [&](const char *part) { return id.find(part) != std::string::npos; };
    if (has("RGB")) m.cls = "RGB";
    else if (has("DDTW")) m.cls = "FANTW";       // ceiling fan with a tunable white light
    else if (has("TW")) m.cls = "TW";
    else if (has("SOCKET")) m.cls = "SOCKET";
    else if (has("FANDIM")) m.cls = "FANDIM";    // ceiling fan with a dimmable light
    else if (has("DW") || has("DIM")) m.cls = "DW";   // dimmable bulbs, and the wall dimmer (DIMTRIACS)
    return !m.cls.empty();
}

void apply_config(const json &result, Model &m) {
    if (!result.is_object()) return;
    for (const char *key : {"extRange", "cctRange", "whiteRange"}) {
        const auto it = result.find(key);
        if (it == result.end() || !it->is_array() || it->empty() || it->size() > 16) continue;
        double lo = 1e9, hi = 0;
        bool numbers = true;
        for (const auto &v : *it) {
            numbers = numbers && v.is_number() && std::isfinite(v.get<double>());
            if (numbers) lo = std::min(lo, v.get<double>()), hi = std::max(hi, v.get<double>());
        }
        if (!numbers || lo < 1000 || hi > 10000 || lo > hi) continue;
        m.kelvin_min = static_cast<int>(lo);
        m.kelvin_max = static_cast<int>(hi);
        break;
    }
    const int min_dim = jint(result, "minDimLevel", 0);
    if (min_dim >= 1 && min_dim < 100) m.min_dimming = min_dim;
    const int speeds = jint(result, "fanSpeed", 0);
    if (speeds >= 1 && speeds <= 100) m.fan_speeds = speeds;
}

// pywizlight bulb.py _clamp_dimming(): the percentage is sent as it is, raised to the lowest the
// module takes.
int dimming_from_percent(int percent, int min_dimming) {
    return std::clamp(percent, std::clamp(min_dimming, 1, 100), 100);
}

int percent_from_dimming(int dimming) { return std::clamp(dimming, 1, 100); }

int fan_speed_from_percent(int percent, int speeds) {
    if (percent <= 0) return 0;
    speeds = std::max(1, speeds);
    return std::clamp(static_cast<int>(std::ceil(std::min(percent, 100) * speeds / 100.0)), 1, speeds);
}

int percent_from_fan_speed(int speed, int speeds) {
    speeds = std::max(1, speeds);
    return static_cast<int>(std::lround(std::clamp(speed, 0, speeds) * 100.0 / speeds));
}

namespace {
const double kEps = 1.0e-5;
const double kPi = 3.14159265358979323846;
const double kThird = kPi * 2 / 3;
// Red, green and blue as directions 120 degrees apart on the colour wheel.
void basis(int i, double out[2]) {
    out[0] = std::cos(kThird * i);
    out[1] = std::sin(kThird * i);
}
double dot(const double a[2], const double b[2]) { return a[0] * b[0] + a[1] * b[1]; }
}  // namespace

// pywizlight rgbcw.py: hs2rgbcw() and trapezoid().
void hs_to_rgbw(double hue, double saturation, int rgb[3], int &white) {
    if (!std::isfinite(hue)) hue = 0;
    if (!std::isfinite(saturation)) saturation = 0;
    double turn = hue / 360;
    turn -= std::floor(turn);
    const double angle = turn * kPi * 2;
    const double dir[2] = {std::cos(angle), std::sin(angle)};
    const double sat = std::clamp(saturation / 100, 0.0, 1.0);

    double level[3] = {0, 0, 0};
    if (sat > kEps) {
        // The colour is a mix of the one or two primaries within 120 degrees of it.
        const double edge = std::cos(kThird - kEps);
        double b[3][2];
        int near[3], count = 0;
        for (int i = 0; i < 3; i++) {
            basis(i, b[i]);
            if (dot(dir, b[i]) > edge) near[count++] = i;
        }
        if (count == 1) {
            level[near[0]] = 1;
        } else if (count == 2) {
            const double *b0 = b[near[0]], *b1 = b[near[1]];
            const double line[2] = {b1[1], -b1[0]};
            const double c0 = dot(dir, line) / dot(b0, line);
            const double at[2] = {dir[0] - b0[0] * c0, dir[1] - b0[1] * c0};
            const double c1 = dot(at, b1);
            const double top = std::max(c0, c1);
            level[near[0]] = std::min(c0 / top, 1.0);
            level[near[1]] = std::min(c1 / top, 1.0);
        }
    }
    double w = 1;
    if (sat >= 0.5) {
        w = 1 - (sat - 0.5) * 2;
    } else {
        for (double &l : level) l *= sat * 2;
    }
    for (int i = 0; i < 3; i++) rgb[i] = std::clamp(static_cast<int>(level[i] * 255), 0, 255);
    white = std::clamp(static_cast<int>(w * 128), 0, 128);
}

// pywizlight rgbcw.py: rgbcw2hs(). One difference: red, green and blue all equal (no direction on
// the colour wheel) is given as saturation 0 rather than as red.
void rgbw_to_hs(const int rgb[3], int white, int &hue, int &saturation) {
    double dir[2] = {0, 0};
    for (int i = 0; i < 3; i++) {
        double b[2];
        basis(i, b);
        const double level = std::clamp(rgb[i], 0, 255) / 255.0;
        dir[0] += b[0] * level;
        dir[1] += b[1] * level;
    }
    const double len_sq = dot(dir, dir);
    const double len = len_sq > kEps ? std::sqrt(len_sq) : 0;
    const double w = std::clamp(white, 0, 128) / 128.0;
    const double sat = len == 0 ? 0 : w == 1 ? len * 0.5 : 1 - w / 2;
    double angle = len == 0 ? 0 : std::atan2(dir[1], dir[0]);
    if (angle < 0) angle += kPi * 2;
    hue = static_cast<int>(std::lround(angle * 180 / kPi)) % 360;
    saturation = std::clamp(static_cast<int>(std::lround(sat * 100)), 0, 100);
}

bool parse_key(const std::string &text, std::string &key) {
    if (text.size() > 65536) return false;
    std::string candidate = text;
    const json pasted = json::parse(text, nullptr, false);
    if (pasted.is_object() || pasted.is_array()) candidate = find_key_in(pasted);
    std::string hex;
    for (const unsigned char c : candidate)
        if (!std::isspace(c)) hex += static_cast<char>(c);
    if (hex.size() != 32 || !is_hex(hex)) return false;
    key.clear();
    for (size_t i = 0; i < 32; i += 2) key += static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16));
    return true;
}

// pywizlight utils.py sign_request(): the HMAC covers the params text exactly as sent, compact,
// with the time of signing last in it.
std::string datagram(const std::string &method, const std::string &params, const std::string &key, long long unix_time) {
    if (key.empty()) return "{\"method\":\"" + method + "\",\"params\":" + params + "}";
    std::string body = params.size() > 2 ? params.substr(0, params.size() - 1) + "," : "{";
    body += "\"sigTs\":" + std::to_string(unix_time) + "}";
    return "{\"method\":\"" + method + "\",\"env\":\"pro\",\"params\":" + body + ",\"hmac\":\"" + hmac_sha256_b64(key, body) + "\"}";
}

std::vector<Thing> things_for(const Model &m, const std::string &mac, bool metering, bool signed_firmware) {
    const std::string tail = mac.size() >= 4 && mac.size() <= 32 && is_hex(mac) ? " " + home::clean_name(mac.substr(mac.size() - 4)) : "";
    std::vector<Thing> out;
    const auto add = [&](const char *id, const std::string &name, const char *type, uint32_t caps, json ref) {
        Thing t;
        t.id = id;
        t.name = name;
        t.type = type;
        t.caps = caps;
        if (signed_firmware) ref["signed"] = true;   // next_step asks for the key while there is none
        t.ref = std::move(ref);
        t.state = json::object();
        out.push_back(std::move(t));
    };
    if (m.is_plug()) add("plug", "WiZ plug" + tail, "plug", home::kPower | (metering ? uint32_t(home::kSensor) : 0u), {{"k", "plug"}});
    if (m.has_light())
        add("light", "WiZ light" + tail, "light", m.light_caps(),
            {{"k", "light"}, {"kmin", m.kelvin_min}, {"kmax", m.kelvin_max}, {"mind", m.min_dimming}});
    if (m.has_fan()) add("fan", "WiZ fan" + tail, "fan", home::kPower | home::kFanSpeed, {{"k", "fan"}, {"speeds", m.fan_speeds}});
    return out;
}

void fill_state(Thing &thing, const json &pilot, const json *power_mw) {
    const std::string kind = jstr(thing.ref, "k");
    json state = json::object();
    const bool on = jbool(pilot, "state");
    if (kind == "light") {
        state["on"] = on;
        const int dimming = jint(pilot, "dimming", -1);
        if (dimming >= 0 && (thing.caps & home::kBrightness)) state["brightness"] = percent_from_dimming(dimming);
        // A light in one of its scenes is neither a plain white nor a plain colour.
        const int scene = jint(pilot, "sceneId", 0);
        const int temp = jint(pilot, "temp", 0);
        if (scene == 0 && temp >= 1000 && temp <= 10000 && (thing.caps & home::kColorTemp)) {
            state["kelvin"] = temp;
        } else if (scene == 0 && temp <= 0 && (thing.caps & home::kColor) && pilot.contains("r") && pilot.contains("g") &&
                   pilot.contains("b")) {
            const int rgb[3] = {jint(pilot, "r", 0), jint(pilot, "g", 0), jint(pilot, "b", 0)};
            int hue, sat;
            rgbw_to_hs(rgb, jint(pilot, "w", 0), hue, sat);
            state["hue"] = hue;
            state["saturation"] = sat;
        }
    } else if (kind == "plug") {
        state["on"] = on;
        // Milliwatts: from getPower, or "pc" where the pilot itself carries the reading.
        double mw = 0;
        bool known = power_mw && power_mw->is_number() && std::isfinite(mw = power_mw->get<double>());
        if (!known) known = jnum(pilot, "pc", mw);
        if (known && mw >= 0 && mw < 1.0e8 && (thing.caps & home::kSensor)) state["power_w"] = std::round(mw / 100.0) / 10.0;
    } else if (kind == "fan") {
        state["on"] = jint(pilot, "fanState", 0) == 1;
        const int speed = jint(pilot, "fanSpeed", -1);
        if (speed >= 0) state["fan"] = percent_from_fan_speed(speed, jint(thing.ref, "speeds", 6));
    }
    thing.state = std::move(state);
}

bool pilot_for(const Thing &thing, const Change &c, std::string &params, std::string &err) {
    const std::string kind = jstr(thing.ref, "k");
    if (c.target_c || c.mode || c.position || c.volume || c.volume_step || c.mute || c.transport || c.source || c.key)
        return err = "A WiZ device cannot do that.", false;
    nlohmann::ordered_json p = nlohmann::ordered_json::object();
    if (kind == "plug") {
        if (!c.on || c.brightness || c.kelvin || c.hs || c.fan) return err = "A plug can only be switched on or off.", false;
        p["state"] = *c.on;
    } else if (kind == "fan") {
        if ((!c.on && !c.fan) || c.brightness || c.kelvin || c.hs)
            return err = "A fan can be switched on or off, or set to a speed.", false;
        const int speed = c.fan ? fan_speed_from_percent(*c.fan, jint(thing.ref, "speeds", 6)) : -1;
        if ((c.on && !*c.on) || speed == 0) {
            p["fanState"] = 0;
        } else {
            p["fanState"] = 1;
            if (speed > 0) p["fanSpeed"] = speed;
        }
    } else if (kind == "light") {
        if (c.fan) return err = "A light has no fan speed.", false;
        if (c.on && !*c.on) {
            p["state"] = false;
        } else {
            if (!c.on && !c.brightness && !c.kelvin && !c.hs) return err = "A light cannot do that.", false;
            if (c.brightness && !(thing.caps & home::kBrightness)) return err = "This light cannot be dimmed.", false;
            if (c.kelvin && !(thing.caps & home::kColorTemp)) return err = "This light cannot change its shade of white.", false;
            if (c.hs && !(thing.caps & home::kColor)) return err = "This light cannot change colour.", false;
            // The device takes a white temperature or a colour, not both in one command.
            if (c.kelvin && c.hs) return err = "Ask for a colour or a shade of white, not both at once.", false;
            p["state"] = true;
            if (c.brightness) p["dimming"] = dimming_from_percent(*c.brightness, jint(thing.ref, "mind", 10));
            if (c.kelvin) {
                const int lo = jint(thing.ref, "kmin", 2200), hi = jint(thing.ref, "kmax", 6500);
                p["temp"] = std::clamp(*c.kelvin, std::min(lo, hi), std::max(lo, hi));
            }
            if (c.hs) {
                int rgb[3], white;
                hs_to_rgbw(c.hs->first, c.hs->second, rgb, white);
                p["r"] = rgb[0];
                p["g"] = rgb[1];
                p["b"] = rgb[2];
                p["w"] = white;
            }
        }
    } else {
        return err = "This is not something the WiZ driver can control.", false;
    }
    params = p.dump();
    return true;
}

}  // namespace detail

// ---------------------------------------------------------------------------------------------
// The driver
// ---------------------------------------------------------------------------------------------
namespace {

int left(long long deadline, int most) {
    return static_cast<int>(std::clamp(deadline - now_ms(), 0LL, static_cast<long long>(most)));
}

class WizDriver : public home::Driver {
public:
    const char *kind() const override { return kFamily; }

    home::PairStep next_step(const Device &dev) const override {
        const bool wanted = needs_key_now(dev) || std::any_of(dev.things.begin(), dev.things.end(), [](const Thing &t) {
                                return jbool(t.ref, "signed");
                            });
        if (!wanted) return {};
        const Key k = family_key();
        if (!k.bytes.empty() && !k.refused) return {};
        home::PairStep step;
        step.kind = home::Pairing::Token;
        step.field = "WiZ signing key";
        step.prompt = k.refused
                          ? "A WiZ device did not accept the saved signing key, so it is not sent again. Type the "
                            "udp_signing_key from the WiZ app's export for local integration again: 32 characters, 0 to 9 "
                            "and a to f."
                          : "This WiZ device only accepts signed commands. Type the udp_signing_key from the WiZ app's export "
                            "for local integration: 32 characters, 0 to 9 and a to f. One key serves every WiZ device of the "
                            "home.";
        return step;
    }

    // Nothing is sent: a device only shows whether a key is right when it is given a change to
    // make, and none is made up here. The first real command tells.
    bool pair(Device &, const std::string &input, std::string &err) override {
        if (!secrets_) {
            err = "Nothing can be saved.";
            return false;
        }
        std::string key;
        if (!detail::parse_key(input, key)) {
            err = "That is not a WiZ signing key: it is 32 characters, each 0 to 9 or a to f.";
            return false;
        }
        // Typed again: one more try, even for the key that was refused.
        secrets_->set(kFamily, json{{"signing_key", hex_of(key)}});
        return true;
    }

    bool enumerate(Device &dev, std::vector<Thing> &out, std::string &err) override {
        out.clear();
        const long long deadline = now_ms() + 4000;
        Target t = target(dev);

        json sys;
        Outcome o = call(t, "getSystemConfig", "{}", "", now_ms() + left(deadline, 1500), sys);
        if (o != Outcome::Ok) {
            err = o == Outcome::NoAnswer ? kNoAnswerSetup : sentence(o);
            return false;
        }
        const std::string module = jstr(sys, "moduleName"), mac = jstr(sys, "mac");
        detail::Model model;
        if (!detail::classify(module, jint(sys, "typeId", -1), model)) {
            err = "Zero Bot cannot control this kind of WiZ device.";
            return false;
        }

        // The white range and the lowest dimming: getModelConfig on firmware 1.22 and later,
        // getUserConfig before. Without an answer the usual values stand.
        json config;
        o = call(t, "getModelConfig", "{}", "", now_ms() + left(deadline, 900), config);
        if (o == Outcome::NoSuchMethod) o = call(t, "getUserConfig", "{}", "", now_ms() + left(deadline, 900), config);
        if (o == Outcome::Ok) detail::apply_config(config, model);

        json power;
        const bool metering = model.is_plug() && call(t, "getPower", "{}", "", now_ms() + left(deadline, 700), power) == Outcome::Ok &&
                              power.contains("power") && power["power"].is_number();

        json pilot;
        o = call(t, "getPilot", "{}", "", now_ms() + std::max(left(deadline, 1500), 300), pilot);
        if (o != Outcome::Ok) {
            err = sentence(o);
            return false;
        }
        out = detail::things_for(model, mac, metering, t.signed_firmware);
        for (Thing &thing : out) detail::fill_state(thing, pilot, metering ? &power["power"] : nullptr);
        if (dev.model.empty()) dev.model = home::clean_name(module);
        return true;
    }

    // Asked for particular things, a metering plug is also asked for its power (a second
    // request); asked for "all of them", only for the pilot.
    bool read(const Device &dev, std::vector<Thing> &things, std::string &err) override {
        const bool detail = !things.empty();
        if (!detail) things = dev.things;
        if (things.empty()) return true;
        const long long deadline = now_ms() + 2500;
        Target t = target(dev);
        json pilot, power;
        const Outcome o = call(t, "getPilot", "{}", "", now_ms() + 1800, pilot);
        if (o != Outcome::Ok) {
            err = sentence(o);
            return false;
        }
        const bool wants_power = detail && std::any_of(things.begin(), things.end(), [](const Thing &th) {
                                     return jstr(th.ref, "k") == "plug" && (th.caps & home::kSensor);
                                 });
        const bool metering = wants_power && left(deadline, 700) > 50 &&
                              call(t, "getPower", "{}", "", now_ms() + left(deadline, 700), power) == Outcome::Ok &&
                              power.contains("power") && power["power"].is_number();
        for (Thing &thing : things) detail::fill_state(thing, pilot, metering ? &power["power"] : nullptr);
        return true;
    }

    bool apply(const Device &dev, const Thing &thing, const Change &change, int deadline_ms, std::string &err) override {
        std::string params;
        if (!detail::pilot_for(thing, change, params, err)) return false;
        const long long deadline = now_ms() + std::clamp(deadline_ms <= 0 ? 2500 : deadline_ms, 100, 10000);
        Target t = target(dev);
        const bool known_signed = jbool(thing.ref, "signed") || needs_key_now(dev);
        json result;
        Outcome o;
        if (known_signed) {
            o = signed_set(t, params, deadline, result);
        } else {
            o = call(t, "setPilot", params, "", deadline, result);
            // Firmware that started signing since it was last read: the saved key, once.
            if (o == Outcome::NeedsKey && !family_key().bytes.empty()) o = signed_set(t, params, deadline, result);
        }
        if (o == Outcome::Ok && jbool(result, "success")) return true;
        err = o == Outcome::Ok ? kRefused : sentence(o);
        return false;
    }

private:
    struct Key {
        std::string bytes;      // 16 bytes; empty when none is saved
        bool refused = false;   // a device turned it down: it is not sent again
    };

    Key family_key() const {
        Key k;
        if (!secrets_) return k;
        const json j = secrets_->get(kFamily);
        if (!detail::parse_key(jstr(j, "signing_key"), k.bytes)) return {};
        k.refused = jbool(j, "refused");
        return k;
    }

    // setPilot signed with the family's key, when there is one that no device has refused and the
    // clock is set. A refusal is kept in the file so that no later run sends that key again.
    Outcome signed_set(Target &t, const std::string &params, long long deadline, json &result) {
        const Key k = family_key();
        if (k.bytes.empty()) return Outcome::NeedsKey;
        if (k.refused) return Outcome::KeyRefused;
        // A test's own clock counts as set.
        const bool synced = g_unix_clock != system_unix_time || kernel_clock_synced();
        if (!detail::signing_clock_ok(unix_now(), synced, uptime_s())) return Outcome::ClockNotSet;
        const Outcome o = call(t, "setPilot", params, k.bytes, deadline, result);
        if (o == Outcome::KeyRefused) {
            // Only the key that was sent is marked: one typed meanwhile gets its own try.
            json j = secrets_->get(kFamily);
            std::string saved;
            if (detail::parse_key(jstr(j, "signing_key"), saved) && saved == k.bytes) {
                j["refused"] = true;
                secrets_->set(kFamily, j);
            }
        }
        return o;
    }

    Target target(const Device &dev) const {
        Target t;
        t.memory = memory_id(dev);
        t.ip = dev.ip;
        t.port = dev.port > 0 ? dev.port : kPort;
        return t;
    }
};

}  // namespace

}  // namespace wiz

std::unique_ptr<home::Driver> make_wiz_driver() { return std::make_unique<wiz::WizDriver>(); }
