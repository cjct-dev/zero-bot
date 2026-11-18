// Shelly on the smart-home driver contract: Gen1 over its REST paths, Gen2 and later over
// JSON-RPC. See shelly.h for what a caller can rely on.
//
// Sources: Shelly's public API documentation, shelly-api-docs.shelly.cloud (Gen1: /shelly,
// /settings, /status, /relay, /light, /color, /white, /roller and HTTP basic login; Gen2+:
// Shelly.GetDeviceInfo, GetStatus, GetConfig, Switch, Light, RGB, RGBW, CCT, Cover, the RPC frame
// and digest authentication). Read for behaviour only, no code copied: aioshelly (Apache-2.0) for
// "a relay whose consumption type starts with light is a light" and for leaving out a relay that
// drives the device's own thermostat. Gen1 model codes and which Gen1 models measure power are
// from memory of the vendor's product list (STATUS.md says which facts rest on memory).
#include "smarthome/drivers/shelly.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <set>

#include <openssl/evp.h>

#include "core/util.h"
#include "net/http.h"
#include "smarthome/color.h"
#include "smarthome/home.h"

namespace shelly {

const char *const kNoAnswer = "The Shelly did not answer.";
const char *const kNeedsPassword = "The Shelly asks for a password.";
const char *const kRefused = "The Shelly did not accept the saved password,";
const char *const kThrottled = "The Shelly is turning logins away for a while";
const char *const kOtherDevice = "Another device now answers at this address.";
const char *const kNotUnderstood = "The Shelly's answer could not be understood.";

namespace {

constexpr size_t kMaxBody = 256 * 1024;
constexpr int kListMs = 4000;             // enumerate, read and pair
constexpr size_t kMaxThings = 32;
constexpr size_t kMaxChannels = 16;       // of one kind (Gen1 arrays)
constexpr int kMaxComponentId = 299;      // Gen2+ ids: 0.., add-on probes 100.., BLU 200..
const char *const kFamily = "shelly";

const char kTypeIt[] = " Type it in Settings, Smart home, on the device's page.";
const char kReadAgain[] = "Read the device again in Settings, Smart home.";

// ---------------------------------------------------------------- JSON from a device
const json *member(const json &j, const char *key) {
    if (!j.is_object()) return nullptr;
    const auto it = j.find(key);
    return it == j.end() ? nullptr : &*it;
}
const json *member_obj(const json &j, const char *key) {
    const json *m = member(j, key);
    return m && m->is_object() ? m : nullptr;
}
const json *member_arr(const json &j, const char *key) {
    const json *m = member(j, key);
    return m && m->is_array() ? m : nullptr;
}
std::string text(const json *v) { return v && v->is_string() ? v->get<std::string>() : std::string(); }
std::optional<bool> boolean(const json *v) {
    if (!v || !v->is_boolean()) return std::nullopt;
    return v->get<bool>();
}
// A number within its documented range; anything else (a string, NaN, out of range) is no value.
std::optional<double> number(const json *v, double lo, double hi) {
    if (!v || !v->is_number()) return std::nullopt;
    const double d = v->get<double>();
    if (!std::isfinite(d) || d < lo || d > hi) return std::nullopt;
    return d;
}
int whole(const json *v, int lo, int hi, int fallback) {
    if (!v || !v->is_number_integer()) return fallback;
    const long long n = v->get<long long>();
    return n < lo || n > hi ? fallback : static_cast<int>(n);
}
int rounded(double d) { return static_cast<int>(std::lround(d)); }

// Parses text from a device: refused when too long, nested too deep or not JSON.
bool parse(const std::string &body, json &out) {
    if (body.size() > kMaxBody || detail::too_deep(body)) return false;
    out = json::parse(body, nullptr, false);
    return !out.is_discarded();
}

bool token_chars(const std::string &s, const char *extra, size_t max) {
    if (s.empty() || s.size() > max) return false;
    for (const unsigned char c : s)
        if (!isalnum(c) && !strchr(extra, c)) return false;
    return true;
}

std::string lower_hex_mac(const std::string &raw) {
    std::string out;
    for (const unsigned char c : raw) {
        if (c == ':' || c == '-') continue;
        if (!isxdigit(c)) return "";
        out += static_cast<char>(tolower(c));
    }
    return out.size() == 12 ? out : "";
}

std::string upper(std::string s) {
    for (char &c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}

bool starts_with(const std::string &s, const char *prefix) { return s.compare(0, strlen(prefix), prefix) == 0; }

bool all_digits(const std::string &s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
}

// ---------------------------------------------------------------- Gen1 models (from memory of the vendor's list)
struct Gen1Model { const char *type, *name; };
const Gen1Model kGen1Models[] = {
    {"SHSW-1", "1"},          {"SHSW-PM", "1PM"},        {"SHSW-L", "1L"},       {"SHSW-21", "2"},
    {"SHSW-25", "2.5"},       {"SHSW-44", "4Pro"},       {"SHPLG-1", "Plug"},    {"SHPLG2-1", "Plug"},
    {"SHPLG-S", "Plug S"},    {"SHPLG-U1", "Plug US"},   {"SHDM-1", "Dimmer"},   {"SHDM-2", "Dimmer 2"},
    {"SHDIMW-1", "Dimmer W1"}, {"SHRGBW2", "RGBW2"},     {"SHBLB-1", "Bulb"},    {"SHCB-1", "Bulb RGBW"},
    {"SHBDUO-1", "Duo"},      {"SHVIN-1", "Vintage"},    {"SHHT-1", "H&T"},      {"SHEM", "EM"},
    {"SHEM-3", "3EM"},        {"SHUNI-1", "UNI"},        {"SHIX3-1", "i3"},
};
std::string gen1_model_name(const std::string &type) {
    for (const auto &m : kGen1Models)
        if (type == m.type) return m.name;
    return type;
}
// Models with nothing to offer here: thermostats, alarms (gas, smoke), flood, door and motion
// sensors, buttons, input-only devices and the infrared blaster.
bool gen1_left_out(const std::string &type) {
    for (const char *p : {"SHGS", "SHSM", "SHWT", "SHDW", "SHMOS", "SHBTN", "SHTRV", "SHIX3", "SHSEN"})
        if (starts_with(type, p)) return true;
    return false;
}
// Models whose relays do not measure power (their /status may still hold a "meters" entry of 0).
bool gen1_meters(const detail::Identity &who) {
    if (who.num_meters == 0) return false;
    for (const char *t : {"SHSW-1", "SHUNI-1", "SHIX3-1", "SHEM", "SHEM-3"})
        if (who.model == t) return false;
    return true;
}

std::string model_name(const detail::Identity &who) {
    if (who.gen >= 2) return who.app.empty() ? who.model : who.app;
    return gen1_model_name(who.model);
}

// ---------------------------------------------------------------- things
home::Thing make_thing(const detail::Identity &who, const std::string &comp, int id, const std::string &thing_id,
                       const std::string &name, const std::string &type, uint32_t caps) {
    home::Thing t;
    t.id = thing_id;
    t.name = home::clean_name(name);
    t.type = type;
    t.caps = caps;
    t.ref = {{"gen", who.gen}, {"comp", comp}, {"id", id}, {"dev", who.who()}, {"mac", who.mac}};
    return t;
}

// Names left empty in the Shelly app: the device's name (or model), numbered when it has several
// things of the same sort.
void fill_names(std::vector<home::Thing> &things, const std::string &device_name, const detail::Identity &who) {
    const std::string base = !device_name.empty() ? device_name : "Shelly " + model_name(who);
    size_t controls = 0, sensors = 0;
    for (const auto &t : things) (t.type == "sensor" ? sensors : controls) += 1;
    size_t c = 0, s = 0;
    for (auto &t : things) {
        const bool sensor = t.type == "sensor";
        const size_t nth = sensor ? ++s : ++c;
        if (!t.name.empty()) continue;
        std::string name = base;
        if (sensor && controls > 0) name += " sensor";
        if ((sensor ? sensors : controls) > 1) name += " " + std::to_string(nth);
        t.name = home::clean_name(name);
    }
}

bool light_type(const std::string &appliance) { return starts_with(util::lower(util::trim(appliance)), "light"); }

void put_hs(json &state, const json *rgb) {
    if (!rgb || !rgb->is_array() || rgb->size() != 3) return;
    int c[3];
    for (int k = 0; k < 3; ++k) {
        const auto v = number(&(*rgb)[k], 0, 255);
        if (!v) return;
        c[k] = rounded(*v);
    }
    double h = 0, s = 0, val = 0;
    color::rgb_to_hs(c[0], c[1], c[2], h, s, val);
    state["hue"] = rounded(h) % 360;
    state["saturation"] = rounded(s);
}

std::string ref_text(const home::Thing &t, const char *key) { return text(member(t.ref, key)); }
int ref_int(const home::Thing &t, const char *key, int lo, int hi, int fallback) { return whole(member(t.ref, key), lo, hi, fallback); }
bool ref_flag(const home::Thing &t, const char *key) { return boolean(member(t.ref, key)).value_or(false); }

}  // namespace

// ================================================================ detail
namespace detail {

bool too_deep(const std::string &text, int max) {
    int depth = 0;
    bool in_string = false, escaped = false;
    for (const char c : text) {
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') in_string = true;
        else if (c == '[' || c == '{') { if (++depth > max) return true; }
        else if (c == ']' || c == '}') --depth;
    }
    return false;
}

bool valid_ipv4(const std::string &ip) {
    if (ip.empty() || ip.size() > 15) return false;
    for (const char c : ip)
        if (!isdigit(static_cast<unsigned char>(c)) && c != '.') return false;
    in_addr a;
    return inet_pton(AF_INET, ip.c_str(), &a) == 1;
}

std::string basic_auth_header(const std::string &user, const std::string &password) {
    return "Authorization: Basic " + util::base64(user + ":" + password);
}

std::string fingerprint(const std::string &user, const std::string &password) {
    const std::string in = "zero-bot shelly login\n" + user + "\n" + password;
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_Digest(in.data(), in.size(), md, &len, EVP_sha256(), nullptr) != 1) return "";
    static const char hex[] = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < len && out.size() < 32; ++i) {
        out += hex[md[i] >> 4];
        out += hex[md[i] & 15];
    }
    return out;
}

// Words of a name, lower case. Bytes of UTF-8 letters ("ü", "ż") belong to their word, so "Tür" is
// one word, not "t" and "r".
static std::vector<std::string> name_words(const std::string &s) {
    std::vector<std::string> out;
    std::string w;
    for (const char c : util::lower(s) + " ") {
        if (isalnum(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 0x80) w += c;
        else if (!w.empty()) { out.push_back(w); w.clear(); }
    }
    return out;
}
static bool says_banned(const std::string &s) {
    // English, and the words for door, gate, lock and garage in the languages the app's yes words
    // cover (German, French, Spanish, Italian, Portuguese, Dutch) and Polish.
    static const std::set<std::string> kBanned = {
        "gate", "gates", "door", "doors", "lock", "locks", "latch", "valve", "valves", "alarm", "siren", "doorlock", "opener",
        "openers", "tor", "tore", "hoftor", "gartentor", "rolltor", "schiebetor", "einfahrtstor", "porte", "portes", "portail",
        "portails", "serrure", "verrou", "puerta", "puertas", "portón", "porton", "cerradura", "garaje", "porta", "portone",
        "cancello", "serratura", "portão", "portao", "fechadura", "brama", "drzwi", "zamek", "garaż"};
    // Endings of German and Dutch compounds ("Haustür", "Voordeur", "Türschloss"). Not "tor": motor,
    // radiator and monitor end in it.
    static const char *const kBannedEnds[] = {"tür", "türe", "tuer", "deur", "deuren", "poort", "schloss", "riegel"};
    for (const auto &w : name_words(s)) {
        if (kBanned.count(w) || w.find("garage") != std::string::npos) return true;   // "GarageDoor", "Garagentor"
        for (const char *end : kBannedEnds)
            if (w.size() >= strlen(end) && w.compare(w.size() - strlen(end), std::string::npos, end) == 0) return true;
    }
    return false;
}
static bool says_light(const std::string &s) {
    static const std::set<std::string> kLight = {"light", "lights", "lamp", "lamps", "bulb", "bulbs", "led", "leds"};
    for (const auto &w : name_words(s))
        if (kLight.count(w)) return true;
    return false;
}

bool banned(const std::string &name, const std::string &appliance_type) {
    return (says_banned(name) || says_banned(appliance_type)) && !says_light(name);
}

bool left_out(const std::string &name, const std::string &device_name, const std::string &appliance_type, double auto_off_s) {
    if (banned(name, appliance_type)) return true;
    // A channel the owner left unnamed goes by the device's name ("Garage door"), and a Gen1 blind
    // has no name of its own: the device's name counts unless the channel's own says it is a light.
    if (banned(device_name, "") && !says_light(name)) return true;
    // Switched off again within seconds of being switched on: a pulse to an opener or a bell.
    return auto_off_s > 0 && auto_off_s <= kPulseSeconds;
}

// left_out() for one Gen2+ "switch:N" or "cover:N", from the result of Shelly.GetConfig. Names are
// taken as the device has them, before clean_name() (which may cut a long name short).
static bool gen2_channel_left_out(const json &config, const std::string &comp, int id) {
    std::string device_name, appliance;
    const json *consumption = nullptr;
    if (const json *sys = member_obj(config, "sys")) {
        if (const json *dev = member_obj(*sys, "device")) device_name = text(member(*dev, "name"));
        if (const json *ui = member_obj(*sys, "ui_data")) consumption = member_arr(*ui, "consumption_types");
    }
    const std::string key = comp + ":" + std::to_string(id);
    const json *cfg = member_obj(config, key.c_str());
    const std::string name = cfg ? text(member(*cfg, "name")) : "";
    if (comp == "cover") return left_out(name, device_name, "");   // a "garage door" wired as a cover is still a garage door
    if (consumption && id < static_cast<int>(consumption->size())) appliance = text(&(*consumption)[id]);
    // Auto-off (Switch config auto_off, auto_off_delay in seconds).
    double auto_off = 0;
    if (cfg && boolean(member(*cfg, "auto_off")).value_or(false)) auto_off = number(member(*cfg, "auto_off_delay"), 0, 1e9).value_or(0);
    return left_out(name, device_name, appliance, auto_off);
}

// left_out() for one Gen1 relay or roller, from the body of /settings.
static bool gen1_channel_left_out(const json &settings, const std::string &comp, size_t i) {
    const std::string device_name = text(member(settings, "name"));
    const json *list = member_arr(settings, comp == "roller" ? "rollers" : "relays");
    const json *ch = list && i < list->size() && (*list)[i].is_object() ? &(*list)[i] : nullptr;
    const std::string name = ch ? text(member(*ch, "name")) : "";
    // Gen1 blinds have no name of their own (the documentation lists none): the device's counts.
    if (comp == "roller") return left_out(name, device_name, "");
    // auto_off: "automatic flip back timer, seconds", 0 when off.
    return left_out(name, device_name, ch ? text(member(*ch, "appliance_type")) : "",
                    ch ? number(member(*ch, "auto_off"), 0, 1e9).value_or(0) : 0);
}

bool recheck(const home::Thing &thing, const json &config, std::string &err) {
    const int gen = ref_int(thing, "gen", 1, 9, 0);
    const std::string comp = ref_text(thing, "comp");
    const int id = ref_int(thing, "id", 0, kMaxComponentId, -1);
    if (gen == 0 || id < 0 || !config.is_object()) {
        err = kNotUnderstood;
        return false;
    }
    bool there = true, out = false;
    if (gen >= 2) {
        if (comp != "switch" && comp != "cover") return true;
        there = member_obj(config, (comp + ":" + std::to_string(id)).c_str()) != nullptr;   // a 2PM changed profile: gone
        out = there && gen2_channel_left_out(config, comp, id);
    } else {
        if (comp != "relay" && comp != "roller") return true;
        // A 2.5 changed between relay and roller mode: the channel is gone.
        const bool roller_mode = util::lower(text(member(config, "mode"))) == "roller";
        const json *list = member_arr(config, comp == "roller" ? "rollers" : "relays");
        there = (comp == "roller") == roller_mode && list && static_cast<size_t>(id) < list->size() && (*list)[id].is_object();
        out = there && gen1_channel_left_out(config, comp, static_cast<size_t>(id));
    }
    if (!there) {
        err = std::string("The Shelly has changed since it was read: it no longer has this channel. Nothing was sent. ") + kReadAgain;
        return false;
    }
    if (out) {
        err = std::string("The Shelly's settings now make this a door, gate, lock or opener (by its name, or a timer that switches "
                          "it off within seconds), which Zero Bot does not control. Nothing was sent. ") + kReadAgain;
        return false;
    }
    return true;
}

bool parse_identity(const std::string &body, Identity &out) {
    out = Identity();
    json j;
    if (!parse(body, j) || !j.is_object()) return false;
    const json *gen = member(j, "gen");
    if (gen) {
        const int g = whole(gen, 2, 9, 0);
        if (g == 0) return false;
        const std::string id = util::lower(text(member(j, "id")));
        if (!token_chars(id, "-_", 64)) return false;
        out.gen = g;
        out.id = id;
        out.model = home::clean_name(text(member(j, "model")));
        out.app = home::clean_name(text(member(j, "app")));
        const std::string profile = util::lower(text(member(j, "profile")));
        if (token_chars(profile, "_", 20)) out.profile = profile;
        out.auth = boolean(member(j, "auth_en")).value_or(false);
    } else {
        // Gen1 has no "gen": its "type" names the model.
        const std::string type = upper(text(member(j, "type")));
        if (!token_chars(type, "-", 24)) return false;
        out.gen = 1;
        out.model = type;
        out.auth = boolean(member(j, "auth")).value_or(false);
        out.num_meters = whole(member(j, "num_meters"), 0, 16, -1);
    }
    out.mac = lower_hex_mac(text(member(j, "mac")));
    if (out.gen == 1 && out.mac.empty()) return false;   // Gen1 is recognised by its MAC
    return true;
}

std::vector<home::Thing> things_gen2(const Identity &who, const json &config, const json &status, std::string &device_name) {
    std::vector<home::Thing> out;
    device_name.clear();
    if (const json *sys = member_obj(config, "sys"))
        if (const json *dev = member_obj(*sys, "device")) device_name = home::clean_name(text(member(*dev, "name")));
    bool thermostat_relay = false;
    if (const json *sys = member_obj(status, "sys")) thermostat_relay = boolean(member(*sys, "relay_in_thermostat")).value_or(false);
    const json *consumption = nullptr;
    if (const json *sys = member_obj(config, "sys"))
        if (const json *ui = member_obj(*sys, "ui_data")) consumption = member_arr(*ui, "consumption_types");
    const bool plug = util::lower(who.app).find("plug") != std::string::npos;

    // The components the device has, by their status entries: "switch:0", "cover:0", ...
    std::vector<std::pair<std::string, int>> comps;
    if (status.is_object())
        for (auto it = status.begin(); it != status.end() && comps.size() < 200; ++it) {
            const std::string &key = it.key();
            const size_t colon = key.find(':');
            if (colon == std::string::npos || colon == 0 || colon > 16 || !it->is_object()) continue;
            const std::string comp = key.substr(0, colon), digits = key.substr(colon + 1);
            if (digits.size() > 3 || !all_digits(digits)) continue;
            const int id = std::stoi(digits);
            if (id > kMaxComponentId || std::to_string(id) != digits) continue;
            comps.push_back({comp, id});
        }
    // Kinds in a fixed order, each by number (keys sort as text: "switch:10" before "switch:2").
    static const char *const kOrder[] = {"switch", "light", "rgb", "rgbw", "cct", "cover", "temperature", "humidity", "pm1"};
    std::sort(comps.begin(), comps.end(), [](const auto &a, const auto &b) {
        auto rank = [](const std::string &c) {
            for (size_t i = 0; i < sizeof(kOrder) / sizeof(kOrder[0]); ++i)
                if (c == kOrder[i]) return static_cast<int>(i);
            return 99;
        };
        return rank(a.first) != rank(b.first) ? rank(a.first) < rank(b.first) : a.second < b.second;
    });
    std::set<int> temperatures;
    for (const auto &c : comps)
        if (c.first == "temperature") temperatures.insert(c.second);

    for (const auto &c : comps) {
        if (out.size() >= kMaxThings) break;
        const std::string &comp = c.first;
        const int id = c.second;
        const std::string key = comp + ":" + std::to_string(id);
        const json *cfg = member_obj(config, key.c_str());
        const json *st = member_obj(status, key.c_str());
        const std::string name = cfg ? text(member(*cfg, "name")) : "";
        home::Thing t;
        if (comp == "switch") {
            if (thermostat_relay) continue;   // the device's own thermostat drives it
            if (gen2_channel_left_out(config, comp, id)) continue;   // garage doors, locks, pulses (apply() asks again)
            std::string appliance;
            if (consumption && id < static_cast<int>(consumption->size())) appliance = text(&(*consumption)[id]);
            const std::string type = light_type(appliance) ? "light" : plug ? "plug" : "switch";
            uint32_t caps = home::kPower;
            if (st && number(member(*st, "apower"), -100000, 100000)) caps |= home::kSensor;
            t = make_thing(who, comp, id, key, name, type, caps);
        } else if (comp == "light") {
            t = make_thing(who, comp, id, key, name, "light", home::kPower | home::kBrightness);
        } else if (comp == "rgb" || comp == "rgbw") {
            t = make_thing(who, comp, id, key, name, "light", home::kPower | home::kBrightness | home::kColor);
        } else if (comp == "cct") {
            t = make_thing(who, comp, id, key, name, "light", home::kPower | home::kBrightness | home::kColorTemp);
            int lo = 2700, hi = 6500;
            if (const json *range = cfg ? member_arr(*cfg, "ct_range") : nullptr; range && range->size() == 2) {
                const auto a = number(&(*range)[0], 1000, 10000), b = number(&(*range)[1], 1000, 10000);
                if (a && b && *a < *b) { lo = rounded(*a); hi = rounded(*b); }
            }
            t.ref["kmin"] = lo;
            t.ref["kmax"] = hi;
        } else if (comp == "cover") {
            if (gen2_channel_left_out(config, comp, id)) continue;   // a "garage door" wired as a cover is still a garage door
            t = make_thing(who, comp, id, key, name, "cover", home::kPosition);
            t.ref["calibrated"] = st && boolean(member(*st, "pos_control")).value_or(false);
        } else if (comp == "temperature") {
            t = make_thing(who, comp, id, key, name, "sensor", home::kSensor);
        } else if (comp == "humidity") {
            if (temperatures.count(id)) continue;   // read with the temperature of the same number
            t = make_thing(who, comp, id, key, name, "sensor", home::kSensor);
        } else if (comp == "pm1") {
            t = make_thing(who, comp, id, key, name, "sensor", home::kSensor);
        } else {
            continue;   // inputs, meters, thermostats, smoke, scripts, ...: not offered
        }
        t.state = state_of(t, status);
        out.push_back(std::move(t));
    }
    fill_names(out, device_name, who);
    return out;
}

std::vector<home::Thing> things_gen1(const Identity &who, const json &settings, const json &status, std::string &device_name) {
    std::vector<home::Thing> out;
    device_name = home::clean_name(text(member(settings, "name")));
    const std::string &type = who.model;
    if (gen1_left_out(type)) return out;
    if (type == "SHHT-1") {   // a battery sensor: readable only while it is awake
        home::Thing t = make_thing(who, "sensor", 0, "sensor", "", "sensor", home::kSensor);
        t.state = state_of(t, status);
        out.push_back(std::move(t));
        fill_names(out, device_name, who);
        return out;
    }
    const std::string mode = util::lower(text(member(settings, "mode")));
    const bool meters = gen1_meters(who);
    const json *meter_list = member_arr(status, "meters");
    auto metered = [&](size_t i) {
        if (!meters || !meter_list || i >= meter_list->size() || !(*meter_list)[i].is_object()) return false;
        const json &m = (*meter_list)[i];
        return boolean(member(m, "is_valid")).value_or(true) && number(member(m, "power"), -100000, 100000).has_value();
    };

    if (mode == "roller") {
        const json *rollers = member_arr(status, "rollers");
        const json *named = member_arr(settings, "rollers");
        for (size_t i = 0; rollers && i < rollers->size() && i < kMaxChannels; ++i) {
            if (!(*rollers)[i].is_object()) continue;
            const std::string name = named && i < named->size() && (*named)[i].is_object() ? text(member((*named)[i], "name")) : "";
            if (gen1_channel_left_out(settings, "roller", i)) continue;   // garage doors (apply() asks again)
            const int id = static_cast<int>(i);
            home::Thing t = make_thing(who, "roller", id, "roller:" + std::to_string(id), name, "cover", home::kPosition);
            t.ref["calibrated"] = boolean(member((*rollers)[i], "positioning")).value_or(false);
            t.state = state_of(t, status);
            out.push_back(std::move(t));
        }
    } else if (const json *relays = member_arr(settings, "relays")) {
        for (size_t i = 0; i < relays->size() && i < kMaxChannels; ++i) {
            if (!(*relays)[i].is_object()) continue;
            const json &r = (*relays)[i];
            const std::string name = text(member(r, "name")), appliance = text(member(r, "appliance_type"));
            if (gen1_channel_left_out(settings, "relay", i)) continue;   // garage doors, locks, pulses (apply() asks again)
            const std::string kind = light_type(appliance) ? "light" : starts_with(type, "SHPLG") ? "plug" : "switch";
            const int id = static_cast<int>(i);
            home::Thing t = make_thing(who, "relay", id, "relay:" + std::to_string(id), name, kind,
                                       home::kPower | (metered(i) ? home::kSensor : 0u));
            t.state = state_of(t, status);
            out.push_back(std::move(t));
        }
    }

    if (const json *lights = member_arr(settings, "lights")) {
        const bool rgbw2 = type == "SHRGBW2", bulb = type == "SHBLB-1" || type == "SHCB-1", duo = type == "SHBDUO-1";
        const bool dimmer = starts_with(type, "SHDM") || starts_with(type, "SHDIMW");
        size_t count = std::min(lights->size(), kMaxChannels);
        if (rgbw2 && mode == "color") count = std::min<size_t>(count, 1);   // one colour channel
        for (size_t i = 0; i < count && out.size() < kMaxThings; ++i) {
            if (!(*lights)[i].is_object()) continue;
            const int id = static_cast<int>(i);
            uint32_t caps = home::kPower | home::kBrightness;
            std::string path = "light";
            bool gain = false;
            int kmin = 0, kmax = 0;
            if (rgbw2) {
                path = mode == "color" ? "color" : "white";
                if (mode == "color") { caps |= home::kColor; gain = true; }
            } else if (bulb) {
                if (mode == "color") { caps |= home::kColor; gain = true; }
                else { caps |= home::kColorTemp; kmin = 3000; kmax = 6500; }
            } else if (duo) {
                caps |= home::kColorTemp;
                kmin = 2700;
                kmax = 6500;
            }
            if (metered(i)) caps |= home::kSensor;
            home::Thing t = make_thing(who, "light", id, "light:" + std::to_string(id), text(member((*lights)[i], "name")),
                                       "light", caps);
            t.ref["path"] = path;
            if (gain) t.ref["gain"] = true;
            if (kmin) { t.ref["kmin"] = kmin; t.ref["kmax"] = kmax; }
            if (dimmer) t.ref["min"] = 1;   // a Gen1 dimmer's brightness runs 1..100
            t.state = state_of(t, status);
            out.push_back(std::move(t));
        }
    }
    if (out.size() > kMaxThings) out.resize(kMaxThings);
    fill_names(out, device_name, who);
    return out;
}

json state_of(const home::Thing &thing, const json &status) {
    json s = json::object();
    const int gen = ref_int(thing, "gen", 1, 9, 0);
    const std::string comp = ref_text(thing, "comp");
    const int id = ref_int(thing, "id", 0, kMaxComponentId, -1);
    if (gen == 0 || id < 0 || !status.is_object()) return s;
    const bool metering = (thing.caps & home::kSensor) != 0;
    auto put_power = [&](const json *v) {
        if (const auto p = number(v, -100000, 100000)) s["power_w"] = std::round(*p * 10) / 10;
    };
    if (gen >= 2) {
        const std::string key = comp + ":" + std::to_string(id);
        const json *st = member_obj(status, key.c_str());
        if (!st) return s;
        if (comp == "switch" || comp == "light" || comp == "rgb" || comp == "rgbw" || comp == "cct") {
            if (const auto on = boolean(member(*st, "output"))) s["on"] = *on;
            if (comp != "switch")
                if (const auto b = number(member(*st, "brightness"), 0, 100)) s["brightness"] = rounded(*b);
            if (comp == "rgb" || comp == "rgbw") put_hs(s, member(*st, "rgb"));
            if (comp == "cct")
                if (const auto k = number(member(*st, "ct"), 1000, 10000)) s["kelvin"] = rounded(*k);
            if (metering) put_power(member(*st, "apower"));
        } else if (comp == "cover") {
            if (const auto p = number(member(*st, "current_pos"), 0, 100)) s["position"] = rounded(*p);
            else {
                const std::string state = text(member(*st, "state"));
                if (state == "open") s["position"] = 100;
                else if (state == "closed") s["position"] = 0;
            }
        } else if (comp == "temperature") {
            if (const auto c = number(member(*st, "tC"), -60, 150)) s["current_c"] = std::round(*c * 10) / 10;
            const std::string hkey = "humidity:" + std::to_string(id);
            if (const json *h = member_obj(status, hkey.c_str()))
                if (const auto rh = number(member(*h, "rh"), 0, 100)) s["humidity"] = rounded(*rh);
        } else if (comp == "humidity") {
            if (const auto rh = number(member(*st, "rh"), 0, 100)) s["humidity"] = rounded(*rh);
        } else if (comp == "pm1") {
            put_power(member(*st, "apower"));
        }
        return s;
    }
    auto entry = [&](const char *list) -> const json * {
        const json *a = member_arr(status, list);
        return a && static_cast<size_t>(id) < a->size() && (*a)[id].is_object() ? &(*a)[id] : nullptr;
    };
    auto meter = [&]() {
        if (!metering) return;
        if (const json *m = entry("meters"))
            if (boolean(member(*m, "is_valid")).value_or(true)) put_power(member(*m, "power"));
    };
    if (comp == "relay") {
        const json *r = entry("relays");
        if (!r) return s;
        if (const auto on = boolean(member(*r, "ison"))) s["on"] = *on;
        meter();
    } else if (comp == "roller") {
        const json *r = entry("rollers");
        if (!r) return s;
        // Without calibration the device does not know where the blind is.
        if (boolean(member(*r, "positioning")).value_or(false))
            if (const auto p = number(member(*r, "current_pos"), 0, 100)) s["position"] = rounded(*p);
    } else if (comp == "light") {
        const json *l = entry("lights");
        if (!l) return s;
        if (const auto on = boolean(member(*l, "ison"))) s["on"] = *on;
        if (ref_flag(thing, "gain")) {
            if (const auto g = number(member(*l, "gain"), 0, 100)) s["brightness"] = rounded(*g);
            const json rgb = json::array({l->value("red", json()), l->value("green", json()), l->value("blue", json())});
            put_hs(s, &rgb);
        } else if (const auto b = number(member(*l, "brightness"), 0, 100)) {
            s["brightness"] = rounded(*b);
        }
        if (thing.caps & home::kColorTemp)
            if (const auto k = number(member(*l, "temp"), 1000, 10000)) s["kelvin"] = rounded(*k);
        meter();
    } else if (comp == "sensor") {
        if (const json *t = member_obj(status, "tmp"))
            if (boolean(member(*t, "is_valid")).value_or(true))
                if (const auto c = number(member(*t, "tC"), -60, 150)) s["current_c"] = std::round(*c * 10) / 10;
        if (const json *h = member_obj(status, "hum"))
            if (boolean(member(*h, "is_valid")).value_or(true))
                if (const auto rh = number(member(*h, "value"), 0, 100)) s["humidity"] = rounded(*rh);
    }
    return s;
}

bool plan(const home::Thing &thing, const home::Change &c, Call &out, std::string &err) {
    out = Call();
    const int gen = ref_int(thing, "gen", 1, 9, 0);
    const std::string comp = ref_text(thing, "comp");
    const int id = ref_int(thing, "id", 0, kMaxComponentId, -1);
    if (gen == 0 || comp.empty() || id < 0) {
        err = std::string("Zero Bot does not know this Shelly channel properly. ") + kReadAgain;
        return false;
    }
    const std::string n = std::to_string(id);
    const bool other = c.target_c || c.mode || c.fan || c.volume || c.volume_step || c.mute || c.source || c.key;
    const bool stop = c.transport && *c.transport == "stop";

    if (comp == "temperature" || comp == "humidity" || comp == "pm1" || comp == "sensor") {
        err = "This is a sensor; it can only be read.";
        return false;
    }
    if ((gen >= 2 && comp == "switch") || (gen == 1 && comp == "relay")) {
        if (!c.on || c.brightness || c.kelvin || c.hs || c.position || c.transport || other) {
            err = "This switch can only be turned on and off.";
            return false;
        }
        if (gen >= 2) {
            out.method = "Switch.Set";
            out.params = {{"id", id}, {"on", *c.on}};
        } else {
            out.path = "/relay/" + n + "?turn=" + (*c.on ? "on" : "off");
        }
        return true;
    }
    if ((gen >= 2 && comp == "cover") || (gen == 1 && comp == "roller")) {
        if (c.brightness || c.kelvin || c.hs || (c.transport && !stop) || other) {
            err = "This blind can only be opened, closed, stopped or moved to a position.";
            return false;
        }
        if (stop && (c.position || c.on)) {
            err = "A blind cannot be stopped and moved in one go.";
            return false;
        }
        int target = -1;
        if (c.position) target = std::max(0, std::min(100, *c.position));
        else if (c.on) target = *c.on ? 100 : 0;
        if (!stop && target < 0) {
            err = "Nothing was asked of the blind.";
            return false;
        }
        if (!stop && target > 0 && target < 100 && !ref_flag(thing, "calibrated")) {
            err = "This blind has not been calibrated in the Shelly app, so it can only be opened, closed or stopped.";
            return false;
        }
        if (gen >= 2) {
            out.params = {{"id", id}};
            if (stop) out.method = "Cover.Stop";
            else if (target == 100) out.method = "Cover.Open";
            else if (target == 0) out.method = "Cover.Close";
            else { out.method = "Cover.GoToPosition"; out.params["pos"] = target; }
        } else {
            out.path = "/roller/" + n + "?go=";
            if (stop) out.path += "stop";
            else if (target == 100) out.path += "open";
            else if (target == 0) out.path += "close";
            else out.path += "to_pos&roller_pos=" + std::to_string(target);
        }
        return true;
    }
    const bool light = gen >= 2 ? (comp == "light" || comp == "rgb" || comp == "rgbw" || comp == "cct") : comp == "light";
    if (!light) {
        err = "Zero Bot cannot change this part of the Shelly.";
        return false;
    }
    if (c.position || c.transport || other) {
        err = "This light can only be switched, dimmed" + std::string(thing.caps & home::kColor ? " or coloured." : ".");
        return false;
    }
    if (c.hs && !(thing.caps & home::kColor)) { err = "This light cannot show colours."; return false; }
    if (c.kelvin && !(thing.caps & home::kColorTemp)) { err = "This light has no setting for warmer or cooler white."; return false; }
    if (c.brightness && !(thing.caps & home::kBrightness)) { err = "This light cannot be dimmed."; return false; }
    std::optional<bool> on = c.on;
    std::optional<int> bri;
    if (c.brightness) bri = std::max(0, std::min(100, *c.brightness));
    if (bri && *bri == 0) { on = false; bri.reset(); }   // dimmed to nothing: off
    const bool values = bri || c.hs || c.kelvin;
    if (!on && values) on = true;                        // a brightness or colour asked for means the light on
    if (!on) { err = "Nothing was asked of the light."; return false; }
    const bool off = !*on;
    const int lowest = ref_int(thing, "min", 0, 100, 0);
    if (bri) bri = std::max(*bri, std::max(lowest, 1));
    std::optional<int> kelvin;
    if (c.kelvin && !off) {
        const int lo = ref_int(thing, "kmin", 1000, 10000, 2700), hi = ref_int(thing, "kmax", 1000, 10000, 6500);
        kelvin = std::max(lo, std::min(hi, *c.kelvin));
    }
    int r = 0, g = 0, b = 0;
    if (c.hs && !off) color::hs_to_rgb(c.hs->first, c.hs->second, 100, r, g, b);
    if (gen >= 2) {
        out.method = comp == "light" ? "Light.Set" : comp == "rgb" ? "RGB.Set" : comp == "rgbw" ? "RGBW.Set" : "CCT.Set";
        out.params = {{"id", id}, {"on", *on}};
        if (!off) {
            if (bri) out.params["brightness"] = *bri;
            if (c.hs) out.params["rgb"] = json::array({r, g, b});
            if (kelvin) out.params["ct"] = *kelvin;
        }
        return true;
    }
    std::string path = ref_text(thing, "path");
    if (path != "color" && path != "white") path = "light";
    out.path = "/" + path + "/" + n + "?turn=" + (off ? "off" : "on");
    if (!off) {
        if (bri) out.path += std::string(ref_flag(thing, "gain") ? "&gain=" : "&brightness=") + std::to_string(*bri);
        if (c.hs) out.path += "&red=" + std::to_string(r) + "&green=" + std::to_string(g) + "&blue=" + std::to_string(b);
        if (kelvin) out.path += "&temp=" + std::to_string(*kelvin);
    }
    return true;
}

bool allowed(const Call &call) {
    if (!call.method.empty()) {
        static const std::set<std::string> kMethods = {"Switch.Set", "Light.Set",  "RGB.Set",     "RGBW.Set",
                                                       "CCT.Set",    "Cover.Open", "Cover.Close", "Cover.Stop",
                                                       "Cover.GoToPosition", "Shelly.GetStatus", "Shelly.GetConfig"};
        static const std::set<std::string> kParams = {"id", "on", "brightness", "rgb", "ct", "pos"};
        if (!kMethods.count(call.method) || !call.path.empty()) return false;
        if (call.params.is_null()) return true;
        if (!call.params.is_object()) return false;
        for (auto it = call.params.begin(); it != call.params.end(); ++it)
            if (!kParams.count(it.key())) return false;
        return true;
    }
    const std::string &p = call.path;
    if (p == "/shelly" || p == "/settings" || p == "/status") return true;
    // Each path with the parameters it may carry: switching, dimming and colour, moving a blind.
    static const std::set<std::string> kLight = {"turn", "brightness", "gain", "red", "green", "blue", "temp"};
    static const std::map<std::string, std::set<std::string>> kPaths = {
        {"relay", {"turn"}}, {"light", kLight}, {"color", kLight}, {"white", kLight}, {"roller", {"go", "roller_pos"}}};
    if (p.size() < 4 || p[0] != '/') return false;
    const size_t slash = p.find('/', 1), q = p.find('?');
    if (slash == std::string::npos || q == std::string::npos || q < slash) return false;
    const auto path = kPaths.find(p.substr(1, slash - 1));
    if (path == kPaths.end()) return false;
    const std::string num = p.substr(slash + 1, q - slash - 1);
    if (num.size() > 2 || !all_digits(num)) return false;
    std::set<std::string> seen;
    for (const std::string &kv : util::split(p.substr(q + 1), '&')) {
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) return false;
        const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
        if (!path->second.count(k) || !seen.insert(k).second || v.empty() || v.size() > 6) return false;
        if (k == "turn") { if (v != "on" && v != "off") return false; }
        else if (k == "go") { if (v != "open" && v != "close" && v != "stop" && v != "to_pos") return false; }
        else if (!all_digits(v)) return false;
    }
    return true;
}

std::string rpc_path(const std::string &method, const json &params) {
    std::string path = "/rpc/" + method;
    if (!params.is_object() || params.empty()) return path;
    char sep = '?';
    auto add = [&](const std::string &key, const json &value) {
        path += sep + key + "=" + http::url_encode(value.dump());
        sep = '&';
    };
    if (params.contains("id")) add("id", params["id"]);
    for (auto it = params.begin(); it != params.end(); ++it)
        if (it.key() != "id") add(it.key(), *it);
    return path;
}

bool read_rpc_answer(long status, const std::string &body, json &result, std::string &err, int *code) {
    result = json();
    if (code) *code = 0;
    json j;
    if (status >= 200 && status < 300) {
        if (!parse(body, j)) { err = kNotUnderstood; return false; }
        result = j;
        return true;
    }
    // An error: {"code": ..., "message": ...}, with a 4xx or 5xx status.
    if (!parse(body, j) || !j.is_object() || !member(j, "code")) { err = kNotUnderstood; return false; }
    {
        const int c = whole(member(j, "code"), -1000000, 1000000, 0);
        if (code) *code = c;
        switch (c) {
            case -103: err = "The Shelly did not accept that command."; break;
            case -104: err = "The Shelly ran out of time doing that."; break;
            case -105: err = std::string("The Shelly no longer has that channel. ") + kReadAgain; break;
            case -108: err = "The Shelly is busy; try again in a moment."; break;
            case -109: err = "The Shelly could not do that now: it reports a fault, such as overheating, or a blind whose "
                             "position it does not know."; break;
            case -114: err = "The Shelly says that part of it is unavailable just now."; break;
            default: err = "The Shelly refused that command."; break;
        }
        return false;
    }
}

}  // namespace detail

// ================================================================ talking to the device
namespace {

using detail::Call;
using detail::Identity;
using Clock = std::chrono::steady_clock;

struct Deadline {
    Clock::time_point end;
    explicit Deadline(int ms) : end(Clock::now() + std::chrono::milliseconds(std::max(ms, 0))) {}
    long left() const {
        const long ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count());
        return ms > 0 ? ms : 0;
    }
};

struct Target {
    std::string ip, key;   // key: what the family's secrets are kept under
    int port = 80;
};

struct Login {
    std::string user, password;
    bool have() const { return !password.empty(); }
};

struct Reply {
    long status = 0;
    std::string body;
    bool answered = false, too_long = false, no_time = false, sent_login = false;
};

std::string device_key(const home::Device &dev) { return dev.id.empty() ? dev.ip : dev.id; }

bool target_of(const home::Device &dev, Target &t, std::string &err) {
    if (!detail::valid_ipv4(dev.ip)) {
        err = "The Shelly's saved address cannot be used. Scan the network again in Settings, Smart home.";
        return false;
    }
    t.ip = dev.ip;
    t.port = dev.port > 0 && dev.port <= 65535 ? dev.port : 80;
    t.key = device_key(dev);
    return true;
}

Login login_of(const home::Device &dev, int gen) {
    Login l;
    l.password = text(member(dev.auth, "password"));
    l.user = gen >= 2 ? "admin" : text(member(dev.auth, "user"));
    if (l.user.empty()) l.user = "admin";
    return l;
}

// One HTTP exchange. A password goes as HTTP digest (Gen2+, which libcurl answers only after the
// device's challenge) or as basic authentication (Gen1).
Reply exchange(const Target &t, const char *method, const std::string &path, const std::string &body, int gen,
               const Login *login, const Deadline &dl) {
    Reply r;
    const long left = dl.left();
    if (left < 50) { r.no_time = true; return r; }
    http::Request req;
    req.local = true;
    req.method = method;
    req.url = "http://" + t.ip + ":" + std::to_string(t.port) + path;
    req.timeout_ms = left;
    req.connect_timeout_ms = std::min(left, 2000L);
    if (!body.empty()) {
        req.body = body;
        req.headers.push_back("Content-Type: application/json");
    }
    if (login && login->have()) {
        if (gen >= 2) {
            req.digest_user = "admin";
            req.digest_pass = login->password;
        } else {
            req.headers.push_back(detail::basic_auth_header(login->user, login->password));
        }
        r.sent_login = true;
    }
    std::atomic<bool> cancel{false};
    std::string collected;
    bool too_long = false;
    req.cancel = &cancel;
    req.on_data = [&](const char *p, size_t n) {
        if (too_long) return;
        if (collected.size() + n > kMaxBody) { too_long = true; cancel = true; return; }
        collected.append(p, n);
    };
    const http::Response res = http::send(req);
    r.status = res.status;
    r.too_long = too_long || res.body.size() > kMaxBody;
    r.answered = res.error.empty() || too_long;
    if (r.answered) r.body = res.status >= 200 && res.status < 300 ? collected : res.body;
    return r;
}

}  // namespace

// ================================================================ the driver
namespace {

class ShellyDriver : public home::Driver {
public:
    const char *kind() const override { return kFamily; }

    home::PairStep next_step(const home::Device &dev) const override {
        home::PairStep step;
        const std::string key = device_key(dev);
        const int gen = locked_gen(key);
        const Login login = login_of(dev, gen);
        const bool refused = login.have() && is_refused(key, login);
        if (login.have() && !refused) return step;   // ready
        if (gen == 0 && !refused) return step;       // no password asked for, as far as is known
        if (gen == 1 && (refused || text(member(dev.auth, "user")).empty())) {
            step.kind = home::Pairing::EnterCode;
            step.prompt = refused ? "The Shelly did not accept the saved login, so nothing more is sent to it. Type its user "
                                    "name again (it is admin unless it was changed); its password is asked for next."
                                  : "This Shelly asks for a login, set up in the Shelly app. Type its user name (it is admin "
                                    "unless it was changed); its password is asked for next.";
            step.field = "User name";
            return step;
        }
        step.kind = home::Pairing::Password;
        step.prompt = refused ? "The Shelly did not accept the saved password, so nothing more is sent to it. Type the "
                                "password again; capitals count."
                              : "This Shelly asks for a password, set by its owner in the Shelly app. Type it; capitals "
                                "count. It is kept on this handheld only.";
        step.field = "Shelly password";
        return step;
    }

    bool pair(home::Device &dev, const std::string &input, std::string &err) override {
        err.clear();
        if (!dev.auth.is_object()) dev.auth = json::object();
        const home::PairStep step = next_step(dev);
        const std::string key = device_key(dev);
        if (step.kind == home::Pairing::EnterCode) {
            const std::string user = util::trim(input);
            if (user.empty() || user.size() > 64 || user.find(':') != std::string::npos ||
                std::any_of(user.begin(), user.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; })) {
                err = "That cannot be a Shelly user name (no colons, at most 64 characters).";
                return false;
            }
            // A new user name starts afresh: its password is asked for next. The device the login
            // is for stays the same.
            const std::string paired = text(member(dev.auth, "dev"));
            dev.auth = {{"user", user}};
            if (!paired.empty()) dev.auth["dev"] = paired;
            forget(key, false);
            return true;
        }
        if (step.kind != home::Pairing::Password) return true;
        if (input.empty() || input.size() > 256) { err = "Type the password."; return false; }
        Target t;
        if (!target_of(dev, t, err)) return false;
        const Deadline dl(kListMs);
        // What was typed is kept (a password typed again is no longer refused), but it goes only to
        // the device it is for: after a change of address another Shelly would be sent it (in the
        // clear, for Gen1), and its refusal would be taken for this one's.
        dev.auth["password"] = input;
        if (locked_gen(key) >= 2) dev.auth["user"] = "admin";
        forget(key, false);
        Identity who;
        if (!identify(t, dl, who, err)) {
            err += " The password is kept, and is tried when the device is next read.";
            return false;
        }
        if (!same_device(dev.things, dev.auth, who, dev.things.empty() ? 0 : ref_int(dev.things[0], "gen", 1, 9, 0), err)) {
            err += " The password typed is kept.";
            return false;
        }
        const int gen = who.gen;
        if (gen >= 2) dev.auth["user"] = "admin";
        // One request to check it. A refusal is not kept as the device's password at all.
        const Login login = login_of(dev, gen);
        const Reply r = exchange(t, "GET", gen >= 2 ? detail::rpc_path("Shelly.GetStatus", json()) : std::string("/status"), "", gen,
                                 &login, dl);
        if (r.answered && !r.too_long && r.status == 401) {
            dev.auth.erase("password");
            if (gen == 1) dev.auth.erase("user");
            err = gen == 1 ? "The Shelly did not accept this user name and password. Type them again; capitals count."
                           : "The Shelly did not accept this password. Capitals count; type it again.";
            return false;
        }
        // From now on the password belongs to the device that answered, and goes to no other, even
        // before its things are read (one with nothing to offer never has any to tell it apart).
        dev.auth["dev"] = who.who();
        if (r.answered && r.status == 429) {
            err = std::string(kThrottled) + " after wrong passwords. The password is saved and will be tried when the "
                                            "device is next read; wait a few minutes first.";
            return false;
        }
        if (!r.answered) {
            err = std::string("The password is saved, but the Shelly did not answer.");
            return false;
        }
        if (r.status < 200 || r.status >= 300) {
            err = "The password is saved, but the Shelly reported an error when asked with it.";
            return false;
        }
        return true;
    }

    void unpair(home::Device &dev) override {
        forget(device_key(dev), true);
        dev.auth = json::object();
    }

    bool enumerate(home::Device &dev, std::vector<home::Thing> &out, std::string &err) override {
        out.clear();
        err.clear();
        try {
            Target t;
            if (!target_of(dev, t, err)) return false;
            const Deadline dl(kListMs);
            Identity who;
            if (!identify(t, dl, who, err)) return false;
            // A device that answers at the address of another, already read or paired, is not taken
            // for it (nor sent its password).
            if (!same_device(dev.things, dev.auth, who, 0, err)) return false;
            const Login login = login_of(dev, who.gen);
            if (!may_log_in(t, who, login, err)) return false;
            std::string name;
            if (who.gen >= 2) {
                json config, status;
                if (!rpc(t, who.gen, &login, "Shelly.GetConfig", dl, config, err) ||
                    !rpc(t, who.gen, &login, "Shelly.GetStatus", dl, status, err))
                    return false;
                out = detail::things_gen2(who, config, status, name);
            } else {
                json settings, status;
                if (!get_json(t, &login, "/settings", dl, settings, err) || !get_json(t, &login, "/status", dl, status, err))
                    return false;
                out = detail::things_gen1(who, settings, status, name);
            }
            if (!name.empty()) dev.name = name;
            else if (dev.name.empty()) dev.name = "Shelly " + model_name(who);
            const std::string model = model_name(who);
            if (!model.empty()) dev.model = model;
            if (out.empty()) {
                err = "This Shelly has nothing Zero Bot may use: no switch, light, blind or sensor it offers.";
                return false;
            }
            return true;
        } catch (const std::exception &) {
            out.clear();
            err = kNotUnderstood;
            return false;
        }
    }

    bool read(const home::Device &dev, std::vector<home::Thing> &things, std::string &err) override {
        err.clear();
        try {
            if (things.empty()) things = dev.things;
            if (things.empty()) return true;
            Target t;
            if (!target_of(dev, t, err)) return false;
            const int gen = ref_int(things[0], "gen", 1, 9, 0);
            if (gen == 0) {
                err = std::string("Zero Bot does not know this Shelly properly. ") + kReadAgain;
                return false;
            }
            const Login login = login_of(dev, gen);
            if (!ready(t, gen, login, err)) return false;
            const Deadline dl(kListMs);
            // Who answers, first: a password goes only to the device it is for (see pair()), and
            // another device's answer (its 401, a web server's 404) is not kept as this one's.
            Identity who;
            if (!identify(t, dl, who, err) || !same_device(things, dev.auth, who, gen, err)) return false;
            if (!may_log_in(t, who, login, err)) return false;
            json status;
            if (gen >= 2) {
                if (!rpc(t, gen, &login, "Shelly.GetStatus", dl, status, err)) return false;
            } else if (!get_json(t, &login, "/status", dl, status, err)) {
                return false;
            }
            // The status names the device's MAC (Gen1 "mac", Gen2+ "sys.mac"): another device at
            // this address is not read as this one.
            const json *sys = gen >= 2 ? member_obj(status, "sys") : &status;
            const std::string mac = sys ? lower_hex_mac(text(member(*sys, "mac"))) : "";
            const std::string known = known_device(things, "mac");
            if (!known.empty() && !mac.empty() && mac != known) {
                err = std::string(kOtherDevice) + " Scan the network again in Settings, Smart home.";
                return false;
            }
            for (auto &th : things) th.state = detail::state_of(th, status);
            return true;
        } catch (const std::exception &) {
            err = kNotUnderstood;
            return false;
        }
    }

    bool apply(const home::Device &dev, const home::Thing &thing, const home::Change &change, int deadline_ms,
               std::string &err) override {
        err.clear();
        try {
            Call call;
            if (!detail::plan(thing, change, call, err)) return false;
            if (!detail::allowed(call)) {   // never reached: plan() makes only allowed calls
                err = "Zero Bot will not send that to a Shelly.";
                return false;
            }
            Target t;
            if (!target_of(dev, t, err)) return false;
            const int gen = ref_int(thing, "gen", 1, 9, 0);
            const Login login = login_of(dev, gen);
            if (login.have() && is_refused(t.key, login)) return refused_error(err);
            const Deadline dl(deadline_ms);
            Identity who;
            if (!identify(t, dl, who, err) || !same_device({thing}, dev.auth, who, gen, err)) return false;
            if (!may_log_in(t, who, login, err)) return false;
            // A relay or blind is asked again what it is now: renamed "Garage door" in the Shelly app,
            // given a switch-off timer of a second (an opener's pulse), or a 2PM changed to the other
            // profile since it was read, it is not switched. Lights have no such rule.
            const std::string comp = ref_text(thing, "comp");
            if (comp == "switch" || comp == "cover" || comp == "relay" || comp == "roller") {
                json config;
                if (gen >= 2 ? !rpc(t, gen, &login, "Shelly.GetConfig", dl, config, err) : !get_json(t, &login, "/settings", dl, config, err))
                    return false;
                if (!detail::recheck(thing, config, err)) return false;
            }
            if (gen >= 2) {
                json result;
                return rpc(t, gen, &login, call.method, dl, result, err, call.params);
            }
            const Reply r = exchange(t, "GET", call.path, "", gen, &login, dl);
            if (!settle(t, gen, login, r, err)) return false;
            return true;
        } catch (const std::exception &) {
            err = kNotUnderstood;
            return false;
        }
    }

private:
    // ---- the family's secrets: {"locked": {device: generation}, "refused": {device: fingerprint}}
    json family() const { return secrets_ ? secrets_->get(kFamily) : json::object(); }

    int locked_gen(const std::string &key) const {
        const json f = family();
        const json *locked = member_obj(f, "locked");
        return locked ? whole(member(*locked, key.c_str()), 1, 9, 0) : 0;
    }

    bool is_refused(const std::string &key, const Login &login) const {
        const json f = family();
        const json *refused = member_obj(f, "refused");
        return refused && text(member(*refused, key.c_str())) == detail::fingerprint(login.user, login.password);
    }

    // Several devices are read at once (the app uses up to four threads), so the family's secrets
    // are read, changed and written under one lock: two refusals at the same moment must both stay.
    void note(const char *list, const std::string &key, const json &value) {
        if (!secrets_) return;
        std::lock_guard<std::mutex> lk(secrets_mtx_);
        json f = secrets_->get(kFamily);
        if (!f.is_object()) f = json::object();
        if (!f.contains(list) || !f[list].is_object()) f[list] = json::object();
        if (f[list].contains(key) && f[list][key] == value) return;
        f[list][key] = value;
        secrets_->set(kFamily, f);
    }

    void forget(const std::string &key, bool locked_too) {
        if (!secrets_) return;
        std::lock_guard<std::mutex> lk(secrets_mtx_);
        json f = secrets_->get(kFamily);
        if (!f.is_object()) return;
        bool changed = false;
        for (const char *list : {"refused", "locked"}) {
            if (!locked_too && std::string(list) == "locked") continue;
            if (f.contains(list) && f[list].is_object() && f[list].contains(key)) {
                f[list].erase(key);
                if (f[list].empty()) f.erase(list);
                changed = true;
            }
        }
        if (changed) secrets_->set(kFamily, f);
    }

    bool refused_error(std::string &err) const {
        err = std::string(kRefused) + " so nothing more is sent to it until the password is typed again in Settings, Smart "
                                      "home, on the device's page.";
        return false;
    }

    // What this driver recorded of the device it read: "dev", its id (Gen2+) or MAC (Gen1), or "mac".
    static std::string known_device(const std::vector<home::Thing> &things, const char *key) {
        for (const auto &th : things)
            if (const std::string who = ref_text(th, key); !who.empty()) return who;
        return "";
    }

    // Whether the device that answered /shelly is the one these things were read from, and the one
    // the saved password was typed for (auth "dev"): its id, or for Gen1 its MAC. And of the same
    // generation (`gen`, from the things' refs; 0 when not known).
    static bool same_device(const std::vector<home::Thing> &things, const json &auth, const Identity &who, int gen, std::string &err) {
        for (const std::string &known : {known_device(things, "dev"), text(member(auth, "dev"))})
            if (!known.empty() && known != who.who()) {
                err = std::string(kOtherDevice) + " Nothing was sent to it. Scan the network again in Settings, Smart home.";
                return false;
            }
        if (gen > 0 && (who.gen >= 2) != (gen >= 2)) {
            err = std::string("The Shelly has changed since it was read. ") + kReadAgain;
            return false;
        }
        return true;
    }

    // Nothing is sent with a refused password, and nothing that needs one without it.
    bool ready(const Target &t, int gen, const Login &login, std::string &err) const {
        if (login.have() && is_refused(t.key, login)) return refused_error(err);
        if (!login.have() && locked_gen(t.key) > 0) {
            err = std::string(kNeedsPassword) + kTypeIt;
            return false;
        }
        return true;
    }

    // After GET /shelly: whether the next requests may go, given what the device says of its login.
    bool may_log_in(const Target &t, const Identity &who, const Login &login, std::string &err) {
        if (login.have() && is_refused(t.key, login)) return refused_error(err);
        if (who.auth && !login.have()) {
            note("locked", t.key, who.gen);
            err = std::string(kNeedsPassword) + kTypeIt;
            return false;
        }
        if (!who.auth && !login.have()) forget(t.key, true);   // the owner removed the password
        else if (who.auth) note("locked", t.key, who.gen);
        return true;
    }

    // GET /shelly, which never needs a password.
    bool identify(const Target &t, const Deadline &dl, Identity &who, std::string &err) {
        const Reply r = exchange(t, "GET", "/shelly", "", 1, nullptr, dl);
        // /shelly answers on every Shelly, without a password: any other answer is another device.
        if (r.answered && !r.too_long && r.status != 429 && (r.status < 200 || r.status >= 300)) {
            err = std::string(kOtherDevice) + " Scan the network again in Settings, Smart home.";
            return false;
        }
        if (!settle(t, 1, Login(), r, err)) return false;
        if (!detail::parse_identity(r.body, who)) {
            err = kNotUnderstood;
            return false;
        }
        return true;
    }

    // Turns a reply into an error sentence and remembers what it says about the login. True for a
    // 2xx answer that fits in memory.
    bool settle(const Target &t, int gen, const Login &login, const Reply &r, std::string &err) {
        if (r.no_time) { err = "There was no time left to ask the Shelly. Try again in a moment."; return false; }
        if (r.too_long) { err = "The Shelly's answer was too long to read."; return false; }
        if (!r.answered) { err = kNoAnswer; return false; }
        if (r.status == 401) {
            if (r.sent_login) {
                note("refused", t.key, detail::fingerprint(login.user, login.password));
                note("locked", t.key, gen);
                return refused_error(err);
            }
            note("locked", t.key, gen);
            err = std::string(kNeedsPassword) + kTypeIt;
            return false;
        }
        if (r.status == 429) {
            err = std::string(kThrottled) + " after wrong passwords. Try again in a few minutes.";
            return false;
        }
        if (r.status >= 200 && r.status < 300) return true;
        if (r.status == 404) err = std::string("The Shelly does not have that channel. ") + kReadAgain;
        else if (r.status == 400) err = "The Shelly did not accept that command.";
        else err = "The Shelly reported an error.";
        return false;
    }

    bool get_json(const Target &t, const Login *login, const char *path, const Deadline &dl, json &out, std::string &err) {
        const Reply r = exchange(t, "GET", path, "", 1, login, dl);
        if (!settle(t, 1, login ? *login : Login(), r, err)) return false;
        if (!parse(r.body, out) || !out.is_object()) {
            err = kNotUnderstood;
            return false;
        }
        return true;
    }

    // One Gen2+ call, as GET /rpc/<Method>?<params>. Not POST /rpc: with digest authentication
    // libcurl first sends a POST with an empty body to draw the device's challenge, and a device
    // without a password would take that empty body as the call.
    bool rpc(const Target &t, int gen, const Login *login, const std::string &method, const Deadline &dl, json &result,
             std::string &err, const json &params = json()) {
        const Reply r = exchange(t, "GET", detail::rpc_path(method, params), "", gen, login, dl);
        const Login &l = login ? *login : Login();
        // An error comes as {"code", "message"} with a 4xx or 5xx status.
        const bool error = r.answered && !r.too_long && r.status >= 400 && r.status != 401 && r.status != 429 && !r.body.empty();
        if (!error && !settle(t, gen, l, r, err)) return false;
        int code = 0;
        const bool ok = detail::read_rpc_answer(r.status, r.body, result, err, &code);
        if (error && code == 0) return settle(t, gen, l, r, err);   // not an error the device described
        return ok;
    }

    std::mutex secrets_mtx_;
};

}  // namespace
}  // namespace shelly

std::unique_ptr<home::Driver> make_shelly_driver() { return std::make_unique<shelly::ShellyDriver>(); }
