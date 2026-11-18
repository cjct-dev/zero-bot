// Command-line front end to the Hue driver, for testing against tests/mocks/hue/mock_hue.py. It
// goes through the home::Driver interface only, keeping the device (address, discovery details,
// pairing, things) and the family's secrets in a JSON state file the way the app's store keeps
// them: auth and things are written back after pair, enumerate and unpair; the secrets after every
// command, since read and apply may record a rejected key or a changed certificate there (the
// app's store writes them at once too).
//
//   hue_cli --state FILE init <ip> <port> [bridgeid] [modelid]
//   hue_cli --state FILE step | pair | enumerate | unpair
//   hue_cli --state FILE read [thing-id ...]
//   hue_cli --state FILE apply <thing-id> '<change JSON>'
//   hue_cli --state FILE apply-many '<change JSON>' <thing-id> <thing-id> ...
//   hue_cli claims <kind> ['<info JSON>']
// Options: --repeat N (run the command N times in this one process), --deadline-ms N (apply,
// default 2500), --pair-window-ms N and --pair-interval-ms N (test hooks; default 30000 and 1000).
// A change is {"on":true,"brightness":40,"kelvin":2700,"hs":[240,100],"volume":5,...}.
// Prints one line of JSON per successful run on stdout, or "error: ..." on stderr, followed on
// stderr by "elapsed_ms: N" for every run. Exit status 0 when every run succeeded, 1 otherwise,
// 2 for a usage error. The state file holds the application key (mode 0600): it is a test artefact.
#include "smarthome/drivers/hue.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "net/http.h"

using hue::json;

static int usage() {
    fprintf(stderr,
            "usage: hue_cli --state FILE <init IP PORT [BRIDGEID] [MODELID] | step | pair | enumerate | unpair | read [ID...] |\n"
            "               apply ID CHANGE | apply-many CHANGE ID ID...> [--repeat N] [--deadline-ms N]\n"
            "               [--pair-window-ms N] [--pair-interval-ms N]\n"
            "       hue_cli claims KIND [INFO]\n");
    return 2;
}

// The family's secrets, kept in the state file beside the device, as the app's store keeps them.
struct FileSecrets : home::Secrets {
    json all = json::object();
    json get(const std::string &family) const override {
        const auto it = all.find(family);
        return it != all.end() && it->is_object() ? *it : json::object();
    }
    void set(const std::string &family, const json &value) override {
        if (value.is_object() && !value.empty()) all[family] = value;
        else all.erase(family);
    }
};
static FileSecrets g_secrets;

static json thing_json(const home::Thing &t) {
    return {{"id", t.id},     {"name", t.name}, {"room", t.room},       {"type", t.type},          {"caps", t.caps},
            {"ref", t.ref},   {"state", t.state}, {"enabled", t.enabled}, {"ask_first", t.ask_first}};
}

static home::Thing thing_from(const json &j) {
    home::Thing t;
    t.id = j.value("id", "");
    t.name = j.value("name", "");
    t.room = j.value("room", "");
    t.type = j.value("type", "");
    t.caps = j.value("caps", 0u);
    t.ref = j.contains("ref") ? j["ref"] : json::object();
    t.state = j.contains("state") ? j["state"] : json::object();
    // The user's own settings, which the app keeps beside each thing (a scene is judged against them).
    t.enabled = j.value("enabled", true);
    t.ask_first = j.value("ask_first", false);
    return t;
}

static bool load(const std::string &path, home::Device &dev) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    const json j = json::parse(ss.str(), nullptr, false);
    if (!j.is_object()) return false;
    dev.id = j.value("id", "");
    dev.kind = j.value("kind", "hue_bridge");
    dev.name = j.value("name", "");
    dev.model = j.value("model", "");
    dev.ip = j.value("ip", "");
    dev.port = j.value("port", 0);
    if (j.contains("info") && j["info"].is_object())
        for (auto it = j["info"].begin(); it != j["info"].end(); ++it)
            if (it->is_string()) dev.info[it.key()] = it->get<std::string>();
    dev.auth = j.contains("auth") && j["auth"].is_object() ? j["auth"] : json::object();
    if (j.contains("things") && j["things"].is_array())
        for (const auto &t : j["things"]) dev.things.push_back(thing_from(t));
    if (j.contains("secrets") && j["secrets"].is_object()) g_secrets.all = j["secrets"];
    return true;
}

static bool save(const std::string &path, const home::Device &dev) {
    json things = json::array();
    for (const auto &t : dev.things) things.push_back(thing_json(t));
    json info = json::object();
    for (const auto &kv : dev.info) info[kv.first] = kv.second;
    const json j = {{"id", dev.id},     {"kind", dev.kind}, {"name", dev.name}, {"model", dev.model}, {"ip", dev.ip},
                    {"port", dev.port}, {"info", info},     {"auth", dev.auth}, {"things", things},   {"secrets", g_secrets.all}};
    const std::string text = j.dump(1, ' ', false, json::error_handler_t::replace) + "\n";
    const std::string tmp = path + ".tmp";
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const bool ok = write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    close(fd);
    return ok && rename(tmp.c_str(), path.c_str()) == 0;
}

static bool parse_change(const std::string &text, home::Change &c, std::string &err) {
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) {
        err = "the change is not a JSON object";
        return false;
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
        err = "the change has a value of the wrong type";
        return false;
    }
    return true;
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

int main(int argc, char **argv) {
    std::vector<std::string> args;
    std::string state;
    int repeat = 1, deadline_ms = 2500;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        const bool takes = a == "--state" || a == "--repeat" || a == "--deadline-ms" || a == "--pair-window-ms" ||
                           a == "--pair-interval-ms";
        if (takes && i + 1 >= argc) return usage();
        if (a == "--state") state = argv[++i];
        else if (a == "--repeat") repeat = atoi(argv[++i]);
        else if (a == "--deadline-ms") deadline_ms = atoi(argv[++i]);
        else if (a == "--pair-window-ms") hue::detail::set_pair_window_ms(atoi(argv[++i]));
        else if (a == "--pair-interval-ms") hue::detail::set_pair_interval_ms(atoi(argv[++i]));
        else args.push_back(a);
    }
    if (args.empty() || repeat < 1 || repeat > 100) return usage();
    const std::string cmd = args[0];
    auto driver = make_hue_driver();
    driver->attach(&g_secrets);

    if (cmd == "claims") {
        if (args.size() < 2 || args.size() > 3) return usage();
        std::map<std::string, std::string> info;
        if (args.size() == 3) {
            const json j = json::parse(args[2], nullptr, false);
            if (!j.is_object()) return usage();
            for (auto it = j.begin(); it != j.end(); ++it)
                if (it->is_string()) info[it.key()] = it->get<std::string>();
        }
        printf("%s\n", json({{"claims", driver->claims(args[1], info)}}).dump().c_str());
        return 0;
    }
    if (state.empty()) return usage();
    if (cmd == "init") {
        if (args.size() < 3 || args.size() > 5) return usage();
        home::Device dev;
        dev.kind = "hue_bridge";
        dev.ip = args[1];
        dev.port = atoi(args[2].c_str());
        dev.name = "Hue Bridge";
        if (args.size() > 3 && !args[3].empty()) {
            dev.info["bridgeid"] = args[3];
            dev.id = "hue:" + args[3];
        } else {
            dev.id = "hue:" + dev.ip;
        }
        if (args.size() > 4) dev.info["modelid"] = args[4];
        return save(state, dev) ? 0 : 1;
    }

    home::Device dev;
    if (!load(state, dev)) {
        fprintf(stderr, "error: cannot read the state file\n");
        return 2;
    }
    home::Change change;
    std::vector<std::string> ids;
    std::string perr;
    if (cmd == "apply") {
        if (args.size() != 3 || !parse_change(args[2], change, perr)) return perr.empty() ? usage() : (fprintf(stderr, "error: %s\n", perr.c_str()), 2);
        ids.push_back(args[1]);
    } else if (cmd == "apply-many") {
        if (args.size() < 3 || !parse_change(args[1], change, perr)) return perr.empty() ? usage() : (fprintf(stderr, "error: %s\n", perr.c_str()), 2);
        ids.assign(args.begin() + 2, args.end());
    } else if (cmd == "read") {
        ids.assign(args.begin() + 1, args.end());
    } else if (cmd != "step" && cmd != "pair" && cmd != "enumerate" && cmd != "unpair") {
        return usage();
    }
    auto thing_by_id = [&](const std::string &id, home::Thing &out) {
        for (const auto &t : dev.things)
            if (t.id == id) { out = t; return true; }
        return false;
    };

    http::global_init();
    int failures = 0;
    for (int run = 0; run < repeat; run++) {
        const auto start = std::chrono::steady_clock::now();
        std::string err;
        bool ok = true;
        json out = json::object();
        if (cmd == "step") {
            const home::PairStep s = driver->next_step(dev);
            out = {{"kind", pairing_name(s.kind)}, {"prompt", s.prompt}, {"wait_s", s.wait_s}, {"field", s.field}};
        } else if (cmd == "pair") {
            ok = driver->pair(dev, "", err);
            if (ok) out = {{"paired", true}};
        } else if (cmd == "enumerate") {
            std::vector<home::Thing> things;
            ok = driver->enumerate(dev, things, err);
            if (ok) {
                dev.things = things;
                json list = json::array();
                for (const auto &t : things) list.push_back(thing_json(t));
                out = {{"name", dev.name}, {"model", dev.model}, {"things", list}};
            }
        } else if (cmd == "unpair") {
            driver->unpair(dev);
            out = {{"unpaired", true}};
        } else if (cmd == "read") {
            std::vector<home::Thing> things;
            for (const auto &id : ids) {
                home::Thing t;
                if (!thing_by_id(id, t)) {
                    fprintf(stderr, "error: no thing %s in the state file\n", id.c_str());
                    return 2;
                }
                things.push_back(t);
            }
            ok = driver->read(dev, things, err);
            if (ok) {
                json list = json::array();
                for (const auto &t : things) list.push_back({{"id", t.id}, {"state", t.state}});
                out = {{"things", list}};
            }
        } else if (cmd == "apply") {
            home::Thing t;
            if (!thing_by_id(ids[0], t)) {
                fprintf(stderr, "error: no thing %s in the state file\n", ids[0].c_str());
                return 2;
            }
            ok = driver->apply(dev, t, change, deadline_ms, err);
            if (ok) out = {{"ok", true}};
        } else {   // apply-many
            std::vector<home::Thing> things(ids.size());
            std::vector<const home::Thing *> ptrs;
            for (size_t i = 0; i < ids.size(); i++) {
                if (!thing_by_id(ids[i], things[i])) {
                    fprintf(stderr, "error: no thing %s in the state file\n", ids[i].c_str());
                    return 2;
                }
                ptrs.push_back(&things[i]);
            }
            ok = driver->apply_many(dev, ptrs, change, deadline_ms, err);
            // Not handled (false with no error) is a valid outcome: the caller then applies one by one.
            if (ok || err.empty()) {
                out = {{"ok", ok}, {"handled", ok}};
                ok = true;
            }
        }
        // Like the app: a failed pair or enumerate leaves the device as it was saved (pair stores
        // nothing on failure anyway); the secrets are always kept.
        save(state, dev);
        const long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (ok) {
            printf("%s\n", out.dump(-1, ' ', false, json::error_handler_t::replace).c_str());
        } else {
            fprintf(stderr, "error: %s\n", err.c_str());
            failures++;
        }
        fprintf(stderr, "elapsed_ms: %ld\n", ms);
        fflush(stdout);
        fflush(stderr);
    }
    return failures ? 1 : 0;
}
