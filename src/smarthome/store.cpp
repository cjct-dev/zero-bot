// The smart-home file: networks the user has scanned, the devices found on each, the things
// those devices offer, and the secrets needed to talk to them. One file, mode 0600.
//
//   { "version": 2,
//     "secrets":  { "<family>": {...} },                       shared by a family (an account, a key pair)
//     "networks": { "<network id>": { "label", "gateway_mac", "allowed_at", "last_scan",
//                   "devices": [ { "id", "kind", "name", ..., "auth": {...}, "things": [ {...} ] } ] } } }
//
// Files written before things existed held "switches" per device and the Tapo account at the
// top level; they are rewritten in the new form the first time they are loaded.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <set>

#include "smarthome/internal.h"

namespace home {

namespace {

std::mutex g_mtx;            // guards the file
std::mutex g_net_mtx;        // guards the cached network and the test hooks
const discovery::Network *g_test_network = nullptr;
const testing::NetworkSource *g_test_source = nullptr;
unsigned long (*g_test_clock)() = nullptr;

std::mutex g_drivers_mtx;
std::vector<std::unique_ptr<Driver>> g_drivers;
std::map<const Driver *, std::string> g_driver_names;
std::once_flag g_builtin_once;
bool g_skip_builtin = false;

constexpr uint32_t kKnownCaps = (1u << 14) - 1;
const char *const kTypes[] = {"light", "plug", "switch", "thermostat", "cover", "fan", "speaker", "tv", "scene", "sensor"};
constexpr time_t kKeepGoneFor = 30 * 86400;

std::string path() { return util::data_dir() + "/smarthome.json"; }

// What network_now() reports while ZB_TEST_NETWORK is set. Its address is the loopback one, and
// may_contact() keeps every driver call on 127.x while it is in use.
discovery::Network stand_in() {
    discovery::Network net;
    net.iface = "test";
    net.ssid = "Test network";
    net.address = "127.0.0.1";
    net.broadcast = "127.255.255.255";
    net.gateway = "127.0.0.1";
    net.gateway_mac = "02:00:00:00:00:00";
    net.usable = true;
    return net;
}

bool env_test_network() {
    const char *v = getenv("ZB_TEST_NETWORK");
    return v && *v && std::string(v) != "0";
}

// The twelve hex digits of a MAC address in any of the notations saved ("aa:bb:..", "AA-BB-..",
// none), lower case; "" when it is not one, or is all zeros.
std::string mac_key(const std::string &raw) {
    std::string hex;
    for (const char c : raw) {
        if (c == ':' || c == '-') continue;
        if (!isxdigit(static_cast<unsigned char>(c)) || hex.size() >= 12) return "";
        hex += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return hex.size() == 12 && hex != "000000000000" ? hex : "";
}

// A plug or switch whose name sounds like a heater: the app asks before switching it on.
bool sounds_like_heater(const Thing &t) { return (t.type == "plug" || t.type == "switch") && suggests_heat(t.name); }

// A thing as the app first meets it: a room guessed from its name, and a question before a
// plug that sounds like a heater is switched on. The user can change both.
void first_seen_defaults(Thing &t) {
    if (t.room.empty()) t.room = infer_room(t.name);
    if (sounds_like_heater(t)) t.ask_first = true;
}

json thing_to_json(const Thing &t, const ThingMeta &m) {
    json j = {{"id", t.id}, {"name", t.name}, {"alias", t.alias}, {"room", t.room}, {"type", t.type}, {"caps", t.caps},
              {"enabled", t.enabled}, {"ask_first", t.ask_first},
              {"ref", t.ref.is_object() ? t.ref : json::object()}, {"state", t.state.is_object() ? t.state : json::object()}};
    if (m.room_by_user) j["room_by_user"] = true;
    if (m.used) j["used"] = static_cast<long long>(m.used);
    if (m.gone) j["gone"] = static_cast<long long>(m.gone);
    return j;
}

bool thing_from_json(const json &j, Thing &t, ThingMeta &m) {
    using util::jget;
    if (!j.is_object()) return false;
    t.id = jget(j, "id", std::string());
    t.name = clean_name(jget(j, "name", std::string()));
    t.alias = clean_name(jget(j, "alias", std::string()));
    t.room = clean_name(jget(j, "room", std::string()));
    t.type = jget(j, "type", std::string());
    t.caps = jget<uint32_t>(j, "caps", 0) & kKnownCaps;
    t.enabled = jget(j, "enabled", true);
    t.ask_first = jget(j, "ask_first", false);
    t.ref = j.contains("ref") && j["ref"].is_object() ? j["ref"] : json::object();
    t.state = j.contains("state") && j["state"].is_object() ? j["state"] : json::object();
    m.room_by_user = jget(j, "room_by_user", false);
    m.used = jget<long long>(j, "used", 0);
    m.gone = jget<long long>(j, "gone", 0);
    return !t.id.empty() && allowed_type(t.type);
}

json to_json(const Saved &d) {
    json things = json::array();
    for (const auto &t : d.things) {
        const auto m = d.meta.find(t.id);
        things.push_back(thing_to_json(t, m == d.meta.end() ? ThingMeta() : m->second));
    }
    return {{"id", d.id}, {"kind", d.kind}, {"name", d.name}, {"model", d.model}, {"mac", d.mac}, {"last_ip", d.ip},
            {"port", d.port}, {"info", d.info}, {"found_by", d.found_by}, {"enabled", d.enabled},
            {"needs_setup", d.needs_setup}, {"last_seen", static_cast<long long>(d.last_seen)}, {"problem", d.problem},
            {"auth", d.auth.is_object() ? d.auth : json::object()}, {"things", things}};
}

Saved from_json(const json &j) {
    using util::jget;
    Saved d;
    d.id = jget(j, "id", std::string()); d.kind = jget(j, "kind", std::string());
    d.name = jget(j, "name", std::string()); d.model = jget(j, "model", std::string());
    d.mac = jget(j, "mac", std::string()); d.ip = jget(j, "last_ip", std::string());
    d.port = jget(j, "port", 0); d.enabled = jget(j, "enabled", true);
    d.needs_setup = jget(j, "needs_setup", false);
    d.last_seen = jget<long long>(j, "last_seen", 0); d.problem = jget(j, "problem", std::string());
    if (j.contains("info") && j["info"].is_object())
        for (auto it = j["info"].begin(); it != j["info"].end(); ++it)
            if (it.value().is_string()) d.info[it.key()] = it.value().get<std::string>();
    if (j.contains("found_by") && j["found_by"].is_array())
        for (const auto &m : j["found_by"])
            if (m.is_string()) d.found_by.push_back(m.get<std::string>());
    if (j.contains("auth") && j["auth"].is_object()) d.auth = j["auth"];
    if (j.contains("things") && j["things"].is_array())
        for (const auto &tj : j["things"]) {
            Thing t;
            ThingMeta m;
            if (!thing_from_json(tj, t, m) || d.thing(t.id)) continue;
            if (m.room_by_user || m.used || m.gone) d.meta[t.id] = m;
            d.things.push_back(std::move(t));
        }
    return d;
}

NetworkEntry entry_from(const std::string &id, const json &n) {
    NetworkEntry e;
    e.id = id;
    e.label = util::jget(n, "label", std::string());
    e.gateway_mac = util::jget(n, "gateway_mac", std::string());
    e.allowed_at = util::jget<long long>(n, "allowed_at", 0);
    e.last_scan = util::jget<long long>(n, "last_scan", 0);
    if (n.contains("devices") && n["devices"].is_array())
        for (const auto &d : n["devices"])
            if (d.is_object()) e.devices.push_back(from_json(d));
    return e;
}

void store_entry(json &root, const NetworkEntry &e) {
    json devices = json::array();
    for (const auto &d : e.devices) devices.push_back(to_json(d));
    root["networks"][e.id] = {{"label", e.label}, {"gateway_mac", e.gateway_mac},
                              {"allowed_at", static_cast<long long>(e.allowed_at)},
                              {"last_scan", static_cast<long long>(e.last_scan)}, {"devices", devices}};
}

// Brings a file written by an earlier version into today's form. Returns whether it changed.
bool migrate(json &root) {
    bool changed = false;
    if (!root.contains("secrets") || !root["secrets"].is_object()) root["secrets"] = json::object();
    if (root.contains("tapo")) {
        // The Tapo account used to sit at the top level.
        if (root["tapo"].is_object() && !root["tapo"].empty() && !root["secrets"].contains("tapo")) root["secrets"]["tapo"] = root["tapo"];
        root.erase("tapo");
        changed = true;
    }
    for (auto &net : root["networks"]) {
        if (!net.is_object() || !net.contains("devices") || !net["devices"].is_array()) continue;
        for (auto &dev : net["devices"]) {
            if (!dev.is_object() || !dev.contains("switches")) continue;
            // Each switch (a plug, or one socket of a strip) becomes a thing.
            if (!dev.contains("things") && dev["switches"].is_array()) {
                json things = json::array();
                std::set<std::string> ids;
                for (const auto &sw : dev["switches"]) {
                    if (!sw.is_object()) continue;
                    const std::string child = util::jget(sw, "child", std::string());
                    Thing t;
                    t.id = child.empty() ? "main" : child;
                    if (!ids.insert(t.id).second) continue;
                    t.name = clean_name(util::jget(sw, "name", std::string()));
                    t.type = "plug";
                    t.caps = kPower;
                    t.ref = {{"child", child}};
                    t.state = {{"on", util::jget(sw, "on", false)}};
                    first_seen_defaults(t);
                    things.push_back(thing_to_json(t, ThingMeta()));
                }
                dev["things"] = things;
            }
            dev.erase("switches");
            changed = true;
        }
    }
    return changed;
}

json load_root() {   // the caller holds g_mtx
    json j = util::load_json(path(), json::object());
    if (!j.is_object()) j = json::object();
    if (!j.contains("networks") || !j["networks"].is_object()) j["networks"] = json::object();
    if (migrate(j)) {
        j["version"] = 2;
        util::save_json(path(), j);
    }
    return j;
}

void save_root(json &root) {   // the caller holds g_mtx
    root["version"] = 2;
    util::save_json(path(), root);
}

class FileSecrets : public Secrets {
public:
    json get(const std::string &family) const override {
        std::lock_guard<std::mutex> lk(g_mtx);
        const json root = load_root();
        const auto it = root["secrets"].find(family);
        return it != root["secrets"].end() && it->is_object() ? *it : json::object();
    }
    void set(const std::string &family, const json &value) override {
        std::lock_guard<std::mutex> lk(g_mtx);
        json root = load_root();
        if (value.is_object() && !value.empty()) root["secrets"][family] = value;
        else root["secrets"].erase(family);
        save_root(root);
    }
};

FileSecrets g_secrets;

void ensure_builtin() {
    std::call_once(g_builtin_once, [] {
        if (!g_skip_builtin) register_builtin_drivers();
    });
}

void edit_thing(const std::string &network_id, const std::string &device_id, const std::string &thing_id,
                const std::function<void(Thing &, ThingMeta &)> &change) {
    modify_entry(network_id, [&](NetworkEntry &e) {
        for (auto &d : e.devices)
            if (d.id == device_id)
                for (auto &t : d.things)
                    if (t.id == thing_id) change(t, d.meta[t.id]);
    });
}

}  // namespace

bool allowed_type(const std::string &type) {
    return std::find_if(std::begin(kTypes), std::end(kTypes), [&](const char *t) { return type == t; }) != std::end(kTypes);
}

// ---------------------------------------------------------------- drivers and secrets
void register_driver(std::unique_ptr<Driver> driver, const std::string &name) {
    if (!driver) return;
    driver->attach(&g_secrets);
    std::lock_guard<std::mutex> lk(g_drivers_mtx);
    if (!name.empty()) g_driver_names[driver.get()] = name;
    g_drivers.push_back(std::move(driver));
}

std::string kind_label(const Device &device) {
    if (const Driver *drv = driver_for(device)) {
        std::lock_guard<std::mutex> lk(g_drivers_mtx);
        const auto it = g_driver_names.find(drv);
        if (it != g_driver_names.end()) return it->second;
    }
    // Kinds discovery reports that no driver built into the app may claim yet.
    static const std::pair<const char *, const char *> kKnown[] = {
        {"tapo", "Tapo"}, {"homekit", "HomeKit"}, {"hue_bridge", "Hue Bridge"}, {"cast", "Cast"}, {"fire_tv", "Fire TV"},
        {"matter", "Matter"}, {"shelly", "Shelly"}, {"wled", "WLED"}, {"esphome", "ESPHome"}, {"elgato", "Elgato"},
        {"nanoleaf", "Nanoleaf"}, {"sonos", "Sonos"}, {"android_tv", "Android TV"}, {"roku", "Roku"}, {"wemo", "WeMo"},
        {"tplink", "TP-Link"}, {"airplay", "AirPlay"}, {"wiz", "WiZ"}, {"kasa", "Kasa"}};
    for (const auto &k : kKnown)
        if (device.kind == k.first) return k.second;
    return "Device";
}

Driver *driver_for(const std::string &kind) {
    ensure_builtin();
    std::lock_guard<std::mutex> lk(g_drivers_mtx);
    for (const auto &d : g_drivers)
        if (kind == d->kind()) return d.get();
    return nullptr;
}

Driver *driver_for(const Device &device) {
    ensure_builtin();
    std::lock_guard<std::mutex> lk(g_drivers_mtx);
    for (const auto &d : g_drivers)
        if (d->claims(device.kind, device.info)) return d.get();
    return nullptr;
}

Secrets &secrets() { return g_secrets; }

bool Saved::controllable() const { return driver_for(*this) != nullptr; }

bool Saved::listed(const Thing &t) const {
    const auto m = meta.find(t.id);
    return m == meta.end() || !m->second.gone;
}

bool Saved::any_listed() const {
    return std::any_of(things.begin(), things.end(), [this](const Thing &t) { return listed(t); });
}

std::string Saved::title() const {
    const Thing *only = nullptr;
    int count = 0;
    for (const auto &t : things) {
        if (!listed(t)) continue;
        only = &t;
        ++count;
    }
    if (count == 1 && !spoken(*only).empty()) return spoken(*only);
    return name.empty() ? kind : name;
}

const Thing *Saved::thing(const std::string &thing_id) const {
    for (const auto &t : things)
        if (t.id == thing_id) return &t;
    return nullptr;
}

// ---------------------------------------------------------------- the network
unsigned long now_ms() {
    if (g_test_clock) return g_test_clock();
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return static_cast<unsigned long>(duration_cast<milliseconds>(steady_clock::now() - t0).count());
}

bool test_network() {
    {
        std::lock_guard<std::mutex> lk(g_net_mtx);
        if (g_test_network) return true;
    }
    return env_test_network();
}

// The full look (the Wi-Fi name, the router's hardware address) costs a subprocess and is kept for
// a few seconds; whether it still describes the network is checked on every call, from the
// default route and the router's address in the neighbour table, which cost a file read each. So
// the moment the connection moves (to a phone's tether, to another network) the old answer is
// dropped, and nothing saved for the old network is offered or contacted.
discovery::Network network_now(bool fresh) {
    static discovery::Network cached;
    static unsigned long at = 0;
    std::lock_guard<std::mutex> lk(g_net_mtx);
    if (g_test_network) return *g_test_network;
    if (env_test_network()) return stand_in();
    const testing::NetworkSource *src = g_test_source;
    const unsigned long now = now_ms();
    bool moved = false;
    if (at != 0 && !fresh) {
        const discovery::Route route = src && src->route ? src->route() : discovery::default_route();
        moved = route.iface != cached.iface || route.gateway != cached.gateway;
        if (!moved && cached.usable) {
            // Another router at the same address, or one not known yet: right after joining another
            // network (many home routers use the same IP address) the system has not learnt the new
            // router's hardware address. Either way the full look decides; it makes the system learn it.
            const std::string mac = src && src->mac_of ? src->mac_of(cached.gateway) : discovery::mac_of(cached.gateway);
            moved = mac != cached.gateway_mac;
        }
    }
    if (fresh || moved || at == 0 || now - at > 8000) {
        cached = src && src->current ? src->current() : discovery::current_network();
        at = now ? now : 1;
    }
    return cached;
}

bool scan(std::vector<discovery::Device> &found, std::string &err, bool *heard_self) {
    found.clear();
    if (heard_self) *heard_self = false;
    if (test_network()) { err = "Nothing is scanned while the test network is in use."; return false; }
    discovery::Options opt;
    return discovery::scan(opt, found, err, heard_self);
}

bool on_network(const std::string &network_id, std::string &why_not) {
    const discovery::Network net = network_now();
    if (!net.usable) { why_not = net.why_not.empty() ? "There is no home network right now." : net.why_not; return false; }
    if (net.id() != network_id) { why_not = "These devices are on a different network."; return false; }
    return true;
}

bool may_contact(const Device &device, std::string &why_not) {
    if (device.ip.empty()) { why_not = "Its address is not known; scan the network again."; return false; }
    if (!test_network()) return true;
    bool loopback = device.ip.compare(0, 4, "127.") == 0;
    for (const char c : device.ip) loopback = loopback && (isdigit(static_cast<unsigned char>(c)) || c == '.');
    if (!loopback) why_not = "On the test network only devices at a 127.x address are contacted.";
    return loopback;
}

// ---------------------------------------------------------------- the file
bool scanned(const std::string &network_id) {
    std::lock_guard<std::mutex> lk(g_mtx);
    return load_root()["networks"].contains(network_id);
}

NetworkEntry entry(const std::string &network_id) {
    std::lock_guard<std::mutex> lk(g_mtx);
    const json root = load_root();
    if (!root["networks"].contains(network_id)) { NetworkEntry e; e.id = network_id; return e; }
    return entry_from(network_id, root["networks"][network_id]);
}

bool modify_entry(const std::string &network_id, const std::function<void(NetworkEntry &)> &change) {
    std::lock_guard<std::mutex> lk(g_mtx);
    json root = load_root();
    if (!root["networks"].contains(network_id)) return false;
    NetworkEntry e = entry_from(network_id, root["networks"][network_id]);
    change(e);
    store_entry(root, e);
    save_root(root);
    return true;
}

int remember_scan(const discovery::Network &net, const std::vector<discovery::Device> &found) {
    std::lock_guard<std::mutex> lk(g_mtx);
    json root = load_root();
    NetworkEntry e;
    e.id = net.id();
    if (root["networks"].contains(e.id)) e = entry_from(e.id, root["networks"][e.id]);
    const time_t now = time(nullptr);
    if (!e.allowed_at) e.allowed_at = now;
    e.label = net.label();
    e.gateway_mac = net.gateway_mac;
    e.last_scan = now;
    int added = 0;
    for (const auto &f : found) {
        Saved *known = nullptr;
        bool twin = false;   // the same device (its MAC) is already saved under another id
        const std::string mac = mac_key(f.mac);
        for (auto &d : e.devices) {
            if (d.id == f.id) known = &d;
            else twin = twin || (!mac.empty() && mac_key(d.mac) == mac);
        }
        if (!known) {
            e.devices.emplace_back();
            known = &e.devices.back();
            known->id = f.id;
            ++added;
            // Found another way this time (a Kasa plug that was saved as a HomeKit accessory): the
            // new record starts unused by the assistant, so that nothing decided about the first
            // (the assistant may not use it, ask before switching on) is bypassed through it. Its
            // things are still read, and the user can switch it on in settings.
            if (twin) known->enabled = false;
        }
        // Settings, pairing and things read from the device survive; what discovery reports is refreshed.
        known->kind = f.kind; known->model = f.model;
        if (known->things.empty() || known->name.empty()) known->name = f.name;
        known->ip = f.ip; known->port = f.port; known->info = f.info; known->found_by = f.found_by;
        if (!f.mac.empty()) known->mac = f.mac;
        known->last_seen = now;
    }
    store_entry(root, e);
    save_root(root);
    return added;
}

void set_enabled(const std::string &network_id, const std::string &device_id, bool on) {
    modify_entry(network_id, [&](NetworkEntry &e) {
        for (auto &d : e.devices)
            if (d.id == device_id) d.enabled = on;
    });
}

void remove_device(const std::string &network_id, const std::string &device_id) {
    modify_entry(network_id, [&](NetworkEntry &e) {
        e.devices.erase(std::remove_if(e.devices.begin(), e.devices.end(), [&](const Saved &d) { return d.id == device_id; }), e.devices.end());
    });
}

void forget_network(const std::string &network_id) {
    std::lock_guard<std::mutex> lk(g_mtx);
    json root = load_root();
    root["networks"].erase(network_id);
    save_root(root);
}

void set_thing_alias(const std::string &network_id, const std::string &device_id, const std::string &thing_id, const std::string &alias) {
    edit_thing(network_id, device_id, thing_id, [&](Thing &t, ThingMeta &) {
        const bool sounded_hot = suggests_heat(spoken(t));
        t.alias = clean_name(alias);
        // The name typed here can be the first that says what a plug feeds (a WiZ plug has none of
        // its own): one that newly sounds like a heater asks first, as a maker's name does when the
        // thing is first seen. A name that already did keeps whatever the user chose.
        if ((t.type == "plug" || t.type == "switch") && !sounded_hot && suggests_heat(spoken(t))) t.ask_first = true;
    });
}

void set_thing_room(const std::string &network_id, const std::string &device_id, const std::string &thing_id, const std::string &room) {
    // An empty room typed by the user also stays: it means "in no room", not "guess again".
    edit_thing(network_id, device_id, thing_id, [&](Thing &t, ThingMeta &m) { t.room = clean_name(room); m.room_by_user = true; });
}

void set_thing_enabled(const std::string &network_id, const std::string &device_id, const std::string &thing_id, bool on) {
    edit_thing(network_id, device_id, thing_id, [&](Thing &t, ThingMeta &) { t.enabled = on; });
}

void set_thing_ask_first(const std::string &network_id, const std::string &device_id, const std::string &thing_id, bool on) {
    edit_thing(network_id, device_id, thing_id, [&](Thing &t, ThingMeta &) { t.ask_first = on; });
}

std::vector<std::string> rooms(const std::string &network_id) {
    std::set<std::string> seen;
    std::vector<std::string> out;
    for (const auto &d : entry(network_id).devices)
        for (const auto &t : d.things)
            if (!t.room.empty() && d.listed(t) && seen.insert(plain(t.room)).second) out.push_back(t.room);
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------- merging what a driver read
void merge_things(Saved &device, std::vector<Thing> fresh, time_t now) {
    std::vector<Thing> merged;
    std::set<std::string> listed;
    for (auto &f : fresh) {
        // Only what the app knows how to offer becomes a thing, whatever a driver hands over.
        if (f.id.empty() || f.id.size() > 128 || !allowed_type(f.type) || !listed.insert(f.id).second) continue;
        f.name = clean_name(f.name);
        f.room = clean_name(f.room);
        f.caps &= kKnownCaps;
        if (f.name.empty()) f.name = clean_name(device.name);
        if (!f.ref.is_object()) f.ref = json::object();
        if (!f.state.is_object()) f.state = json::object();
        const Thing *old = device.thing(f.id);
        ThingMeta &meta = device.meta[f.id];
        if (old) {
            // What the user decided stays; what the device reports is taken afresh.
            f.alias = old->alias;
            f.enabled = old->enabled;
            f.ask_first = old->ask_first;
            // Renamed in its maker's app into a heater (a relay first called "Shelly Plus1PM", now
            // "Towel heater"), or now reported as a plug or switch: it asks first, as it would have
            // if it had been met that way. The user can switch that off again.
            if (sounds_like_heater(f) && !sounds_like_heater(*old)) f.ask_first = true;
            if (meta.room_by_user) f.room = old->room;
            else if (f.room.empty()) f.room = infer_room(f.name);
        } else {
            f.alias.clear();
            f.enabled = true;
            f.ask_first = false;
            first_seen_defaults(f);
        }
        meta.gone = 0;
        merged.push_back(std::move(f));
    }
    // A thing the device no longer lists keeps its settings for a while, in case it only dropped
    // off its bridge for a moment; it is not offered meanwhile.
    for (const auto &old : device.things) {
        if (listed.count(old.id)) continue;
        ThingMeta &meta = device.meta[old.id];
        if (!meta.gone) meta.gone = now;
        if (now - meta.gone > kKeepGoneFor) { device.meta.erase(old.id); continue; }
        merged.push_back(old);
    }
    // Settings of things that are gone for good go with them.
    for (auto it = device.meta.begin(); it != device.meta.end();) {
        const bool kept = std::any_of(merged.begin(), merged.end(), [&](const Thing &t) { return t.id == it->first; });
        it = kept ? std::next(it) : device.meta.erase(it);
    }
    device.things = std::move(merged);
}

// ---------------------------------------------------------------- for the device-free tests
namespace testing {

void set_network(const discovery::Network *network) {
    std::lock_guard<std::mutex> lk(g_net_mtx);
    g_test_network = network;
}

void set_network_source(const NetworkSource *source) {
    std::lock_guard<std::mutex> lk(g_net_mtx);
    g_test_source = source;
}

void set_clock(unsigned long (*clock_ms)()) { g_test_clock = clock_ms; }

void clear_drivers() {
    g_skip_builtin = true;
    ensure_builtin();
    std::lock_guard<std::mutex> lk(g_drivers_mtx);
    g_driver_names.clear();
    g_drivers.clear();
}

}  // namespace testing

}  // namespace home
