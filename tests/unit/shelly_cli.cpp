// Command-line front end to the Shelly driver, for testing against mock_shelly.py.
//
//   shelly_cli STATE.json COMMAND [ARG...] [--deadline MS]
//
// STATE.json holds {"device": {id, kind, name, model, mac, ip, port, info, auth}, "things": [...],
// "secrets": {...}}. It stands in for the app's saved-devices file (mode 0600: it holds the
// password): pair, unpair, enumerate, read and apply write it back, so that a second run of this
// program is "a later run" of the app.
//
// Commands:
//   claims              {"claims": true|false} for the device's kind
//   step                the next pairing step: {"kind", "prompt", "wait_s", "field"}
//   pair TEXT           what the user typed for the step
//   unpair
//   enumerate           {"things": [{id, name, type, caps, cap_names, state, ref}], "device": {name, model}}
//   read [THING_ID]     the state of all things, or of one
//   apply THING_ID CHANGE   CHANGE is JSON with the fields of home::Change, e.g. '{"on":true}', '{"hs":[120,100]}'
// --deadline MS sets apply's deadline (default 2500).
//
// Prints one line of JSON on stdout, or "error[<kind>]: <message>" on stderr, where kind is
// no_answer, needs_password, refused, throttled, other_device, not_understood or other. Exit
// status 0 on success, 1 on failure, 2 for a usage error. The password is never printed.
#include "smarthome/drivers/shelly.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

#include <curl/curl.h>

using shelly::json;

static int usage() {
    fprintf(stderr, "usage: shelly_cli STATE.json <claims|step|pair TEXT|unpair|enumerate|read [THING]|apply THING CHANGE_JSON> "
                    "[--deadline MS]\n");
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
    t.type = str(j, "type");
    t.caps = j.value("caps", 0u);
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
    return {{"id", t.id},     {"name", t.name},   {"type", t.type}, {"caps", t.caps},
            {"cap_names", caps_names(t.caps)}, {"ref", t.ref}, {"state", t.state}};
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

static const char *kind_of(const std::string &err) {
    auto starts = [&](const char *p) { return err.compare(0, strlen(p), p) == 0; };
    if (starts(shelly::kNoAnswer)) return "no_answer";
    if (starts(shelly::kNeedsPassword)) return "needs_password";
    if (starts(shelly::kRefused)) return "refused";
    if (starts(shelly::kThrottled)) return "throttled";
    if (starts(shelly::kOtherDevice)) return "other_device";
    if (starts(shelly::kNotUnderstood)) return "not_understood";
    return "other";
}

struct FileSecrets : home::Secrets {
    json all = json::object();
    json get(const std::string &family) const override { return all.contains(family) ? all[family] : json::object(); }
    void set(const std::string &family, const json &value) override {
        if (value.is_object() && !value.empty()) all[family] = value;
        else all.erase(family);
    }
};
static FileSecrets g_secrets;

static bool load(const std::string &path, home::Device &d, std::string &err) {
    std::ifstream f(path);
    if (!f) { err = "cannot read " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    const json j = json::parse(ss.str(), nullptr, false);
    if (!j.is_object() || !j.contains("device") || !j["device"].is_object()) { err = path + " is not a state file"; return false; }
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
        for (const json &t : j["things"]) d.things.push_back(thing_from(t));
    if (j.contains("secrets") && j["secrets"].is_object()) g_secrets.all = j["secrets"];
    return true;
}

static bool save(const std::string &path, const home::Device &d) {
    json info = json::object();
    for (const auto &kv : d.info) info[kv.first] = kv.second;
    json list = json::array();
    for (const home::Thing &t : d.things) list.push_back(thing_json(t));
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

static bool change_from(const json &j, home::Change &c, std::string &err) {
    if (!j.is_object()) { err = "the change is not a JSON object"; return false; }
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
        return false;
    }
    return true;
}

static int fail(const std::string &err) {
    fprintf(stderr, "error[%s]: %s\n", kind_of(err), err.c_str());
    return 1;
}

int main(int argc, char **argv) {
    std::vector<std::string> args;
    int deadline = 2500;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--deadline") {
            if (i + 1 >= argc) return usage();
            deadline = atoi(argv[++i]);
        } else {
            args.push_back(a);
        }
    }
    if (args.size() < 2) return usage();
    const std::string &path = args[0], &cmd = args[1];
    home::Device dev;
    std::string err;
    if (!load(path, dev, err)) {
        fprintf(stderr, "error[usage]: %s\n", err.c_str());
        return 2;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    auto driver = make_shelly_driver();
    driver->attach(&g_secrets);
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed = [&] {
        return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
    };
    int rc = 0;
    if (cmd == "claims" && args.size() == 2) {
        printf("%s\n", json{{"claims", driver->claims(dev.kind, dev.info)}}.dump().c_str());
    } else if (cmd == "step" && args.size() == 2) {
        const home::PairStep s = driver->next_step(dev);
        printf("%s\n", json{{"kind", pairing_name(s.kind)}, {"prompt", s.prompt}, {"wait_s", s.wait_s}, {"field", s.field}}.dump().c_str());
    } else if (cmd == "pair" && args.size() == 3) {
        const bool ok = driver->pair(dev, args[2], err);
        save(path, dev);
        if (ok) printf("%s\n", json{{"ok", true}, {"ms", elapsed()}}.dump().c_str());
        else rc = fail(err);
    } else if (cmd == "unpair" && args.size() == 2) {
        driver->unpair(dev);
        save(path, dev);
        printf("{\"ok\":true}\n");
    } else if (cmd == "enumerate" && args.size() == 2) {
        std::vector<home::Thing> out;
        const bool ok = driver->enumerate(dev, out, err);
        if (ok) dev.things = out;
        save(path, dev);
        if (ok) {
            json list = json::array();
            for (const home::Thing &t : out) list.push_back(thing_json(t));
            printf("%s\n", json{{"things", list}, {"device", {{"name", dev.name}, {"model", dev.model}}}, {"ms", elapsed()}}
                               .dump(-1, ' ', false, json::error_handler_t::replace).c_str());
        } else {
            rc = fail(err);
        }
    } else if (cmd == "read" && (args.size() == 2 || args.size() == 3)) {
        std::vector<home::Thing> things;
        if (args.size() == 3)
            for (const auto &t : dev.things)
                if (t.id == args[2]) things.push_back(t);
        if (args.size() == 3 && things.empty()) {
            fprintf(stderr, "error[usage]: no thing %s\n", args[2].c_str());
            rc = 2;
        } else if (driver->read(dev, things, err)) {
            json list = json::array();
            for (const home::Thing &t : things) list.push_back(thing_json(t));
            save(path, dev);
            printf("%s\n", json{{"things", list}, {"ms", elapsed()}}.dump(-1, ' ', false, json::error_handler_t::replace).c_str());
        } else {
            save(path, dev);
            rc = fail(err);
        }
    } else if (cmd == "apply" && args.size() == 4) {
        home::Change change;
        home::Thing thing;
        bool found = false;
        for (const auto &t : dev.things)
            if (t.id == args[2]) { thing = t; found = true; }
        if (!found || !change_from(json::parse(args[3], nullptr, false), change, err)) {
            fprintf(stderr, "error[usage]: %s\n", found ? err.c_str() : ("no thing " + args[2]).c_str());
            rc = 2;
        } else {
            const bool ok = driver->apply(dev, thing, change, deadline, err);
            save(path, dev);
            if (ok) printf("%s\n", json{{"ok", true}, {"ms", elapsed()}}.dump().c_str());
            else rc = fail(err);
        }
    } else {
        rc = usage();
    }
    fflush(stdout);
    curl_global_cleanup();
    return rc;
}
