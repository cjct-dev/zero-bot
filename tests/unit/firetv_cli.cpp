// Command-line front end to the Fire TV driver, for testing against mock_firetv.py.
//
//   firetv_cli STATE.json COMMAND [ARG] [--deadline MS] [--wake-port N] [--again-after MS]
//              [--then CHANGE] [--first-device OTHER.json]
//
// STATE.json holds {"device": {id, kind, name, model, mac, ip, port, info, auth}, "things": [...]}.
// It stands in for the app's saved-devices file: pair, unpair, enumerate and read write it back
// (mode 0600, because it holds the pairing token), apply reads the thing from it.
//
// Commands:
//   claims              {"claims": true|false} for the device's kind
//   step                the next pairing step: {"kind", "prompt", "wait_s", "field"}
//   pair [CODE]         pairing step 1 without a code, step 2 with one
//   unpair
//   enumerate           {"things": [{id, name, type, caps: [...], state, ref}]}
//   read                the same, after refreshing the state
//   apply CHANGE        CHANGE is JSON with the fields of home::Change, e.g. '{"key":"home"}'
// --deadline MS sets apply's deadline (default 2500). --wake-port N sends Wake-on-LAN packets to
// port N instead of 9 (a mock's port; for a TV on 127.0.0.1 they go only to that address).
// --again-after MS (apply only) applies the change, prints {"first": {"ok", "fault", "error"}}
// as a line on stdout, waits MS and applies it again with the same driver object, as the app
// would (the driver remembers unanswered wake packets per device between calls); the second
// result is printed as usual. --then CHANGE makes the second call apply CHANGE instead (for
// example on=false after on=true), and --first-device OTHER.json makes the first call go to the
// device saved in OTHER.json instead (two TVs, one driver object); either one alone also asks
// for the two calls, with no wait unless --again-after gives one. OTHER.json is only read.
//
// Prints one line of JSON on stdout, or "error[<fault>]: <message>" on stderr. Exit status 0 on
// success, 1 on failure, 2 for a usage error. Nothing secret is printed: the token stays in the file.
#include "smarthome/drivers/firetv.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <curl/curl.h>

using firetv::json;

static int usage() {
    fprintf(stderr, "usage: firetv_cli STATE.json <claims|step|pair [CODE]|unpair|enumerate|read|apply CHANGE_JSON> "
                    "[--deadline MS] [--wake-port N] [--again-after MS] [--then CHANGE_JSON] [--first-device OTHER.json]\n");
    return 2;
}

static std::string str(const json &j, const char *key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : "";
}

static home::Thing thing_from(const json &j) {
    home::Thing t;
    t.id = str(j, "id");
    t.name = str(j, "name");
    t.alias = str(j, "alias");
    t.room = str(j, "room");
    t.type = str(j, "type");
    t.caps = j.value("caps", 0u);
    t.enabled = j.value("enabled", true);
    t.ask_first = j.value("ask_first", false);
    t.ref = j.value("ref", json::object());
    t.state = j.value("state", json::object());
    return t;
}

static json caps_names(uint32_t caps) {
    static const struct { uint32_t bit; const char *name; } names[] = {
        {home::kPower, "power"},         {home::kBrightness, "brightness"}, {home::kColorTemp, "color_temp"},
        {home::kColor, "color"},         {home::kScene, "scene"},           {home::kTargetTemp, "target_temp"},
        {home::kPosition, "position"},   {home::kFanSpeed, "fan_speed"},    {home::kTransport, "transport"},
        {home::kVolume, "volume"},       {home::kVolumeStep, "volume_step"}, {home::kSource, "source"},
        {home::kKeys, "keys"},           {home::kSensor, "sensor"}};
    json out = json::array();
    for (const auto &n : names)
        if (caps & n.bit) out.push_back(n.name);
    return out;
}

static json thing_json(const home::Thing &t) {
    return {{"id", t.id}, {"name", t.name},   {"alias", t.alias}, {"room", t.room}, {"type", t.type},
            {"caps", t.caps}, {"cap_names", caps_names(t.caps)}, {"enabled", t.enabled},
            {"ask_first", t.ask_first}, {"ref", t.ref}, {"state", t.state}};
}

static const char *pairing_name(home::Pairing p) {
    switch (p) {
    case home::Pairing::None: return "none";
    case home::Pairing::PressButton: return "press_button";
    case home::Pairing::EnterCode: return "enter_code";
    case home::Pairing::AcceptPrompt: return "accept_prompt";
    case home::Pairing::Password: return "password";
    case home::Pairing::Token: return "token";
    }
    return "?";
}

// The family's secrets, kept in the state file beside the device, as the app's store keeps them.
struct FileSecrets : home::Secrets {
    json all = json::object();
    json get(const std::string &family) const override { return all.contains(family) ? all[family] : json::object(); }
    void set(const std::string &family, const json &value) override { all[family] = value; }
};
static FileSecrets g_secrets;

static bool load(const std::string &path, home::Device &d, std::vector<home::Thing> &things, std::string &err) {
    std::ifstream f(path);
    if (!f) {
        err = "cannot read " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    const json j = json::parse(ss.str(), nullptr, false);
    if (!j.is_object() || !j.contains("device") || !j["device"].is_object()) {
        err = path + " is not a state file";
        return false;
    }
    const json &dj = j["device"];
    d.id = str(dj, "id");
    d.kind = str(dj, "kind");
    d.name = str(dj, "name");
    d.model = str(dj, "model");
    d.mac = str(dj, "mac");
    d.ip = str(dj, "ip");
    d.port = dj.value("port", 0);
    if (dj.contains("info") && dj["info"].is_object())
        for (auto it = dj["info"].begin(); it != dj["info"].end(); ++it)
            if (it->is_string()) d.info[it.key()] = it->get<std::string>();
    d.auth = dj.value("auth", json::object());
    if (j.contains("things") && j["things"].is_array())
        for (const json &t : j["things"]) things.push_back(thing_from(t));
    d.things = things;
    if (j.contains("secrets") && j["secrets"].is_object()) g_secrets.all = j["secrets"];
    return true;
}

static bool save(const std::string &path, const home::Device &d, const std::vector<home::Thing> &things) {
    json info = json::object();
    for (const auto &kv : d.info) info[kv.first] = kv.second;
    json list = json::array();
    for (const home::Thing &t : things) list.push_back(thing_json(t));
    const json j = {{"device",
                     {{"id", d.id}, {"kind", d.kind}, {"name", d.name}, {"model", d.model}, {"mac", d.mac},
                      {"ip", d.ip}, {"port", d.port}, {"info", info}, {"auth", d.auth}}},
                    {"things", list},
                    {"secrets", g_secrets.all}};
    const std::string text = j.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
    const std::string tmp = path + ".tmp";
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const bool ok = write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    close(fd);
    return ok && rename(tmp.c_str(), path.c_str()) == 0;
}

static home::Change change_from(const json &j, std::string &err) {
    home::Change c;
    if (!j.is_object()) {
        err = "the change is not a JSON object";
        return c;
    }
    try {
        if (j.contains("on")) c.on = j["on"].get<bool>();
        if (j.contains("brightness")) c.brightness = j["brightness"].get<int>();
        if (j.contains("kelvin")) c.kelvin = j["kelvin"].get<int>();
        if (j.contains("hs")) c.hs = std::make_pair(j["hs"].at(0).get<int>(), j["hs"].at(1).get<int>());
        if (j.contains("target_c")) c.target_c = j["target_c"].get<double>();
        if (j.contains("mode")) c.mode = j["mode"].get<std::string>();
        if (j.contains("position")) c.position = j["position"].get<int>();
        if (j.contains("fan")) c.fan = j["fan"].get<int>();
        if (j.contains("volume")) c.volume = j["volume"].get<int>();
        if (j.contains("volume_step")) c.volume_step = j["volume_step"].get<int>();
        if (j.contains("mute")) c.mute = j["mute"].get<bool>();
        if (j.contains("transport")) c.transport = j["transport"].get<std::string>();
        if (j.contains("source")) c.source = j["source"].get<std::string>();
        if (j.contains("key")) c.key = j["key"].get<std::string>();
    } catch (const std::exception &) {
        err = "the change has a field of the wrong type";
    }
    return c;
}

static int fail(const std::string &err) {
    fprintf(stderr, "error[%s]: %s\n", firetv::fault_name(firetv::fault_of(err)), err.c_str());
    return 1;
}

int main(int argc, char **argv) {
    std::vector<std::string> args;
    int deadline = 2500, again_after = -1;
    std::string then_change, first_device;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if ((a == "--deadline" || a == "--wake-port" || a == "--again-after" || a == "--then" || a == "--first-device") &&
            i + 1 >= argc)
            return usage();
        if (a == "--deadline") deadline = atoi(argv[++i]);
        else if (a == "--wake-port") firetv::detail::set_wake_port(atoi(argv[++i]));
        else if (a == "--again-after") again_after = atoi(argv[++i]);
        else if (a == "--then") then_change = argv[++i];
        else if (a == "--first-device") first_device = argv[++i];
        else args.push_back(a);
    }
    if ((!then_change.empty() || !first_device.empty()) && again_after < 0) again_after = 0;
    if (args.size() < 2) return usage();
    const std::string &path = args[0], &cmd = args[1];
    const bool takes_arg = cmd == "apply";
    const bool may_take_arg = cmd == "pair";
    if (args.size() > 3 || (takes_arg && args.size() != 3) || (!takes_arg && !may_take_arg && args.size() != 2)) return usage();

    home::Device dev;
    std::vector<home::Thing> things;
    std::string err;
    if (!load(path, dev, things, err)) {
        fprintf(stderr, "error[usage]: %s\n", err.c_str());
        return 2;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    auto driver = make_firetv_driver();
    driver->attach(&g_secrets);
    int rc = 0;
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed = [&] {
        return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
    };

    if (cmd == "claims") {
        printf("%s\n", json{{"claims", driver->claims(dev.kind, dev.info)}}.dump().c_str());
    } else if (cmd == "step") {
        const home::PairStep s = driver->next_step(dev);
        printf("%s\n", json{{"kind", pairing_name(s.kind)}, {"prompt", s.prompt}, {"wait_s", s.wait_s}, {"field", s.field}}.dump().c_str());
    } else if (cmd == "pair") {
        const bool ok = driver->pair(dev, args.size() == 3 ? args[2] : "", err);
        save(path, dev, things);   // a failed attempt changes what is stored too (the code is used up)
        if (ok) printf("%s\n", json{{"ok", true}, {"ms", elapsed()}}.dump().c_str());
        else rc = fail(err);
    } else if (cmd == "unpair") {
        driver->unpair(dev);
        save(path, dev, things);
        printf("{\"ok\":true}\n");
    } else if (cmd == "enumerate" || cmd == "read") {
        if (!driver->claims(dev.kind, dev.info)) {
            rc = fail("this device is not a Fire TV (kind \"" + dev.kind + "\")");
        } else {
            std::vector<home::Thing> out = cmd == "read" ? things : std::vector<home::Thing>();
            const bool ok = cmd == "enumerate" ? driver->enumerate(dev, out, err) : driver->read(dev, out, err);
            if (ok) {
                things = out;
                save(path, dev, things);
                json list = json::array();
                for (const home::Thing &t : out) list.push_back(thing_json(t));
                printf("%s\n", json{{"things", list}, {"ms", elapsed()}}.dump(-1, ' ', false, json::error_handler_t::replace).c_str());
            } else {
                save(path, dev, things);
                rc = fail(err);
            }
        }
    } else if (cmd == "apply") {
        const home::Change change = change_from(json::parse(args[2], nullptr, false), err);
        home::Change second = change;
        if (err.empty() && !then_change.empty()) second = change_from(json::parse(then_change, nullptr, false), err);
        home::Device other;
        std::vector<home::Thing> other_things;
        if (err.empty() && !first_device.empty()) load(first_device, other, other_things, err);
        if (!err.empty()) {
            fprintf(stderr, "error[usage]: %s\n", err.c_str());
            rc = 2;
        } else {
            const auto tv_of = [](const std::vector<home::Thing> &list) {
                home::Thing thing;
                thing.id = "tv";
                for (const home::Thing &t : list)
                    if (t.id == "tv") thing = t;
                return thing;
            };
            const home::Thing thing = tv_of(things);
            if (again_after >= 0) {
                std::string first_err;
                const bool first_ok = first_device.empty()
                                          ? driver->apply(dev, thing, change, deadline, first_err)
                                          : driver->apply(other, tv_of(other_things), change, deadline, first_err);
                const json first = {{"ok", first_ok}, {"fault", firetv::fault_name(firetv::fault_of(first_err))}, {"error", first_err}};
                printf("%s\n", json{{"first", first}}.dump().c_str());
                fflush(stdout);
                std::this_thread::sleep_for(std::chrono::milliseconds(again_after));
            }
            if (driver->apply(dev, thing, second, deadline, err)) printf("%s\n", json{{"ok", true}, {"ms", elapsed()}}.dump().c_str());
            else rc = fail(err);
            save(path, dev, things);   // the secrets may hold a key the TV changed to
        }
    } else {
        rc = usage();
    }
    fflush(stdout);
    curl_global_cleanup();
    return rc;
}
