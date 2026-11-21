// Philips Hue bridges over CLIP version 2. See hue.h for what callers need to know.
//
// What this follows (the sprint's STATUS.md says what was checked against each):
//  - Signify, "Get started" (public page): POST /api with {"devicetype":"<app>#<device>"}, refused
//    until the link button is pressed, then answered with a "username" (the application key).
//  - openhue-api (Apache-2.0, an OpenAPI description of CLIP v2): GET /clip/v2/resource lists
//    every resource; the paths /clip/v2/resource/<type>[/<id>]; the hue-application-key header;
//    brightness 0 means "lowest"; mirek 153..500; 401/403 unauthorised, 429 and 503 busy;
//    {"errors":[...],"data":[...]} replies; zones group services, bridge_home groups rooms and
//    devices.
//  - aiohue (Apache-2.0, github.com/home-assistant-libs/aiohue) and its fixture recorded from a
//    real bridge: the resource shapes, scene recall {"recall":{"action":"active"}}, rooms grouping
//    devices, mirek schemas of 50..1000 (and 0..65535, which it corrects), the forms a bridge id
//    takes (normalize_bridge_id), error type 101 = link button not pressed. Nothing is copied
//    verbatim; this is a C++ re-expression of what those describe.
//  - openHAB's Hue binding (EPL-2.0) was READ ONLY to confirm behaviour: 401 and 403 both mean
//    unauthorised, version 2 needs swversion >= 1948086000. Nothing was copied from it.
//  - Colour: smarthome/color.h (the wide-gamut matrices published for Hue lights).
#include "smarthome/drivers/hue.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

#include <curl/curl.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "net/http.h"
#include "smarthome/color.h"
#include "smarthome/drivers/support.h"

namespace hue {
namespace detail {

namespace {
std::atomic<int> g_pair_window_ms{30000};
std::atomic<int> g_pair_interval_ms{1000};

const json &field(const json &j, const char *key) {
    static const json kNull;
    if (!j.is_object()) return kNull;
    const auto it = j.find(key);
    return it == j.end() ? kNull : *it;
}
std::string str(const json &j, const char *key) {
    const json &v = field(j, key);
    return v.is_string() ? v.get<std::string>() : std::string();
}
bool num(const json &j, const char *key, double &out) {
    const json &v = field(j, key);
    if (!v.is_number()) return false;
    const double d = v.get<double>();
    if (!std::isfinite(d)) return false;
    out = d;
    return true;
}
bool boolean(const json &j, const char *key, bool &out) {
    const json &v = field(j, key);
    if (!v.is_boolean()) return false;
    out = v.get<bool>();
    return true;
}
const json &array(const json &j, const char *key) {
    static const json kEmpty = json::array();
    const json &v = field(j, key);
    return v.is_array() ? v : kEmpty;
}
std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}
int clampi(long long v, int lo, int hi) { return static_cast<int>(std::max<long long>(lo, std::min<long long>(hi, v))); }
// Rounds to 1/scale (scale 10: one decimal), so that 21.37 gives exactly the double nearest 21.4.
double round_scaled(double v, double scale) { return std::round(v * scale) / scale; }

// How many of a kind of resource are looked at, at most. A bridge holds far fewer of each.
constexpr size_t kMaxResources = 2000;
constexpr size_t kMaxThings = 400;
}  // namespace

// ---------------------------------------------------------------- text from the bridge
// RFC 3629 well-formed sequences only (no overlong forms, no surrogates), so that the JSON parser,
// which is as strict, never rejects a whole reply for one bad byte in a name.
std::string sanitize_utf8(const std::string &s) { return support::sanitize_utf8(s, support::BadUtf8::QuestionMark); }

std::string clean_name(const std::string &raw, size_t max) {
    const std::string s = sanitize_utf8(raw.size() > 4096 ? raw.substr(0, 4096) : raw);
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (i + len > s.size()) len = s.size() - i;
        const std::string cp = s.substr(i, len);
        i += len;
        if (len == 1) {
            if (c == '\t' || c == '\n' || c == '\r') { out += ' '; continue; }
            if (c < 0x20 || c == 0x7f) continue;                         // C0 controls, DEL
        } else if (len == 2) {
            if (c == 0xC2 && static_cast<unsigned char>(cp[1]) < 0xA0) continue;   // C1 controls U+0080..U+009F
        } else if (len == 3 && c == 0xE2 && static_cast<unsigned char>(cp[1]) == 0x80) {
            const unsigned char t = static_cast<unsigned char>(cp[2]);
            // U+200B..U+200F (zero width, LRM, RLM), U+2028/U+2029, U+202A..U+202E (embeddings, overrides)
            if ((t >= 0x8B && t <= 0x8F) || (t >= 0xA8 && t <= 0xAE)) continue;
        } else if (len == 3 && c == 0xE2 && static_cast<unsigned char>(cp[1]) == 0x81) {
            const unsigned char t = static_cast<unsigned char>(cp[2]);
            if (t >= 0xA6 && t <= 0xA9) continue;                        // U+2066..U+2069 isolates
        } else if (len == 3 && c == 0xEF && cp == "\xEF\xBB\xBF") {
            continue;                                                    // U+FEFF
        }
        out += cp;
    }
    // Trim, squeeze runs of spaces, cap without cutting a character.
    std::string t;
    for (const char c : out) {
        if (c == ' ' && (t.empty() || t.back() == ' ')) continue;
        t += c;
    }
    while (!t.empty() && t.back() == ' ') t.pop_back();
    if (t.size() > max) {
        t = support::utf8_prefix(t, max);
        while (!t.empty() && t.back() == ' ') t.pop_back();
    }
    return t;
}

bool too_deep(const std::string &text, int max) { return support::json_too_deep(text, max); }

static bool charset_ok(const std::string &s, size_t max, const char *extra) {
    if (s.empty() || s.size() > max) return false;
    for (const char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!isalnum(u) && !strchr(extra, c)) return false;
    }
    return true;
}
bool safe_id(const std::string &id) { return charset_ok(id, 64, "-_"); }
bool safe_key(const std::string &key) { return charset_ok(key, 128, "-_"); }
bool safe_pin(const std::string &pin) {
    if (pin.size() != 8 + 44 || pin.compare(0, 8, "sha256//") != 0) return false;
    const std::string b = pin.substr(8);
    if (b.back() != '=') return false;
    return charset_ok(b.substr(0, 43), 43, "+/");
}

std::string normalize_bridge_id(const std::string &id) {
    std::string s;
    for (const char c : id) {
        if (c == ':' || c == '-') continue;
        if (!isxdigit(static_cast<unsigned char>(c))) return "";
        s += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    if (s.size() == 16 && s.compare(6, 4, "fffe") == 0) return s.substr(0, 6) + s.substr(10);
    if (s.size() == 12 || s.size() == 16) return s;
    return "";
}

// ---------------------------------------------------------------- the bridge's certificate
std::string pin_from_pem(const std::string &pem) {
    const support::Certificate x = support::certificate_from_pem(pem);
    if (!x) return "";
    const X509_PUBKEY *pk = X509_get_X509_PUBKEY(x.get());   // const in OpenSSL 4, not in 3: both fit
    if (!pk) return "";
    unsigned char *der = nullptr;
    const int len = i2d_X509_PUBKEY(pk, &der);   // the certificate's own encoding, as libcurl pins it
    if (len <= 0 || !der) return "";
    const std::string pin = support::sha256_pin(der, static_cast<size_t>(len));
    OPENSSL_free(der);
    return pin;
}

std::vector<std::string> names_from_pem(const std::string &pem) {
    std::vector<std::string> out;
    const support::Certificate x = support::certificate_from_pem(pem);
    if (!x) return out;
    const X509_NAME *subject = X509_get_subject_name(x.get());
    for (int pos = -1; subject && out.size() < 8;) {
        pos = X509_NAME_get_index_by_NID(subject, NID_commonName, pos);
        if (pos < 0) break;
        const X509_NAME_ENTRY *e = X509_NAME_get_entry(subject, pos);
        const ASN1_STRING *d = e ? X509_NAME_ENTRY_get_data(e) : nullptr;
        unsigned char *utf8 = nullptr;
        const int n = d ? ASN1_STRING_to_UTF8(&utf8, d) : -1;
        if (n > 0 && utf8) out.emplace_back(reinterpret_cast<const char *>(utf8), static_cast<size_t>(n));
        OPENSSL_free(utf8);
    }
    GENERAL_NAMES *alt = static_cast<GENERAL_NAMES *>(X509_get_ext_d2i(x.get(), NID_subject_alt_name, nullptr, nullptr));
    if (alt) {
        const int n = sk_GENERAL_NAME_num(alt);
        for (int i = 0; i < n && i < 16; ++i) {
            int type = 0;
            void *v = GENERAL_NAME_get0_value(sk_GENERAL_NAME_value(alt, i), &type);
            if (type != GEN_DNS || !v) continue;
            // ASN1_STRING_to_UTF8 rather than ASN1_STRING_length, which OpenSSL 4.1 deprecates
            // in favour of a function OpenSSL 3 does not have.
            unsigned char *utf8 = nullptr;
            const int len = ASN1_STRING_to_UTF8(&utf8, static_cast<const ASN1_STRING *>(v));
            if (len > 0 && len < 256 && utf8) out.emplace_back(reinterpret_cast<const char *>(utf8), static_cast<size_t>(len));
            OPENSSL_free(utf8);
        }
        GENERAL_NAMES_free(alt);
    }
    return out;
}

bool cert_names_bridge(const std::string &pem, const std::string &bridge_id) {
    const std::string want = normalize_bridge_id(bridge_id);
    if (want.empty()) return false;
    for (const auto &n : names_from_pem(pem))
        if (normalize_bridge_id(n) == want) return true;
    return false;
}

// ---------------------------------------------------------------- colour
int mirek_from_kelvin(int kelvin, int lo, int hi) {
    if (lo <= 0 || hi < lo) { lo = 153; hi = 500; }
    return clampi(color::mired_from_kelvin(clampi(kelvin, 1000, 40000)), lo, hi);
}

int kelvin_from_mirek(double mirek) {
    if (!std::isfinite(mirek) || mirek < 1 || mirek > 100000) return 0;
    return color::kelvin_from_mired(static_cast<int>(std::lround(mirek)));
}

void hs_to_xy(double hue, double sat, double &x, double &y) {
    if (!std::isfinite(hue)) hue = 0;
    if (!std::isfinite(sat)) sat = 0;
    color::hs_to_xy(hue, sat, x, y);
    x = round_scaled(std::max(0.0, std::min(1.0, x)), 1e4);
    y = round_scaled(std::max(0.0, std::min(1.0, y)), 1e4);
}

// ---------------------------------------------------------------- replies
bool parse_json(const std::string &body, json &out) {
    if (body.empty() || too_deep(body)) return false;
    out = json::parse(sanitize_utf8(body), nullptr, false);
    return !out.is_discarded();
}

LinkReply read_link_reply(const std::string &body, std::string &key) {
    key.clear();
    json j;
    if (!parse_json(body, j) || !j.is_array() || j.empty() || !j[0].is_object()) return LinkReply::Unreadable;
    const json &first = j[0];
    const json &success = field(first, "success");
    if (success.is_object()) {
        const std::string k = str(success, "username");
        if (!safe_key(k)) return LinkReply::Unreadable;
        key = k;
        return LinkReply::Key;
    }
    const json &error = field(first, "error");
    if (error.is_object()) {
        double type = 0;
        if (num(error, "type", type) && type == 101) return LinkReply::NotPressed;
        return LinkReply::Refused;
    }
    return LinkReply::Unreadable;
}

bool read_config(const std::string &body, Config &out) {
    out = Config();
    json j;
    if (!parse_json(body, j) || !j.is_object()) return false;
    out.bridgeid = str(j, "bridgeid");
    out.modelid = clean_name(str(j, "modelid"), 16);
    out.name = clean_name(str(j, "name"));
    const std::string sw = str(j, "swversion");
    if (!sw.empty() && sw.size() <= 18 && std::all_of(sw.begin(), sw.end(), [](char c) { return c >= '0' && c <= '9'; }))
        out.swversion = std::stoll(sw);
    return !normalize_bridge_id(out.bridgeid).empty();
}

// ---------------------------------------------------------------- things
void split_resources(const json &data, Snapshot &out) {
    out = Snapshot();
    if (!data.is_array()) return;
    const std::pair<const char *, json *> kinds[] = {
        {"device", &out.devices}, {"light", &out.lights},     {"room", &out.rooms},   {"zone", &out.zones},
        {"scene", &out.scenes},   {"bridge_home", &out.homes}, {"temperature", &out.temperatures},
        {"motion", &out.motions}};
    for (const auto &r : data) {
        const std::string type = str(r, "type");
        for (const auto &k : kinds)
            if (type == k.first && k.second->size() < kMaxResources) k.second->push_back(r);
    }
}

std::string fingerprint(std::vector<std::string> items) {
    std::sort(items.begin(), items.end());
    uint64_t h = 1469598103934665603ULL;
    for (const auto &s : items) {
        for (const unsigned char c : s) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        h ^= '\n';
        h *= 1099511628211ULL;
    }
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

std::string group_fingerprint(const json &group) {
    std::vector<std::string> items;
    for (const auto &c : array(group, "children")) {
        if (items.size() >= kMaxResources) break;
        const std::string rid = str(c, "rid"), rtype = str(c, "rtype");
        if (safe_id(rid) && safe_id(rtype)) items.push_back(rtype + "/" + rid);
    }
    return fingerprint(items);
}

json light_state(const json &light) {
    json st = json::object();
    bool on = false;
    if (boolean(field(light, "on"), "on", on)) st["on"] = on;
    double b = 0;
    if (num(field(light, "dimming"), "brightness", b)) {
        int pct = clampi(std::lround(b), 0, 100);
        if (pct == 0 && b > 0) pct = 1;   // the lowest level is still on
        st["brightness"] = pct;
    }
    const json &ct = field(light, "color_temperature");
    bool valid = false;
    double mirek = 0;
    if (boolean(ct, "mirek_valid", valid) && valid && num(ct, "mirek", mirek) && kelvin_from_mirek(mirek) > 0) {
        st["kelvin"] = kelvin_from_mirek(mirek);
    } else {
        const json &xy = field(field(light, "color"), "xy");
        double x = 0, y = 0;
        if (num(xy, "x", x) && num(xy, "y", y) && x >= 0 && x <= 1 && y > 0 && y <= 1) {
            double h = 0, s = 0;
            color::xy_to_hs(x, y, h, s);
            if (std::isfinite(h) && std::isfinite(s)) {
                st["hue"] = clampi(std::lround(h), 0, 360) % 360;
                st["saturation"] = clampi(std::lround(s), 0, 100);
            }
        }
    }
    return st;
}

json sensor_state(const json *temperature, const json *motion) {
    json st = json::object();
    bool enabled = true, valid = true;
    if (temperature && !(boolean(*temperature, "enabled", enabled) && !enabled)) {
        const json &t = field(*temperature, "temperature");
        double c = 0;
        const bool have = num(field(t, "temperature_report"), "temperature", c) || num(t, "temperature", c);
        if (have && !(boolean(t, "temperature_valid", valid) && !valid) && c > -60 && c < 100) st["current_c"] = round_scaled(c, 10);
    }
    enabled = true;
    valid = true;
    if (motion && !(boolean(*motion, "enabled", enabled) && !enabled)) {
        const json &m = field(*motion, "motion");
        bool moving = false;
        const bool have = boolean(field(m, "motion_report"), "motion", moving) || boolean(m, "motion", moving);
        if (have && !(boolean(m, "motion_valid", valid) && !valid)) st["motion"] = moving;
    }
    return st;
}

namespace {
// Services that mark a device as part of a security system, a camera, a siren or a doorbell.
// Such a device is never exposed, not even its light or its temperature.
bool denied_service(const std::string &rtype) {
    static const char *const kDenied[] = {"camera_motion", "bell_button", "speaker",  "contact",
                                          "tamper",        "security_area_motion", "siren", "alarm"};
    for (const char *d : kDenied)
        if (rtype == d) return true;
    return false;
}

struct GroupInfo {
    std::string rtype, rid, name, gl, fp;
    std::vector<std::string> lights;
};
}  // namespace

namespace {
// What build_things() reads before it makes any thing: the devices (and those never exposed), every
// light service, and the rooms, zones and whole home with the lights each holds. Its parts keep the
// names they had as one function.
struct Inventory {
    // Every light service, wanted or not: a group command reaches all of them, so all count
    // towards a group's membership even when one of them is never offered as a thing.
    struct L {
        const json *res;
        std::string id, device;
    };
    std::map<std::string, const json *> devices;
    std::set<std::string> denied;
    std::vector<L> lights;
    std::set<std::string> light_ids;
    std::vector<GroupInfo> groups;
    std::map<std::string, size_t> room_of_device;            // device id -> index in groups
    std::map<std::string, std::vector<size_t>> zones_of_light;
    long home = -1;   // the whole home, an index in groups (-1: none)

    const json *device_of(const std::string &id) const {
        const auto it = devices.find(id);
        return it == devices.end() ? nullptr : it->second;
    }
    long add_group(const json &g, const char *rtype) {
        const std::string rid = str(g, "id");
        if (!safe_id(rid) || groups.size() >= kMaxResources) return -1;
        GroupInfo gi;
        gi.rtype = rtype;
        gi.rid = rid;
        gi.name = clean_name(str(field(g, "metadata"), "name"));
        for (const auto &svc : array(g, "services"))
            if (str(svc, "rtype") == "grouped_light" && safe_id(str(svc, "rid"))) { gi.gl = str(svc, "rid"); break; }
        gi.fp = group_fingerprint(g);
        groups.push_back(gi);
        return static_cast<long>(groups.size() - 1);
    }
    json group_ref(const GroupInfo &g) const {
        return json{{"rtype", g.rtype}, {"rid", g.rid}, {"gl", g.gl}, {"n", g.lights.size()}, {"fp", g.fp}};
    }
    // A smart plug is a light service whose archetype (or its device's) is "plug".
    bool is_plug(const L &l) const {
        const json *dev = device_of(l.device);
        return str(field(*l.res, "metadata"), "archetype") == "plug" || (dev && str(field(*dev, "metadata"), "archetype") == "plug");
    }

    void read_devices(const Snapshot &s, BridgeInfo &info) {
        for (const auto &d : s.devices) {
            if (devices.size() >= kMaxResources) break;
            const std::string id = str(d, "id");
            if (!safe_id(id)) continue;
            devices[id] = &d;
            for (const auto &svc : array(d, "services")) {
                const std::string rtype = str(svc, "rtype");
                if (denied_service(rtype)) denied.insert(id);
                if (rtype == "bridge" && info.name.empty()) {
                    info.name = clean_name(str(field(d, "metadata"), "name"));
                    info.model = clean_name(str(field(d, "product_data"), "model_id"), 16);
                }
            }
        }
    }

    void read_lights(const Snapshot &s) {
        for (const auto &l : s.lights) {
            if (lights.size() >= kMaxResources) break;
            const std::string id = str(l, "id");
            if (!safe_id(id) || !light_ids.insert(id).second) continue;
            lights.push_back({&l, id, str(field(l, "owner"), "rid")});
        }
    }

    void read_groups(const Snapshot &s) {
        for (const auto &r : s.rooms) {
            const long gi = add_group(r, "room");
            if (gi < 0) continue;
            for (const auto &c : array(r, "children"))
                if (str(c, "rtype") == "device" && safe_id(str(c, "rid"))) room_of_device.emplace(str(c, "rid"), static_cast<size_t>(gi));
        }
        for (const auto &z : s.zones) {
            const long gi = add_group(z, "zone");
            if (gi < 0) continue;
            for (const auto &c : array(z, "children"))
                if (str(c, "rtype") == "light" && light_ids.count(str(c, "rid"))) {
                    auto &v = zones_of_light[str(c, "rid")];
                    if (std::find(v.begin(), v.end(), static_cast<size_t>(gi)) == v.end()) v.push_back(static_cast<size_t>(gi));
                }
        }
        // The whole home: every light on the bridge.
        if (!s.homes.empty()) {
            home = add_group(s.homes[0], "bridge_home");
            if (home >= 0) {
                std::vector<std::string> all;
                for (const auto &l : lights) all.push_back(l.id);
                groups[static_cast<size_t>(home)].fp = fingerprint(all);
            }
        }
        for (const auto &l : lights) {
            const auto r = room_of_device.find(l.device);
            if (r != room_of_device.end()) groups[r->second].lights.push_back(l.id);
            for (const size_t z : zones_of_light[l.id]) groups[z].lights.push_back(l.id);
            if (home >= 0) groups[static_cast<size_t>(home)].lights.push_back(l.id);
        }
    }

    // Lights and plugs.
    void add_lights(std::vector<home::Thing> &out) {
        for (const auto &l : lights) {
            if (out.size() >= kMaxThings) break;
            if (denied.count(l.device)) continue;
            const json &res = *l.res;
            bool on = false;
            if (!boolean(field(res, "on"), "on", on)) continue;   // not a light this driver understands
            const json *dev = device_of(l.device);
            home::Thing t;
            t.id = l.id;
            t.name = clean_name(str(field(res, "metadata"), "name"));
            if (t.name.empty() && dev) t.name = clean_name(str(field(*dev, "metadata"), "name"));
            if (t.name.empty()) t.name = "Hue light";
            t.type = is_plug(l) ? "plug" : "light";
            t.caps = home::kPower;
            json ref = {{"kind", "light"}, {"rid", l.id}, {"device", safe_id(l.device) ? l.device : std::string()}};
            double b = 0;
            if (num(field(res, "dimming"), "brightness", b)) t.caps |= home::kBrightness;
            const json &schema = field(field(res, "color_temperature"), "mirek_schema");
            if (schema.is_object()) {
                t.caps |= home::kColorTemp;
                double lo = 153, hi = 500;
                num(schema, "mirek_minimum", lo);
                num(schema, "mirek_maximum", hi);
                // Some devices report 0 and 65535 (aiohue corrects the same); keep within what any light takes.
                int ilo = lo < 50 ? 153 : clampi(std::lround(lo), 50, 1000);
                int ihi = hi > 1000 ? 500 : clampi(std::lround(hi), 50, 1000);
                if (ihi < ilo) { ilo = 153; ihi = 500; }
                ref["mirek"] = {ilo, ihi};
            }
            const json &xy = field(field(res, "color"), "xy");
            double x = 0, y = 0;
            if (num(xy, "x", x) && num(xy, "y", y)) t.caps |= home::kColor;
            // Room: the device's room, else the first of its zones by name.
            const auto r = room_of_device.find(l.device);
            json gl = json::array();
            if (r != room_of_device.end()) {
                t.room = groups[r->second].name;
                if (!groups[r->second].gl.empty()) gl.push_back(group_ref(groups[r->second]));
            }
            std::vector<size_t> zones = zones_of_light[l.id];
            std::stable_sort(zones.begin(), zones.end(), [&](size_t a, size_t b2) { return groups[a].name < groups[b2].name; });
            for (const size_t z : zones) {
                if (t.room.empty()) t.room = groups[z].name;
                if (!groups[z].gl.empty()) gl.push_back(group_ref(groups[z]));
            }
            if (home >= 0 && !groups[static_cast<size_t>(home)].gl.empty()) gl.push_back(group_ref(groups[static_cast<size_t>(home)]));
            ref["groups"] = gl;
            t.ref = ref;
            t.state = light_state(res);
            out.push_back(t);
        }
    }

    void add_scenes(const Snapshot &s, std::vector<home::Thing> &out) const {
        // Scenes, unless one of their lights belongs to a device that is never exposed, or one of them
        // switches on a smart plug: the Hue app puts a room's plugs in the scenes made for that room,
        // and a plug may feed anything (a heater), which the app asks about before switching it on
        // (Thing::ask_first) while a scene would switch it on without that question.
        std::map<std::string, std::string> group_name;
        for (const auto &g : groups)
            if (g.rtype != "bridge_home") group_name[g.rid] = g.name;
        std::map<std::string, std::string> device_of_light;
        std::set<std::string> plugs;
        for (const auto &l : lights) {
            device_of_light[l.id] = l.device;
            if (is_plug(l)) plugs.insert(l.id);
        }
        size_t scenes = 0;
        for (const auto &sc : s.scenes) {
            if (out.size() >= kMaxThings || ++scenes > kMaxResources) break;
            const std::string id = str(sc, "id");
            const std::string name = clean_name(str(field(sc, "metadata"), "name"));
            // A scene whose members cannot be read is not offered: one of them might be a light
            // that is never exposed.
            if (!safe_id(id) || name.empty() || !field(sc, "actions").is_array()) continue;
            bool ok = true;
            for (const auto &a : array(sc, "actions")) {
                const json &target = field(a, "target");
                const std::string rid = str(target, "rid");
                const auto it = device_of_light.find(rid);
                if (str(target, "rtype") != "light" || it == device_of_light.end() || denied.count(it->second)) { ok = false; break; }
                bool on = true;
                const bool says_off = boolean(field(field(a, "action"), "on"), "on", on) && !on;
                if (plugs.count(rid) && !says_off) { ok = false; break; }
            }
            if (!ok) continue;
            home::Thing t;
            t.id = id;
            t.name = name;
            t.type = "scene";
            t.caps = home::kScene;
            const auto g = group_name.find(str(field(sc, "group"), "rid"));
            if (g != group_name.end()) t.room = g->second;
            t.ref = {{"kind", "scene"}, {"rid", id}};
            t.state = json::object();
            out.push_back(t);
        }
    }

    void add_sensors(const Snapshot &s, std::vector<home::Thing> &out) const {
        // Sensors: one thing per device that has a temperature or a motion service.
        std::map<std::string, const json *> temps, motions;
        for (const auto &r : s.temperatures)
            if (temps.size() < kMaxResources && safe_id(str(r, "id"))) temps.emplace(str(r, "id"), &r);
        for (const auto &r : s.motions)
            if (motions.size() < kMaxResources && safe_id(str(r, "id"))) motions.emplace(str(r, "id"), &r);
        for (const auto &d : devices) {
            if (out.size() >= kMaxThings) break;
            if (denied.count(d.first)) continue;
            const json *tres = nullptr, *mres = nullptr;
            std::string trid, mrid;
            for (const auto &svc : array(*d.second, "services")) {
                const std::string rtype = str(svc, "rtype"), rid = str(svc, "rid");
                if (rtype == "temperature" && !tres && temps.count(rid)) { tres = temps[rid]; trid = rid; }
                if (rtype == "motion" && !mres && motions.count(rid)) { mres = motions[rid]; mrid = rid; }
            }
            if (!tres && !mres) continue;
            home::Thing t;
            t.id = d.first;
            t.name = clean_name(str(field(*d.second, "metadata"), "name"));
            if (t.name.empty()) t.name = "Hue sensor";
            t.type = "sensor";
            t.caps = home::kSensor;
            const auto r = room_of_device.find(d.first);
            if (r != room_of_device.end()) t.room = groups[r->second].name;
            t.ref = {{"kind", "sensor"}, {"device", d.first}};
            if (tres) t.ref["temperature"] = trid;
            if (mres) t.ref["motion"] = mrid;
            t.state = sensor_state(tres, mres);
            out.push_back(t);
        }
    }
};
}  // namespace

void build_things(const Snapshot &s, std::vector<home::Thing> &out, BridgeInfo &info) {
    out.clear();
    info = BridgeInfo();
    Inventory inv;
    inv.read_devices(s, info);
    inv.read_lights(s);
    inv.read_groups(s);
    inv.add_lights(out);
    inv.add_scenes(s, out);
    inv.add_sensors(s, out);
}

// ---------------------------------------------------------------- changes
namespace {
bool only_light_fields(const home::Change &c) {
    return !c.target_c && !c.mode && !c.position && !c.fan && !c.volume && !c.volume_step && !c.mute && !c.transport &&
           !c.source && !c.key;
}

void mirek_range(const home::Thing &t, int &lo, int &hi) {
    const json &m = field(t.ref, "mirek");
    lo = 153;
    hi = 500;
    if (m.is_array() && m.size() == 2 && m[0].is_number_integer() && m[1].is_number_integer()) {
        const long long a = m[0].get<long long>(), b = m[1].get<long long>();
        if (a >= 50 && b <= 1000 && a <= b) {
            lo = static_cast<int>(a);
            hi = static_cast<int>(b);
        }
    }
}

// The checks shared by one light and a group of them.
bool check_light_change(const home::Thing &t, const home::Change &c, std::string &why) {
    if (str(t.ref, "kind") != "light" || !safe_id(str(t.ref, "rid")) || (t.type != "light" && t.type != "plug")) {
        why = "this is not a Hue light";
        return false;
    }
    if (!only_light_fields(c)) {
        why = t.type == "plug" ? "this plug can only be switched on and off" : "a Hue light can only be switched, dimmed or coloured";
        return false;
    }
    if (!c.on && !c.brightness && !c.kelvin && !c.hs) {
        why = "nothing was asked of it";
        return false;
    }
    if (c.kelvin && c.hs) {
        why = "a light cannot take a colour and a shade of white at once";
        return false;
    }
    const bool switching_off = (c.on && !*c.on) || (c.brightness && *c.brightness <= 0 && !(c.on && *c.on));
    if (switching_off) return true;   // whatever else was asked, it is going off
    if (c.brightness && !(t.caps & home::kBrightness)) {
        why = t.type == "plug" ? "this plug can only be switched on and off" : "it cannot be dimmed";
        return false;
    }
    if (c.kelvin && (!(t.caps & home::kColorTemp) || *c.kelvin <= 0)) {
        why = *c.kelvin <= 0 ? "that is not a shade of white" : "it cannot be set to a shade of white";
        return false;
    }
    if (c.hs && !(t.caps & home::kColor)) {
        why = "its colour cannot be changed";
        return false;
    }
    return true;
}

void fill_body(const home::Change &c, int lo, int hi, json &body) {
    body = json::object();
    const bool off_by_brightness = c.brightness && *c.brightness <= 0 && !(c.on && *c.on);
    const bool off = (c.on && !*c.on) || off_by_brightness;
    if (off) {
        body["on"] = {{"on", false}};   // nothing else: values sent to a light that is off do not stick
        return;
    }
    if (c.on) body["on"] = {{"on", true}};
    if (c.brightness) body["dimming"] = {{"brightness", clampi(*c.brightness, 0, 100)}};   // 0 with on: the lowest level
    if (c.kelvin) body["color_temperature"] = {{"mirek", mirek_from_kelvin(*c.kelvin, lo, hi)}};
    if (c.hs) {
        double x = 0, y = 0;
        hs_to_xy(c.hs->first, c.hs->second, x, y);
        body["color"] = {{"xy", {{"x", x}, {"y", y}}}};
    }
}
}  // namespace

bool light_body(const home::Thing &t, const home::Change &c, json &body, std::string &why) {
    if (!check_light_change(t, c, why)) return false;
    int lo = 153, hi = 500;
    mirek_range(t, lo, hi);
    fill_body(c, lo, hi, body);
    return true;
}

bool scene_body(const home::Thing &t, const home::Change &c, json &body, std::string &why) {
    if (str(t.ref, "kind") != "scene" || !safe_id(str(t.ref, "rid"))) {
        why = "this is not a Hue scene";
        return false;
    }
    const bool only_on = c.on && *c.on && !c.brightness && !c.kelvin && !c.hs && only_light_fields(c);
    if (!only_on) {
        why = "a scene can only be started";
        return false;
    }
    body = {{"recall", {{"action", "active"}}}};
    return true;
}

namespace {
const char *const kSceneChanged =
    "this scene has changed on the Hue Bridge since the device list was read; refresh the device list in Settings, Smart home";
const char *const kSceneDisabled =
    "this scene also changes a light the assistant may not use; allow that light in Settings, Smart home, or switch the "
    "lights by name instead";
const char *const kScenePlug =
    "this scene also switches on a smart plug, and a plug is only switched on by name; switch the lights on by name instead";
const char *const kSceneAskFirst =
    "this scene also switches on a light that is set to be switched on only after the user says yes; switch it on by name "
    "instead";
}  // namespace

bool scene_allowed(const json &scene, const std::vector<home::Thing> &things, std::string &why) {
    const json &actions = field(scene, "actions");
    if (!actions.is_array() || actions.size() > kMaxResources) {
        why = kSceneChanged;
        return false;
    }
    std::map<std::string, const home::Thing *> lights;   // by light id
    for (const auto &t : things)
        if (str(t.ref, "kind") == "light" && (t.type == "light" || t.type == "plug") && safe_id(str(t.ref, "rid")))
            lights.emplace(str(t.ref, "rid"), &t);
    for (const auto &a : actions) {
        const json &target = field(a, "target");
        const auto it = lights.find(str(target, "rid"));
        // A light that is no thing: one added since the list was read, or one of a device that is
        // never exposed (a camera's floodlight). Either way the list must be read again.
        if (str(target, "rtype") != "light" || it == lights.end()) {
            why = kSceneChanged;
            return false;
        }
        const home::Thing &t = *it->second;
        if (!t.enabled) {
            why = kSceneDisabled;
            return false;
        }
        bool on = true;
        if (boolean(field(field(a, "action"), "on"), "on", on) && !on) continue;   // switched off: safe
        if (t.type == "plug") {
            why = kScenePlug;
            return false;
        }
        if (t.ask_first) {
            why = kSceneAskFirst;
            return false;
        }
    }
    return true;
}

bool pick_group(const std::vector<const home::Thing *> &things, const home::Change &c, Group &out) {
    if (things.size() < 2) return false;
    std::set<std::string> rids;
    std::string why;
    for (const home::Thing *t : things) {
        if (!t || !check_light_change(*t, c, why)) return false;
        if (!rids.insert(str(t->ref, "rid")).second) return false;
    }
    auto entry_of = [](const json &g, Group &e) {
        e.rtype = str(g, "rtype");
        e.rid = str(g, "rid");
        e.gl = str(g, "gl");
        e.fp = str(g, "fp");
        const json &n = field(g, "n");
        e.n = n.is_number_integer() ? clampi(n.get<long long>(), 0, 100000) : 0;
        return (e.rtype == "room" || e.rtype == "zone" || e.rtype == "bridge_home") && safe_id(e.rid) && safe_id(e.gl) &&
               e.fp.size() == 16 && e.n > 0;
    };
    for (const auto &g : array(things[0]->ref, "groups")) {
        Group cand;
        if (!entry_of(g, cand) || static_cast<size_t>(cand.n) != things.size()) continue;
        bool all = true;
        for (const home::Thing *t : things) {
            bool found = false;
            for (const auto &h : array(t->ref, "groups")) {
                Group e;
                if (entry_of(h, e) && e.rid == cand.rid && e.gl == cand.gl && e.fp == cand.fp && e.n == cand.n) { found = true; break; }
            }
            if (!found) { all = false; break; }
        }
        if (all) {
            out = cand;
            return true;
        }
    }
    return false;
}

bool group_body(const std::vector<const home::Thing *> &things, const home::Change &c, json &body, std::string &why) {
    int lo = 0, hi = 100000;
    for (const home::Thing *t : things) {
        if (!t || !check_light_change(*t, c, why)) return false;
        int a = 153, b = 500;
        mirek_range(*t, a, b);
        lo = std::max(lo, a);
        hi = std::min(hi, b);
    }
    if (lo > hi) { lo = 153; hi = 500; }
    fill_body(c, lo, hi, body);
    return true;
}

std::string describe_bridge_error(const std::string &description, bool group) {
    const std::string d = lower(description);
    if (d.find("communication issues") != std::string::npos)
        return group ? "the Hue Bridge has lost contact with some of these lights; they may be switched off at the wall"
                     : "the Hue Bridge has lost contact with this light; it may be switched off at the wall";
    if (d.find("soft off") != std::string::npos)
        return group ? "some of these lights are off, so the change did not take effect on them; switch them on at the same time"
                     : "the light is off, so the change did not take effect; switch it on at the same time";
    return "the Hue Bridge did not accept the change";
}

void set_pair_window_ms(int ms) { g_pair_window_ms = std::max(0, ms); }
void set_pair_interval_ms(int ms) { g_pair_interval_ms = std::max(10, ms); }

}  // namespace detail
}  // namespace hue

// ==================================================================== the driver
namespace {

using hue::json;
using namespace hue::detail;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

const char *const kKind = "hue_bridge";
// "<app name>#<device name>", at most 20 and 19 characters (Signify's limits for devicetype).
const char *const kDeviceType = "zero_bot#cardputer";

// What the settings page shows while pairing. The README quotes the first.
const char *const kPromptPair =
    "Press the round button on the Hue Bridge, then confirm here. Zero Bot keeps trying to connect for 30 seconds.";
const char *const kPromptRefused =
    "The Hue Bridge no longer accepts Zero Bot; it may have been removed in the Hue app. Press the round button on the Hue "
    "Bridge, then confirm here to connect again.";
const char *const kPromptCertChanged =
    "The Hue Bridge's security certificate has changed. A software update can do this, but so could another device "
    "pretending to be the bridge. To confirm it is your bridge, press the round button on it, then confirm here.";

const char *const kPairBadAddress = "The Hue Bridge's address is not valid. Search for devices again.";
const char *const kPairNoAnswer = "The Hue Bridge did not answer. Check that it is switched on and connected to the router, then try again.";
const char *const kPairNotHue = "The device at this address does not answer like a Hue Bridge.";
const char *const kPairOld = "This Hue Bridge needs a software update before Zero Bot can use it. Update it with the Hue app, then try again.";
const char *const kPairV1 = "This is the first, round Hue Bridge, which Zero Bot cannot control. It works with the square Hue Bridge and the Hue Bridge Pro.";
const char *const kPairWrongBridge = "The device at this address is a different Hue Bridge from the one that was found. Search for devices again.";
const char *const kPairWrongCert = "This device's security certificate does not belong to the Hue Bridge that was found, so Zero Bot did not connect to it.";
const char *const kPairNoCert = "Zero Bot could not read the Hue Bridge's security certificate, so it did not connect to it.";
const char *const kPairCertMoved = "The Hue Bridge presented a different security certificate partway through pairing, so nothing was sent. Try again.";
const char *const kPairTimeout = "The button on the Hue Bridge was not pressed in time. Press it, then try again.";
const char *const kPairRefused = "The Hue Bridge refused to connect Zero Bot. Try again in a few minutes.";
const char *const kPairGarbled = "The Hue Bridge sent a reply that could not be understood.";
const char *const kPairStopped = "The Hue Bridge stopped answering while pairing. Try again.";
const char *const kPairFailed = "Pairing with the Hue Bridge did not work. Try again.";

const char *const kErrNotPaired = "the Hue Bridge is not paired yet; pair it in Settings, Smart home";
const char *const kErrRefused =
    "the Hue Bridge no longer accepts Zero Bot's key; pair it again in Settings, Smart home";
const char *const kErrCertChanged =
    "the Hue Bridge's security certificate has changed, so nothing was sent to it; confirm the bridge in Settings, Smart home";
const char *const kErrAddress = "the Hue Bridge's saved address is not valid";
const char *const kErrNoAnswer = "the Hue Bridge did not answer";
const char *const kErrTimeout = "the Hue Bridge did not answer in time";
const char *const kErrMoved = "the Hue Bridge did not answer at its saved address";
const char *const kErrGarbled = "the Hue Bridge sent a reply that could not be understood";
const char *const kErrBusy = "the Hue Bridge is busy; try again in a moment";
const char *const kErrRejected = "the Hue Bridge did not accept the change";
const char *const kErrGoneLight = "the Hue Bridge no longer has this light; refresh the device list in Settings, Smart home";
const char *const kErrGoneScene = "the Hue Bridge no longer has this scene; refresh the device list in Settings, Smart home";
const char *const kErrSensor = "this is a sensor; it can only be read";
const char *const kErrUnknown = "Zero Bot does not know this Hue item; refresh the device list in Settings, Smart home";
const char *const kErrInternal = "something went wrong while talking to the Hue Bridge";

constexpr int kReadDeadlineMs = 4000;
constexpr int kDefaultApplyMs = 2500;
constexpr long kConnectMs = 3000;
constexpr size_t kMaxBody = 4 * 1024 * 1024;

// http::send's words for a peer whose public key is not the pinned one (CURLE_SSL_PINNEDPUBKEYNOTMATCH),
// for a transfer that ran out of time, and for one that never reached the peer.
const char *const kHttpPinMismatch = "the device's certificate key has changed";
const char *const kHttpTimedOut = "timed out";
const char *const kHttpNoConnection = "no connection";

// ---------------------------------------------------------------- transport
struct Target {
    std::string host;   // as it goes in a URL: "192.0.2.7" or "[2001:db8::7]"
    int port = 443;
};

bool make_target(const home::Device &dev, Target &t) {
    in_addr a4{};
    in6_addr a6{};
    if (inet_pton(AF_INET, dev.ip.c_str(), &a4) == 1) t.host = dev.ip;
    else if (inet_pton(AF_INET6, dev.ip.c_str(), &a6) == 1) t.host = "[" + dev.ip + "]";
    else return false;   // never a host name: no DNS lookup is ever made
    t.port = dev.port > 0 && dev.port < 65536 ? dev.port : 443;
    return true;
}

long ms_left(Clock::time_point deadline) {
    return static_cast<long>(std::chrono::duration_cast<milliseconds>(deadline - Clock::now()).count());
}

struct Reply {
    long status = 0;          // 0 when no status line arrived
    std::string body, error;  // error: http::send's transport failure, "" when a whole answer arrived
    std::string pem;          // the peer's certificate, when it was asked for
    bool ok() const { return error.empty(); }
    bool pin_mismatch() const { return error == kHttpPinMismatch; }
};

// One request, one connection (http::send gives a request with TLS options a connection of its
// own and closes it). A request that carries the application key is only ever made with the pin.
// Never through a proxy, never following a redirect, http and https only (Request::local).
Reply call(const Target &t, const char *method, const std::string &path, const std::string *body, const std::string &pin,
           const std::string &key, Clock::time_point deadline, bool want_cert = false) {
    Reply r;
    if (!key.empty() && !safe_pin(pin)) {   // never send the key without the pin
        r.error = "no pin";
        return r;
    }
    const long left = ms_left(deadline);
    if (left < 50) {
        r.error = kHttpTimedOut;
        return r;
    }
    http::Request q;
    q.method = method;
    q.url = "https://" + t.host + ":" + std::to_string(t.port) + path;
    q.local = true;
    // The bridge's certificate is not issued by a public authority and does not name its
    // address, so ordinary verification cannot pass; the pinned key takes its place (libcurl
    // checks the pin even with verification off).
    q.insecure = true;
    q.pinned_pubkey = pin;
    q.want_peer_cert = want_cert;
    q.no_redirect = true;
    q.timeout_ms = left;
    q.connect_timeout_ms = std::min(left, kConnectMs);
    q.headers.push_back("Accept: application/json");
    if (!key.empty()) q.headers.push_back("hue-application-key: " + key);
    if (body) {
        q.body = *body;
        q.headers.push_back("Content-Type: application/json");
    }
    http::Response res = http::send(q);
    r.status = res.status > 0 ? res.status : 0;
    r.error = res.error;
    r.body = std::move(res.body);
    r.pem = std::move(res.peer_cert_pem);
    if (r.error.empty() && r.body.size() > kMaxBody) r.error = "the reply was too large";
    return r;
}

// What a failed transfer means to a person. A peer that sent a status line did answer: a
// transfer that breaks after that is a reply that could not be understood, not silence (which
// would send the shared layer looking for the bridge at another address).
const char *transfer_error(const Reply &r) {
    if (r.error == kHttpTimedOut) return kErrTimeout;
    if (r.status > 0) return kErrGarbled;
    if (r.error == kHttpNoConnection) return kErrNoAnswer;
    if (r.error == "no pin" || r.error == "could not start HTTP client" || r.error == curl_easy_strerror(CURLE_OUT_OF_MEMORY))
        return kErrInternal;
    for (const CURLcode c : {CURLE_WEIRD_SERVER_REPLY, CURLE_PARTIAL_FILE, CURLE_BAD_CONTENT_ENCODING, CURLE_HTTP2,
                             CURLE_RANGE_ERROR, CURLE_WRITE_ERROR, CURLE_FILESIZE_EXCEEDED})
        if (r.error == curl_easy_strerror(c)) return kErrGarbled;
    return kErrNoAnswer;
}

struct Auth {
    std::string key, pin, bridgeid;
    bool paired() const { return safe_key(key) && safe_pin(pin); }
};

Auth read_auth(const json &a) {
    Auth out;
    if (!a.is_object()) return out;
    auto s = [&](const char *k) {
        const auto it = a.find(k);
        return it != a.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    out.key = s("key");
    out.pin = s("pin");
    out.bridgeid = s("bridgeid");
    return out;
}

std::string info_of(const home::Device &d, const char *key) {
    const auto it = d.info.find(key);
    return it == d.info.end() ? std::string() : it->second;
}

// SHA-256 of an application key, in hex: what is remembered of a key the bridge rejected.
std::string key_hash(const std::string &key) {
    return key.empty() ? "" : support::hex(support::digest(EVP_sha256(), key));   // "" too when libcrypto fails
}

// Parses a CLIP v2 reply: an object whose "data" is an array. False when unusable.
bool v2_data(const std::string &body, json &out) {
    json j;
    if (!parse_json(body, j) || !j.is_object()) return false;
    const auto it = j.find("data");
    if (it == j.end() || !it->is_array()) return false;
    out = std::move(*it);
    return true;
}

// The first "errors" description of a CLIP v2 reply, "" when it has none.
std::string first_error(const std::string &body) {
    json j;
    if (!parse_json(body, j) || !j.is_object()) return "";
    const auto it = j.find("errors");
    if (it == j.end() || !it->is_array() || it->empty() || !(*it)[0].is_object()) return "";
    const auto d = (*it)[0].find("description");
    return d != (*it)[0].end() && d->is_string() ? d->get<std::string>() : std::string("error");
}

class HueDriver : public home::Driver {
public:
    const char *kind() const override { return kKind; }

    bool claims(const std::string &device_kind, const std::map<std::string, std::string> &info) const override {
        if (device_kind != kKind) return false;
        const auto it = info.find("modelid");
        if (it == info.end()) return true;
        std::string m;
        for (const char c : it->second)
            if (!isspace(static_cast<unsigned char>(c))) m += static_cast<char>(toupper(static_cast<unsigned char>(c)));
        return m != "BSB001";   // the round first bridge has no version 2 interface
    }

    home::PairStep next_step(const home::Device &dev) const override {
        home::PairStep s;
        try {
            const Auth a = read_auth(dev.auth);
            s.kind = home::Pairing::PressButton;
            s.wait_s = 30;
            const Flags f = a.paired() ? flags(dev, a) : Flags();
            if (!a.paired()) s.prompt = kPromptPair;
            else if (f.refused) s.prompt = kPromptRefused;
            else if (f.cert_changed) s.prompt = kPromptCertChanged;
            else s = home::PairStep();
        } catch (const std::exception &) {
            s = home::PairStep();
            s.kind = home::Pairing::PressButton;
            s.wait_s = 30;
            s.prompt = kPromptPair;
        }
        return s;
    }

    bool pair(home::Device &dev, const std::string &, std::string &err) override {
        try {
            return pair_inner(dev, err);
        } catch (const std::exception &) {
            err = kPairFailed;
            return false;
        }
    }

    void unpair(home::Device &dev) override {
        // Version 2 has no call that removes an application key (that is done in the Hue app),
        // so unpairing forgets the key, the pin and everything remembered about this bridge.
        try {
            forget(dev);
        } catch (const std::exception &) {
        }
        dev.auth = json::object();
    }

    bool enumerate(home::Device &dev, std::vector<home::Thing> &out, std::string &err) override {
        try {
            return enumerate_inner(dev, out, err);
        } catch (const std::exception &) {
            out.clear();
            err = kErrInternal;
            return false;
        }
    }

    bool read(const home::Device &dev, std::vector<home::Thing> &things, std::string &err) override {
        try {
            return read_inner(dev, things, err);
        } catch (const std::exception &) {
            err = kErrInternal;
            return false;
        }
    }

    bool apply(const home::Device &dev, const home::Thing &thing, const home::Change &change, int deadline_ms,
               std::string &err) override {
        try {
            return apply_inner(dev, thing, change, deadline_ms, err);
        } catch (const std::exception &) {
            err = kErrInternal;
            return false;
        }
    }

    bool apply_many(const home::Device &dev, const std::vector<const home::Thing *> &things, const home::Change &change,
                    int deadline_ms, std::string &err) override {
        try {
            return apply_many_inner(dev, things, change, deadline_ms, err);
        } catch (const std::exception &) {
            err = kErrInternal;
            return false;
        }
    }

private:
    // ---- what is remembered about a bridge: a rejected key, a changed certificate (both also in
    // the family's secrets, so that a later run knows them), when the next command may go
    struct Memo {
        std::set<std::string> refused_keys;   // key_hash() of keys the bridge rejected
        std::string bad_pin;                  // a pin the bridge's certificate no longer matched
        Clock::time_point next_send{};
        Clock::time_point used{};
    };
    struct Flags {
        bool refused = false, cert_changed = false;
    };
    mutable std::mutex mu_;
    std::map<std::string, Memo> memo_;

    static std::string memo_key(const home::Device &dev) {
        return dev.id.empty() ? dev.ip + ":" + std::to_string(dev.port) : dev.id;
    }
    // The family's secrets as an object, {} when there are none (or no store is attached).
    json saved() const {
        if (!secrets_) return json::object();
        json s = secrets_->get(kKind);
        return s.is_object() ? s : json::object();
    }
    static std::string saved_entry(const json &s, const char *what, const std::string &id) {
        const json &m = field(s, what);
        return str(m, id.c_str());
    }
    // Writes one entry of the family's secrets ("" removes it). The caller holds mu_.
    void save_entry(const char *what, const std::string &id, const std::string &value) {
        if (!secrets_) return;
        json s = saved();
        if (!s.contains(what) || !s[what].is_object()) s[what] = json::object();
        if (value.empty()) s[what].erase(id);
        else s[what][id] = value;
        if (s[what].empty()) s.erase(what);
        secrets_->set(kKind, s);
    }
    // The keys refused for one device id, as hashes. One bridge can be saved on two networks (two
    // Wi-Fi names on one router), each saved copy paired with a key of its own, and one secrets file
    // serves both: so an entry is one hash (as first written) or a list, and recording or forgetting
    // one key's refusal leaves the other's alone.
    static std::vector<std::string> saved_refusals(const json &s, const std::string &id) {
        std::vector<std::string> out;
        const json &v = field(field(s, "refused"), id.c_str());
        if (v.is_string()) out.push_back(v.get<std::string>());
        else if (v.is_array())
            for (const auto &x : v)
                if (x.is_string()) out.push_back(x.get<std::string>());
        return out;
    }
    // Writes them (none removes the entry). The caller holds mu_.
    void save_refusals(const std::string &id, const std::vector<std::string> &hashes) {
        if (!secrets_) return;
        json s = saved();
        if (!s.contains("refused") || !s["refused"].is_object()) s["refused"] = json::object();
        if (hashes.empty()) s["refused"].erase(id);
        else if (hashes.size() == 1) s["refused"][id] = hashes[0];
        else s["refused"][id] = hashes;
        if (s["refused"].empty()) s.erase("refused");
        secrets_->set(kKind, s);
    }

    template <typename F>
    void with_memo(const home::Device &dev, F f) {
        std::lock_guard<std::mutex> lk(mu_);
        if (memo_.size() > 256) {   // keep it small: drop the least recently used (what matters is also in the secrets)
            auto oldest = memo_.begin();
            for (auto it = memo_.begin(); it != memo_.end(); ++it)
                if (it->second.used < oldest->second.used) oldest = it;
            memo_.erase(oldest);
        }
        Memo &m = memo_[memo_key(dev)];
        m.used = Clock::now();
        f(m);
    }

    // Whether this pairing's key was rejected or the bridge's certificate changed, in this run
    // or an earlier one.
    Flags flags(const home::Device &dev, const Auth &a) const {
        Flags f;
        const std::string id = memo_key(dev), hash = key_hash(a.key);
        std::lock_guard<std::mutex> lk(mu_);
        const auto it = memo_.find(id);
        if (it != memo_.end()) {
            f.refused = !hash.empty() && it->second.refused_keys.count(hash) > 0;
            f.cert_changed = !a.pin.empty() && it->second.bad_pin == a.pin;
        }
        if (!f.refused || !f.cert_changed) {
            const json s = saved();
            if (!f.refused && !hash.empty()) {
                const std::vector<std::string> refused = saved_refusals(s, id);
                f.refused = std::find(refused.begin(), refused.end(), hash) != refused.end();
            }
            f.cert_changed = f.cert_changed || (!a.pin.empty() && saved_entry(s, "cert_changed", id) == a.pin);
        }
        return f;
    }
    void note_refused(const home::Device &dev, const Auth &a) {
        const std::string hash = key_hash(a.key);
        if (hash.empty()) return;
        with_memo(dev, [&](Memo &m) {
            m.refused_keys.insert(hash);
            std::vector<std::string> refused = saved_refusals(saved(), memo_key(dev));
            if (std::find(refused.begin(), refused.end(), hash) != refused.end()) return;
            refused.push_back(hash);
            if (refused.size() > 8) refused.erase(refused.begin());   // a bridge is not paired eight times over
            save_refusals(memo_key(dev), refused);
        });
    }
    void note_cert_changed(const home::Device &dev, const Auth &a) {
        with_memo(dev, [&](Memo &m) {
            m.bad_pin = a.pin;
            save_entry("cert_changed", memo_key(dev), a.pin);
        });
    }
    // Forgets what was recorded about this saved pairing (its key, its pin), before it is replaced
    // or dropped. Only this pairing's: a copy of the bridge saved on another network keeps the
    // refusal of its own key.
    void forget(const home::Device &dev) {
        const Auth a = read_auth(dev.auth);
        const std::string id = memo_key(dev), hash = key_hash(a.key);
        std::lock_guard<std::mutex> lk(mu_);
        const auto it = memo_.find(id);
        if (it != memo_.end()) {
            it->second.refused_keys.erase(hash);
            if (!a.pin.empty() && it->second.bad_pin == a.pin) it->second.bad_pin.clear();
        }
        const json s = saved();
        std::vector<std::string> refused = saved_refusals(s, id);
        const auto kept = std::remove(refused.begin(), refused.end(), hash);
        if (!hash.empty() && kept != refused.end()) {
            refused.erase(kept, refused.end());
            save_refusals(id, refused);
        }
        if (!a.pin.empty() && saved_entry(s, "cert_changed", id) == a.pin) save_entry("cert_changed", id, "");
    }

    // Waits, within the deadline, until the bridge may take another command (Signify asks for
    // about ten light commands or one group command a second). At least 1 s is left for the request.
    void space(const home::Device &dev, Clock::time_point deadline) {
        Clock::time_point next;
        {
            std::lock_guard<std::mutex> lk(mu_);
            const auto it = memo_.find(memo_key(dev));
            if (it == memo_.end()) return;
            next = it->second.next_send;
        }
        const auto until = std::min(next, deadline - milliseconds(1000));
        if (until > Clock::now()) std::this_thread::sleep_until(until);
    }
    void sent(const home::Device &dev, bool group) {
        with_memo(dev, [&](Memo &m) { m.next_send = Clock::now() + milliseconds(group ? 1000 : 100); });
    }

    // Whether the stored pairing may be used now. Nothing is sent when it may not.
    bool ready(const home::Device &dev, Auth &a, Target &t, std::string &err) const {
        a = read_auth(dev.auth);
        if (!a.paired()) { err = kErrNotPaired; return false; }
        const Flags f = flags(dev, a);
        if (f.refused) { err = kErrRefused; return false; }
        if (f.cert_changed) { err = kErrCertChanged; return false; }
        if (!make_target(dev, t)) { err = kErrAddress; return false; }
        return true;
    }

    // After a pinned request failed because the peer's key differs: asks, without the key, who is
    // at the address. The same bridge with a new certificate is remembered as such (so nothing is
    // sent until the user confirms); anything else reads as "not at its address", the shared
    // layer's cue to look for the bridge elsewhere.
    const char *pin_failure(const home::Device &dev, const Auth &a, const Target &t, Clock::time_point deadline) {
        std::string expected = a.bridgeid;
        if (expected.empty()) expected = info_of(dev, "bridgeid");
        // No time left to ask: say only what is certain (nothing was sent), and remember nothing.
        if (expected.empty() || ms_left(deadline) < 150) return kErrCertChanged;
        const Reply r = call(t, "GET", "/api/0/config", nullptr, "", "", deadline, true);
        Config cfg;
        const bool same = r.ok() && r.status == 200 && cert_names_bridge(r.pem, expected) && read_config(r.body, cfg) &&
                          normalize_bridge_id(cfg.bridgeid) == normalize_bridge_id(expected);
        if (!same) return kErrMoved;
        note_cert_changed(dev, a);
        return kErrCertChanged;
    }

    // Turns a failed request into an error sentence, remembering a rejected key or a changed
    // certificate. Returns false when `r` is not a failure (2xx).
    bool failed(const home::Device &dev, const Auth &a, const Target &t, const Reply &r, Clock::time_point deadline,
                std::string &err) {
        if (!r.ok()) {
            err = r.pin_mismatch() ? pin_failure(dev, a, t, deadline) : transfer_error(r);
            return true;
        }
        if (r.status >= 200 && r.status < 300) return false;
        if (r.status == 401 || r.status == 403) {
            note_refused(dev, a);
            err = kErrRefused;
        } else if (r.status == 429 || r.status >= 500) {
            err = kErrBusy;
        } else {
            err = kErrGarbled;
        }
        return true;
    }

    // One request with the key, tried a second time after a short pause when the bridge says it
    // is busy.
    Reply request(const Target &t, const Auth &a, const char *method, const std::string &path, const std::string *body,
                  Clock::time_point deadline) {
        for (int attempt = 0;; ++attempt) {
            Reply r = call(t, method, path, body, a.pin, a.key, deadline);
            const bool busy = r.ok() && (r.status == 429 || r.status == 503);
            if (!busy || attempt > 0 || ms_left(deadline) < 900) return r;
            std::this_thread::sleep_for(milliseconds(300));
        }
    }

    // ---- pairing
    bool pair_inner(home::Device &dev, std::string &err) {
        Target t;
        if (!make_target(dev, t)) { err = kPairBadAddress; return false; }
        // 1. Who is there, and which key it holds. Nothing secret is sent in this request.
        const Reply r = call(t, "GET", "/api/0/config", nullptr, "", "", Clock::now() + milliseconds(6000), true);
        if (!r.ok()) {
            const char *why = transfer_error(r);
            err = why == kErrTimeout || why == kErrNoAnswer ? kPairNoAnswer : why == kErrGarbled ? kPairGarbled : kPairFailed;
            return false;
        }
        Config cfg;
        if (r.status != 200 || !read_config(r.body, cfg)) { err = kPairNotHue; return false; }
        std::string model = cfg.modelid;
        for (char &c : model) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        if (model == "BSB001") { err = kPairV1; return false; }
        if (cfg.swversion < hue::kMinFirmware) { err = kPairOld; return false; }
        const std::string id = normalize_bridge_id(cfg.bridgeid);
        const Auth old = read_auth(dev.auth);
        for (const std::string &expected : {info_of(dev, "bridgeid"), old.bridgeid})
            if (!expected.empty() && normalize_bridge_id(expected) != id) { err = kPairWrongBridge; return false; }
        if (r.pem.empty()) { err = kPairNoCert; return false; }
        if (!cert_names_bridge(r.pem, cfg.bridgeid)) { err = kPairWrongCert; return false; }
        const std::string pin = pin_from_pem(r.pem);
        if (!safe_pin(pin)) { err = kPairNoCert; return false; }

        // 2. Ask for a key, pinned to that public key, until the button is pressed.
        const std::string body = json{{"devicetype", kDeviceType}}.dump();
        const auto end = Clock::now() + milliseconds(g_pair_window_ms.load());
        int failures = 0;
        for (;;) {
            const auto start = Clock::now();
            const Reply p = call(t, "POST", "/api", &body, pin, "", start + milliseconds(4000));
            bool last_failed = false;
            if (p.pin_mismatch()) { err = kPairCertMoved; return false; }
            if (!p.ok()) {
                last_failed = true;
                if (++failures >= 3) { err = kPairStopped; return false; }
            } else if (p.status == 429 || p.status == 503) {
                // busy: ask again at the next tick
            } else if (p.status != 200) {
                err = kPairGarbled;
                return false;
            } else {
                failures = 0;
                std::string key;
                switch (read_link_reply(p.body, key)) {
                    case LinkReply::Key: {
                        json a = json::object();
                        a["key"] = key;
                        a["pin"] = pin;
                        a["bridgeid"] = lower(cfg.bridgeid);
                        forget(dev);
                        dev.auth = a;
                        return true;
                    }
                    case LinkReply::NotPressed: break;
                    case LinkReply::Refused: err = kPairRefused; return false;
                    case LinkReply::Unreadable: err = kPairGarbled; return false;
                }
            }
            const auto next = start + milliseconds(g_pair_interval_ms.load());
            if (next >= end) { err = last_failed ? kPairStopped : kPairTimeout; return false; }
            std::this_thread::sleep_until(next);
        }
    }

    // ---- reading
    bool enumerate_inner(home::Device &dev, std::vector<home::Thing> &out, std::string &err) {
        out.clear();
        Auth a;
        Target t;
        if (!ready(dev, a, t, err)) return false;
        const auto deadline = Clock::now() + milliseconds(kReadDeadlineMs);
        const Reply r = request(t, a, "GET", "/clip/v2/resource", nullptr, deadline);
        if (failed(dev, a, t, r, deadline, err)) return false;
        json data;
        if (!v2_data(r.body, data)) { err = kErrGarbled; return false; }
        Snapshot snap;
        split_resources(data, snap);
        BridgeInfo info;
        build_things(snap, out, info);
        if (!info.name.empty()) dev.name = info.name;
        if (!info.model.empty()) dev.model = info.model;
        return true;
    }

    bool read_inner(const home::Device &dev, std::vector<home::Thing> &things, std::string &err) {
        if (things.empty()) things = dev.things;
        Auth a;
        Target t;
        if (!ready(dev, a, t, err)) return false;
        bool lights = false, temperature = false, motion = false;
        for (const auto &th : things) {
            const std::string kind = str(th.ref, "kind");
            lights = lights || kind == "light";
            temperature = temperature || (kind == "sensor" && safe_id(str(th.ref, "temperature")));
            motion = motion || (kind == "sensor" && safe_id(str(th.ref, "motion")));
        }
        if (!lights && !temperature && !motion) return true;
        const auto deadline = Clock::now() + milliseconds(kReadDeadlineMs);
        std::map<std::string, json> by_id;
        auto fetch = [&](const char *type) {
            const Reply r = request(t, a, "GET", std::string("/clip/v2/resource/") + type, nullptr, deadline);
            if (failed(dev, a, t, r, deadline, err)) return false;
            json data;
            if (!v2_data(r.body, data)) { err = kErrGarbled; return false; }
            size_t n = 0;
            for (auto &item : data) {
                if (++n > 2000) break;
                const std::string id = str(item, "id");
                if (safe_id(id)) by_id[std::string(type) + "/" + id] = std::move(item);
            }
            return true;
        };
        if ((lights && !fetch("light")) || (temperature && !fetch("temperature")) || (motion && !fetch("motion"))) return false;
        for (auto &th : things) {
            const std::string kind = str(th.ref, "kind");
            if (kind == "light") {
                const auto it = by_id.find("light/" + str(th.ref, "rid"));
                th.state = it == by_id.end() ? json::object() : light_state(it->second);
            } else if (kind == "sensor") {
                const auto ti = by_id.find("temperature/" + str(th.ref, "temperature"));
                const auto mi = by_id.find("motion/" + str(th.ref, "motion"));
                th.state = sensor_state(ti == by_id.end() ? nullptr : &ti->second, mi == by_id.end() ? nullptr : &mi->second);
            }
        }
        return true;
    }

    // ---- changing
    // Sends one PUT and reads the answer.
    bool send(const home::Device &dev, const Auth &a, const Target &t, const std::string &path, const json &body, bool group,
              bool scene, Clock::time_point deadline, std::string &err) {
        space(dev, deadline);
        const std::string text = body.dump(-1, ' ', false, json::error_handler_t::replace);
        const Reply r = request(t, a, "PUT", path, &text, deadline);
        if (r.ok()) sent(dev, group || scene);
        if (r.ok() && r.status == 404) {
            err = scene ? kErrGoneScene : kErrGoneLight;
            return false;
        }
        if (r.ok() && r.status == 400) {
            const std::string e = first_error(r.body);
            err = e.empty() ? kErrRejected : describe_bridge_error(e, group);
            return false;
        }
        if (failed(dev, a, t, r, deadline, err)) return false;
        json data;
        if (!v2_data(r.body, data)) {   // a success must still be a well-formed CLIP v2 reply
            err = kErrGarbled;
            return false;
        }
        const std::string e = first_error(r.body);
        if (!e.empty()) {   // accepted, but the bridge says it may not have taken effect (207)
            err = describe_bridge_error(e, group);
            return false;
        }
        return true;
    }

    bool apply_inner(const home::Device &dev, const home::Thing &thing, const home::Change &change, int deadline_ms,
                     std::string &err) {
        const std::string kind = str(thing.ref, "kind");
        json body;
        std::string path;
        // What the thing cannot do is refused before anything else, whatever the pairing's state.
        if (kind == "light") {
            if (!light_body(thing, change, body, err)) return false;
            path = "/clip/v2/resource/light/" + str(thing.ref, "rid");
        } else if (kind == "scene") {
            if (!scene_body(thing, change, body, err)) return false;
            path = "/clip/v2/resource/scene/" + str(thing.ref, "rid");
        } else if (kind == "sensor") {
            err = kErrSensor;
            return false;
        } else {
            err = kErrUnknown;
            return false;
        }
        Auth a;
        Target t;
        if (!ready(dev, a, t, err)) return false;
        const auto deadline = Clock::now() + milliseconds(deadline_ms > 0 ? deadline_ms : kDefaultApplyMs);
        if (kind == "scene" && !scene_still_allowed(dev, a, t, path, str(thing.ref, "rid"), deadline, err)) return false;
        return send(dev, a, t, path, body, false, kind == "scene", deadline, err);
    }

    // A scene is read again right before it is recalled: it may have been changed in the Hue app
    // since the list was read (a smart plug added to it, a light of the room the user has since
    // marked "Ask before switching on"), and recalling it switches everything it now holds.
    bool scene_still_allowed(const home::Device &dev, const Auth &a, const Target &t, const std::string &path,
                             const std::string &rid, Clock::time_point deadline, std::string &err) {
        const Reply r = request(t, a, "GET", path, nullptr, deadline);
        if (r.ok() && r.status == 404) {
            err = kErrGoneScene;
            return false;
        }
        if (failed(dev, a, t, r, deadline, err)) return false;
        json data;
        if (!v2_data(r.body, data) || data.size() != 1 || str(data[0], "id") != rid) {
            err = kErrGarbled;
            return false;
        }
        return scene_allowed(data[0], dev.things, err);
    }

    bool apply_many_inner(const home::Device &dev, const std::vector<const home::Thing *> &things, const home::Change &change,
                          int deadline_ms, std::string &err) {
        err.clear();
        Group g;
        json body;
        std::string why;
        if (!pick_group(things, change, g) || !group_body(things, change, body, why)) return false;   // one by one instead
        Auth a;
        Target t;
        if (!ready(dev, a, t, err)) return false;
        const auto deadline = Clock::now() + milliseconds(deadline_ms > 0 ? deadline_ms : kDefaultApplyMs);
        // The group must still hold exactly these lights: a light added to the room since the
        // list was read would otherwise be switched too.
        const std::string path = g.rtype == "bridge_home" ? "/clip/v2/resource/light" : "/clip/v2/resource/" + g.rtype + "/" + g.rid;
        const Reply r = request(t, a, "GET", path, nullptr, deadline);
        if (r.ok() && r.status == 404) return false;
        if (failed(dev, a, t, r, deadline, err)) return false;
        json data;
        if (!v2_data(r.body, data)) {
            err = kErrGarbled;
            return false;
        }
        std::string now_fp;
        if (g.rtype == "bridge_home") {
            std::vector<std::string> ids;
            std::set<std::string> seen;
            for (const auto &l : data)
                if (safe_id(str(l, "id")) && seen.insert(str(l, "id")).second && ids.size() < 2000) ids.push_back(str(l, "id"));
            now_fp = fingerprint(ids);
        } else if (data.size() == 1) {
            now_fp = group_fingerprint(data[0]);
        }
        if (now_fp != g.fp) return false;   // membership changed: one by one instead
        return send(dev, a, t, "/clip/v2/resource/grouped_light/" + g.gl, body, true, false, deadline, err);
    }
};

}  // namespace

namespace hue {
namespace detail {
std::vector<std::string> error_texts() {
    return {kErrNotPaired, kErrRefused,   kErrCertChanged, kErrAddress,    kErrNoAnswer, kErrTimeout,   kErrMoved,
            kErrGarbled,   kErrBusy,      kErrRejected,    kErrGoneLight,  kErrGoneScene, kErrSensor,   kErrUnknown,
            kErrInternal,  kSceneChanged, kSceneDisabled,  kScenePlug,     kSceneAskFirst};
}
}  // namespace detail
}  // namespace hue

std::unique_ptr<home::Driver> make_hue_driver() { return std::make_unique<HueDriver>(); }
