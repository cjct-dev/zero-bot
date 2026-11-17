#include "smarthome/home.h"

#include <algorithm>
#include <mutex>
#include <set>
#include <sstream>

#include "platform/display.h"
#include "smarthome/tapo.h"

namespace home {

namespace {

std::mutex g_mtx;            // guards the file and the cached network
std::mutex g_device_mtx;     // one conversation with a device at a time

std::string path() { return util::data_dir() + "/smarthome.json"; }

json load() {
    json j = util::load_json(path(), json::object());
    if (!j.is_object()) j = json::object();
    if (!j.contains("networks") || !j["networks"].is_object()) j["networks"] = json::object();
    return j;
}

json to_json(const Saved &d) {
    json sw = json::array();
    for (const auto &s : d.switches) sw.push_back({{"name", s.name}, {"child", s.child}, {"on", s.on}});
    return {{"id", d.id}, {"kind", d.kind}, {"name", d.name}, {"model", d.model}, {"mac", d.mac}, {"last_ip", d.ip},
            {"port", d.port}, {"info", d.info}, {"found_by", d.found_by}, {"enabled", d.enabled},
            {"last_seen", static_cast<long long>(d.last_seen)}, {"switches", sw}, {"problem", d.problem}};
}

Saved from_json(const json &j) {
    using util::jget;
    Saved d;
    d.id = jget(j, "id", std::string()); d.kind = jget(j, "kind", std::string());
    d.name = jget(j, "name", std::string()); d.model = jget(j, "model", std::string());
    d.mac = jget(j, "mac", std::string()); d.ip = jget(j, "last_ip", std::string());
    d.port = jget(j, "port", 0); d.enabled = jget(j, "enabled", true);
    d.last_seen = jget<long long>(j, "last_seen", 0); d.problem = jget(j, "problem", std::string());
    if (j.contains("info") && j["info"].is_object())
        for (auto it = j["info"].begin(); it != j["info"].end(); ++it)
            if (it.value().is_string()) d.info[it.key()] = it.value().get<std::string>();
    if (j.contains("found_by") && j["found_by"].is_array())
        for (const auto &m : j["found_by"])
            if (m.is_string()) d.found_by.push_back(m.get<std::string>());
    if (j.contains("switches") && j["switches"].is_array())
        for (const auto &s : j["switches"])
            d.switches.push_back({jget(s, "name", std::string()), jget(s, "child", std::string()), jget(s, "on", false)});
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
        for (const auto &d : n["devices"]) e.devices.push_back(from_json(d));
    return e;
}

void store_entry(json &root, const NetworkEntry &e) {
    json devices = json::array();
    for (const auto &d : e.devices) devices.push_back(to_json(d));
    root["networks"][e.id] = {{"label", e.label}, {"gateway_mac", e.gateway_mac},
                              {"allowed_at", static_cast<long long>(e.allowed_at)},
                              {"last_scan", static_cast<long long>(e.last_scan)}, {"devices", devices}};
}

// Letters and digits only, lower case: "Bed-heater" and "bed heater" are the same thing said aloud.
std::string plain(const std::string &text) {
    std::string out;
    for (const char c : util::lower(text))
        if (isalnum(static_cast<unsigned char>(c)) || (c & 0x80)) out += c;
        else if (!out.empty() && out.back() != ' ') out += ' ';
    return util::trim(out);
}

tapo::Device tapo_device(const Saved &d) {
    tapo::Device t;
    t.ip = d.ip;
    t.port = d.port ? d.port : 80;
    if (const auto it = d.info.find("scheme"); it != d.info.end()) t.scheme = it->second;
    if (const auto it = d.info.find("tpap"); it != d.info.end()) t.tpap = json::parse(it->second, nullptr, false);
    if (const auto it = d.info.find("owner"); it != d.info.end()) t.owner = it->second;
    return t;
}

tapo::Credentials credentials(const TapoAccount &a) {
    tapo::Credentials c;
    c.email = a.email;
    c.password_sha1_hex = a.password_sha1;
    return c;
}

struct Target {
    Saved device;
    Switch sw;
};

// Enabled switches on the current network, with the device each belongs to.
std::vector<Target> targets(const std::string &network_id) {
    std::vector<Target> out;
    for (const auto &d : entry(network_id).devices) {
        if (!d.enabled || !d.controllable()) continue;
        for (const auto &s : d.switches)
            if (!s.name.empty()) out.push_back({d, s});
    }
    return out;
}

// Finds what the user meant. Exact name first, then a name that contains (or is contained in)
// what was said; more than one candidate is reported rather than guessed.
bool pick(const std::vector<Target> &all, const std::string &said, Target &out, std::string &err) {
    const std::string want = plain(said);
    std::vector<const Target *> hits;
    for (const auto &t : all)
        if (plain(t.sw.name) == want) hits.push_back(&t);
    if (hits.empty())
        for (const auto &t : all) {
            const std::string name = plain(t.sw.name);
            if (!want.empty() && (name.find(want) != std::string::npos || want.find(name) != std::string::npos)) hits.push_back(&t);
        }
    if (hits.size() == 1) { out = *hits[0]; return true; }
    std::string names;
    for (const auto &t : hits.empty() ? std::vector<const Target *>() : hits) names += (names.empty() ? "" : ", ") + t->sw.name;
    if (hits.empty()) {
        for (const auto &t : all) names += (names.empty() ? "" : ", ") + t.sw.name;
        err = "there is no device called '" + said + "'. The devices are: " + names;
    } else {
        err = "'" + said + "' could mean any of: " + names + ". Ask which one";
    }
    return false;
}

void save_switch_state(const std::string &network_id, const std::string &device_id, const std::string &child, bool on) {
    std::lock_guard<std::mutex> lk(g_mtx);
    json root = load();
    if (!root["networks"].contains(network_id)) return;
    NetworkEntry e = entry_from(network_id, root["networks"][network_id]);
    for (auto &d : e.devices)
        if (d.id == device_id)
            for (auto &s : d.switches)
                if (s.child == child) s.on = on;
    store_entry(root, e);
    util::save_json(path(), root);
}

// A plug whose address has changed since it was saved is looked for again by asking the network,
// and recognised by its identifier, never by going through addresses.
bool relocate(const std::string &network_id, Saved &device) {
    const discovery::Network net = network_now(true);
    if (!net.usable || net.id() != network_id) return false;
    std::vector<discovery::Device> found;
    std::string err;
    discovery::Options opt;
    opt.listen_ms = 2500;
    if (!discovery::scan(opt, found, err)) return false;
    for (const auto &f : found)
        if (f.id == device.id && f.ip != device.ip) {
            remember_scan(net, found);
            device.ip = f.ip;
            return true;
        }
    return false;
}

}  // namespace

bool Saved::controllable() const { return kind == "tapo"; }

std::string Saved::title() const {
    if (switches.size() == 1 && !switches[0].name.empty()) return switches[0].name;
    return name;
}

discovery::Network network_now(bool fresh) {
    static discovery::Network cached;
    static unsigned long at = 0;
    std::lock_guard<std::mutex> lk(g_mtx);
    const unsigned long now = display::now_ms();
    if (fresh || at == 0 || now - at > 8000) { cached = discovery::current_network(); at = now ? now : 1; }
    return cached;
}

bool scanned(const std::string &network_id) {
    std::lock_guard<std::mutex> lk(g_mtx);
    return load()["networks"].contains(network_id);
}

NetworkEntry entry(const std::string &network_id) {
    std::lock_guard<std::mutex> lk(g_mtx);
    const json root = load();
    if (!root["networks"].contains(network_id)) { NetworkEntry e; e.id = network_id; return e; }
    return entry_from(network_id, root["networks"][network_id]);
}

int remember_scan(const discovery::Network &net, const std::vector<discovery::Device> &found) {
    std::lock_guard<std::mutex> lk(g_mtx);
    json root = load();
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
        for (auto &d : e.devices)
            if (d.id == f.id) known = &d;
        if (!known) { e.devices.emplace_back(); known = &e.devices.back(); known->id = f.id; ++added; }
        // Settings and names read from the device survive; what discovery reports is refreshed.
        known->kind = f.kind; known->name = f.name; known->model = f.model;
        known->ip = f.ip; known->port = f.port; known->info = f.info; known->found_by = f.found_by;
        if (!f.mac.empty()) known->mac = f.mac;
        known->last_seen = now;
    }
    store_entry(root, e);
    util::save_json(path(), root);
    return added;
}

void set_enabled(const std::string &network_id, const std::string &device_id, bool on) {
    std::lock_guard<std::mutex> lk(g_mtx);
    json root = load();
    if (!root["networks"].contains(network_id)) return;
    NetworkEntry e = entry_from(network_id, root["networks"][network_id]);
    for (auto &d : e.devices)
        if (d.id == device_id) d.enabled = on;
    store_entry(root, e);
    util::save_json(path(), root);
}

void remove_device(const std::string &network_id, const std::string &device_id) {
    std::lock_guard<std::mutex> lk(g_mtx);
    json root = load();
    if (!root["networks"].contains(network_id)) return;
    NetworkEntry e = entry_from(network_id, root["networks"][network_id]);
    e.devices.erase(std::remove_if(e.devices.begin(), e.devices.end(), [&](const Saved &d) { return d.id == device_id; }), e.devices.end());
    store_entry(root, e);
    util::save_json(path(), root);
}

void forget_network(const std::string &network_id) {
    std::lock_guard<std::mutex> lk(g_mtx);
    json root = load();
    root["networks"].erase(network_id);
    util::save_json(path(), root);
}

TapoAccount tapo_account() {
    std::lock_guard<std::mutex> lk(g_mtx);
    const json root = load();
    TapoAccount a;
    if (root.contains("tapo") && root["tapo"].is_object()) {
        a.email = util::jget(root["tapo"], "email", std::string());
        a.password_sha1 = util::jget(root["tapo"], "password_sha1", std::string());
    }
    return a;
}

void set_tapo_account(const std::string &email, const std::string &password) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        json root = load();
        json t = root.contains("tapo") && root["tapo"].is_object() ? root["tapo"] : json::object();
        t["email"] = util::trim(email);
        if (!password.empty()) t["password_sha1"] = tapo::sha1_hex(password);
        if (util::trim(email).empty()) t = json::object();
        root["tapo"] = t;
        util::save_json(path(), root);
    }
    tapo::forget_sessions();   // a corrected password gets a fresh attempt
}

std::string refresh(const std::string &network_id) {
    NetworkEntry e = entry(network_id);
    const TapoAccount account = tapo_account();
    int read = 0, failed = 0, plugs = 0;
    std::string first_problem;
    std::lock_guard<std::mutex> device_lock(g_device_mtx);
    for (auto &d : e.devices) {
        if (!d.controllable()) continue;
        ++plugs;
        d.problem.clear();
        const tapo::Device t = tapo_device(d);
        if (!account.set()) d.problem = "Add your Tapo account to use this.";
        else if (!tapo::owned_by(t, account.email)) d.problem = "This plug belongs to a different Tapo account.";
        else {
            std::string err;
            json info;
            const tapo::Credentials cred = credentials(account);
            if (!tapo::get_info(t, cred, info, err)) d.problem = err;
            else {
                const std::string name = util::truncate_utf8(util::trim(util::jget(info, "name", std::string())), 40);
                std::vector<Switch> switches;
                json sockets;
                std::string sock_err;
                if (util::jget(info, "sockets", 0) > 0 && tapo::list_sockets(t, cred, sockets, sock_err) && sockets.is_array()) {
                    for (const auto &s : sockets)
                        switches.push_back({util::truncate_utf8(util::trim(util::jget(s, "name", std::string())), 40),
                                            util::jget(s, "id", std::string()), util::jget(s, "on", false)});
                } else {
                    switches.push_back({name.empty() ? d.name : name, "", util::jget(info, "on", false)});
                }
                d.switches = switches;
                if (!name.empty()) d.name = name;
            }
        }
        if (d.problem.empty()) ++read;
        else { ++failed; if (first_problem.empty()) first_problem = d.problem; }
    }
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        json root = load();
        if (root["networks"].contains(network_id)) {
            // Only what was read is written back, so a scan that ran meanwhile is not undone.
            NetworkEntry now = entry_from(network_id, root["networks"][network_id]);
            for (auto &d : now.devices)
                for (const auto &r : e.devices)
                    if (r.id == d.id && r.controllable()) { d.switches = r.switches; d.problem = r.problem; d.name = r.name; }
            store_entry(root, now);
            util::save_json(path(), root);
        }
    }
    if (plugs == 0) return "Nothing here can be controlled yet.";
    if (failed == 0) return std::to_string(read) + (read == 1 ? " device ready." : " devices ready.");
    return std::to_string(read) + " ready, " + std::to_string(failed) + " not: " + first_problem;
}

std::vector<std::string> switch_names() {
    const discovery::Network net = network_now();
    std::vector<std::string> names;
    if (!net.usable) return names;
    for (const auto &t : targets(net.id())) names.push_back(t.sw.name);
    return names;
}

bool available() { return !switch_names().empty(); }

std::string switch_power(const std::string &name, bool on) {
    const discovery::Network net = network_now();
    if (!net.usable) return "ERROR: " + net.why_not;
    Target target;
    std::string err;
    if (!pick(targets(net.id()), name, target, err)) return "ERROR: " + err;
    const tapo::Credentials cred = credentials(tapo_account());
    std::lock_guard<std::mutex> device_lock(g_device_mtx);
    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
        const tapo::Device t = tapo_device(target.device);
        err.clear();
        ok = target.sw.child.empty() ? tapo::set_power(t, cred, on, err) : tapo::set_socket_power(t, cred, target.sw.child, on, err);
        // No answer at the saved address: the router may have given the plug a new one.
        if (!ok && attempt == 0 && !relocate(net.id(), target.device)) break;
    }
    if (!ok) return "ERROR: could not reach " + target.sw.name + ": " + err;
    save_switch_state(net.id(), target.device.id, target.sw.child, on);
    return json{{"device", target.sw.name}, {"now", on ? "on" : "off"}}.dump();
}

std::string switch_all_off() {
    const discovery::Network net = network_now();
    if (!net.usable) return "ERROR: " + net.why_not;
    const tapo::Credentials cred = credentials(tapo_account());
    json done = json::array(), failed = json::array();
    {
        std::lock_guard<std::mutex> device_lock(g_device_mtx);
        for (const auto &t : targets(net.id())) {
            const tapo::Device dev = tapo_device(t.device);
            std::string err;
            const bool ok = t.sw.child.empty() ? tapo::set_power(dev, cred, false, err) : tapo::set_socket_power(dev, cred, t.sw.child, false, err);
            if (ok) { done.push_back(t.sw.name); save_switch_state(net.id(), t.device.id, t.sw.child, false); }
            else failed.push_back({{"device", t.sw.name}, {"error", err}});
        }
    }
    json out = {{"switched_off", done}};
    if (!failed.empty()) out["could_not_reach"] = failed;
    return (done.empty() && !failed.empty() ? "ERROR: " : "") + out.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string status(const std::string &name) {
    const discovery::Network net = network_now();
    if (!net.usable) return "ERROR: " + net.why_not;
    std::vector<Target> all = targets(net.id());
    if (!name.empty()) {
        Target one;
        std::string err;
        if (!pick(all, name, one, err)) return "ERROR: " + err;
        all = {one};
    }
    const tapo::Credentials cred = credentials(tapo_account());
    std::lock_guard<std::mutex> device_lock(g_device_mtx);
    json out = json::array();
    std::map<std::string, json> strips;   // one read per power strip, however many sockets it has
    for (const auto &t : all) {
        const tapo::Device dev = tapo_device(t.device);
        json item = {{"device", t.sw.name}};
        std::string err;
        if (t.sw.child.empty()) {
            json info;
            if (!tapo::get_info(dev, cred, info, err)) { item["error"] = err; out.push_back(item); continue; }
            item["state"] = util::jget(info, "on", false) ? "on" : "off";
            // Energy readings only when one device is asked about: they cost a second request each.
            json energy;
            if (!name.empty() && tapo::get_energy(dev, cred, energy, err) && energy.is_object()) {
                if (energy.contains("power_w")) item["power_now_watts"] = energy["power_w"];
                if (energy.contains("today_wh")) item["energy_today_wh"] = energy["today_wh"];
            }
        } else {
            if (!strips.count(t.device.id)) {
                json sockets;
                if (!tapo::list_sockets(dev, cred, sockets, err)) { item["error"] = err; out.push_back(item); continue; }
                strips[t.device.id] = sockets;
            }
            for (const auto &s : strips[t.device.id])
                if (util::jget(s, "id", std::string()) == t.sw.child) item["state"] = util::jget(s, "on", false) ? "on" : "off";
        }
        out.push_back(item);
    }
    return json{{"devices", out}}.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string other_devices() {
    const discovery::Network net = network_now();
    if (!net.usable) return "";
    std::string out;
    for (const auto &d : entry(net.id()).devices)
        if (!d.controllable()) out += (out.empty() ? "" : ", ") + d.name + " (" + d.kind + ")";
    return out;
}

}  // namespace home
