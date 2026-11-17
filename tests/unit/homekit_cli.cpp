// Command-line front end to homekit.cpp, for testing against mock_homekit.py.
//
//   homekit_cli STATE COMMAND [ARGS...] [--then COMMAND [ARGS...]]... [--repeat N]
//
// STATE is a JSON file standing in for the app's 0600 store: {"secrets": {...}, "device": {...}};
// it is written with mode 0600 (it holds this controller's private key).
// Commands:
//   init IP PORT [KEY=VALUE...]     writes a fresh STATE; KEY=VALUE pairs become discovery's
//                                   Device::info (sf=1 ci=2 id=AA:BB:... c#=3); no network
//   claims [KIND]                   {"claims": bool} for the saved info (KIND default "homekit")
//   step                            the next pairing step
//   pair INPUT                      pairs with a setup code or X-HM:// text
//   unpair                          Remove Pairing
//   enumerate                       the things, which are also saved in STATE
//   read [THING_ID...]              fresh state of those things (all saved ones when none given)
//   apply THING_ID KEY=VALUE...     on=true brightness=40 kelvin=2700 hs=120,50 target=20.5
//                                   mode=heat position=30 fan=50 volume=10 mute=true
//                                   transport=play source=x key=ok volume_step=1
//   apply-many ID,ID,... KEY=VALUE...
//                                   as the app does it (src/smarthome/control.cpp apply_device):
//                                   apply_many, and when that returns false with no reason, apply
//                                   to each thing alone; then prints {"each": {id: "ok" | error}}
//   code INPUT EXPECTED             whether INPUT reads as the setup code EXPECTED; no network.
//                                   Prints {"matches": bool}, never the code.
//   time-verify N                   connects and runs pair-verify N times, with timings
// Prints one line of JSON on stdout for each successful command, or "error: ..." on stderr. Exit
// status 0 when every command succeeded, 1 otherwise, 2 for a usage error. --then runs further
// commands with the same driver object, as the app keeps one for its whole life; --repeat N runs
// the whole list N times, which is how "never sent again" can be watched.
// Secrets and Device::auth are never printed.
#include "smarthome/drivers/homekit.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using home::json;

namespace {
class FileSecrets : public home::Secrets {
public:
    explicit FileSecrets(json &root) : root_(root) {}
    json get(const std::string &family) const override {
        if (!root_.contains("secrets") || !root_["secrets"].is_object() || !root_["secrets"].contains(family))
            return json::object();
        return root_["secrets"][family];
    }
    void set(const std::string &family, const json &value) override {
        if (!root_.contains("secrets") || !root_["secrets"].is_object()) root_["secrets"] = json::object();
        root_["secrets"][family] = value;
        sets++;
    }
    int sets = 0;

private:
    json &root_;
};

json thing_json(const home::Thing &t) {
    return {{"id", t.id},     {"name", t.name},   {"alias", t.alias}, {"room", t.room},           {"type", t.type},
            {"caps", t.caps}, {"enabled", t.enabled}, {"ask_first", t.ask_first}, {"ref", t.ref}, {"state", t.state}};
}

home::Thing thing_from(const json &j) {
    home::Thing t;
    t.id = j.value("id", "");
    t.name = j.value("name", "");
    t.alias = j.value("alias", "");
    t.room = j.value("room", "");
    t.type = j.value("type", "");
    t.caps = j.value("caps", 0u);
    t.enabled = j.value("enabled", true);
    t.ask_first = j.value("ask_first", false);
    t.ref = j.contains("ref") ? j["ref"] : json::object();
    t.state = j.contains("state") ? j["state"] : json::object();
    return t;
}

home::Device device_from(const json &j) {
    home::Device d;
    d.id = j.value("id", "");
    d.kind = j.value("kind", "homekit");
    d.name = j.value("name", "");
    d.model = j.value("model", "");
    d.ip = j.value("ip", "");
    d.port = j.value("port", 0);
    if (j.contains("info") && j["info"].is_object())
        for (auto it = j["info"].begin(); it != j["info"].end(); ++it)
            if (it.value().is_string()) d.info[it.key()] = it.value().get<std::string>();
    d.auth = j.contains("auth") && j["auth"].is_object() ? j["auth"] : json::object();
    if (j.contains("things") && j["things"].is_array())
        for (const json &t : j["things"]) d.things.push_back(thing_from(t));
    return d;
}

json device_json(const home::Device &d) {
    json info = json::object();
    for (const auto &kv : d.info) info[kv.first] = kv.second;
    json things = json::array();
    for (const home::Thing &t : d.things) things.push_back(thing_json(t));
    return {{"id", d.id},   {"kind", d.kind}, {"name", d.name}, {"model", d.model}, {"ip", d.ip},
            {"port", d.port}, {"info", info},  {"auth", d.auth}, {"things", things}};
}

bool load(const std::string &path, json &root) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    root = json::parse(ss.str(), nullptr, false);
    return root.is_object();
}

// Written to a new file that only its owner may read (it holds this controller's private key),
// then renamed over the old one.
bool save(const std::string &path, const json &root) {
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const std::string text = root.dump(2) + "\n";
    bool ok = fchmod(fd, 0600) == 0;   // in case an older .tmp was left with another mode
    for (size_t off = 0; ok && off < text.size();) {
        const ssize_t n = ::write(fd, text.data() + off, text.size() - off);
        if (n <= 0) ok = false;
        else off += static_cast<size_t>(n);
    }
    ok = ::close(fd) == 0 && ok;
    return ok && std::rename(tmp.c_str(), path.c_str()) == 0;
}

bool parse_bool(const std::string &v, bool &out) {
    if (v == "true" || v == "1" || v == "on") out = true;
    else if (v == "false" || v == "0" || v == "off") out = false;
    else return false;
    return true;
}

bool parse_change(const std::vector<std::string> &kvs, home::Change &c) {
    for (const std::string &kv : kvs) {
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) return false;
        const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
        try {
            bool b;
            if (k == "on" && parse_bool(v, b)) c.on = b;
            else if (k == "mute" && parse_bool(v, b)) c.mute = b;
            else if (k == "brightness") c.brightness = std::stoi(v);
            else if (k == "kelvin") c.kelvin = std::stoi(v);
            else if (k == "target") c.target_c = std::stod(v);
            else if (k == "mode") c.mode = v;
            else if (k == "position") c.position = std::stoi(v);
            else if (k == "fan") c.fan = std::stoi(v);
            else if (k == "volume") c.volume = std::stoi(v);
            else if (k == "volume_step") c.volume_step = std::stoi(v);
            else if (k == "transport") c.transport = v;
            else if (k == "source") c.source = v;
            else if (k == "key") c.key = v;
            else if (k == "hs" && v.find(',') != std::string::npos)
                c.hs = std::make_pair(std::stoi(v.substr(0, v.find(','))), std::stoi(v.substr(v.find(',') + 1)));
            else return false;
        } catch (...) {
            return false;
        }
    }
    return true;
}

const char *pairing_name(home::Pairing p) {
    switch (p) {
    case home::Pairing::None: return "None";
    case home::Pairing::PressButton: return "PressButton";
    case home::Pairing::EnterCode: return "EnterCode";
    case home::Pairing::AcceptPrompt: return "AcceptPrompt";
    case home::Pairing::Password: return "Password";
    case home::Pairing::Token: return "Token";
    }
    return "?";
}

int usage() {
    fprintf(stderr, "usage: homekit_cli STATE init IP PORT [KEY=VALUE...] | claims [KIND] | step | pair INPUT | unpair | "
                    "enumerate | read [ID...] | apply ID KEY=VALUE... | apply-many ID,ID KEY=VALUE... | code INPUT EXPECTED | "
                    "time-verify N   [--then COMMAND...]... [--repeat N]\n");
    return 2;
}

std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}
}  // namespace

int main(int argc, char **argv) {
    std::vector<std::string> args;
    int repeat = 1;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--repeat") {
            if (i + 1 >= argc) return usage();
            repeat = atoi(argv[++i]);
        } else {
            args.push_back(a);
        }
    }
    if (args.size() < 2 || repeat < 1 || repeat > 100) return usage();
    const std::string path = args[0];
    std::vector<std::vector<std::string>> cmds(1);   // split at --then
    for (size_t i = 1; i < args.size(); i++) {
        if (args[i] == "--then") {
            if (cmds.back().empty()) return usage();
            cmds.emplace_back();
        } else {
            cmds.back().push_back(args[i]);
        }
    }
    if (cmds.back().empty()) return usage();

    if (cmds[0][0] == "code") {
        if (cmds.size() != 1 || cmds[0].size() != 3) return usage();
        std::string code, expected, err;
        if (!homekit::detail::parse_setup_code(cmds[0][2], expected, err)) return usage();
        if (!homekit::detail::parse_setup_code(cmds[0][1], code, err)) {
            fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        printf("%s\n", json({{"matches", code == expected}}).dump().c_str());
        return 0;
    }
    if (cmds[0][0] == "init") {
        const std::vector<std::string> rest(cmds[0].begin() + 1, cmds[0].end());
        if (cmds.size() != 1 || rest.size() < 2) return usage();
        json info = json::object();
        for (size_t i = 2; i < rest.size(); i++) {
            const size_t eq = rest[i].find('=');
            if (eq == std::string::npos) return usage();
            info[rest[i].substr(0, eq)] = rest[i].substr(eq + 1);
        }
        const std::string id = info.contains("id") ? info["id"].get<std::string>() : "AA:BB:CC:DD:EE:01";
        json root = {{"secrets", json::object()},
                     {"device", {{"id", "homekit:" + id}, {"kind", "homekit"}, {"name", "Mock Bridge"}, {"model", "MockBridge1,1"},
                                 {"ip", rest[0]}, {"port", atoi(rest[1].c_str())}, {"info", info}, {"auth", json::object()},
                                 {"things", json::array()}}}};
        if (!save(path, root)) {
            fprintf(stderr, "error: cannot write %s\n", path.c_str());
            return 1;
        }
        printf("{\"ok\":true}\n");
        return 0;
    }

    json root;
    if (!load(path, root) || !root.contains("device")) {
        fprintf(stderr, "error: cannot read %s\n", path.c_str());
        return 2;
    }
    FileSecrets secrets(root);
    home::Device dev = device_from(root["device"]);
    std::unique_ptr<home::Driver> driver = make_homekit_driver();
    driver->attach(&secrets);

    int failures = 0;
    for (int run = 0; run < repeat; run++)
        for (const std::vector<std::string> &c : cmds) {
            const std::string &cmd = c[0];
            const std::vector<std::string> rest(c.begin() + 1, c.end());
            std::string err;
            bool ok = true, print = false;
            json out = {{"ok", true}};
            if (cmd == "claims") {
                out["claims"] = driver->claims(rest.empty() ? dev.kind : rest[0], dev.info);
            } else if (cmd == "step") {
                const home::PairStep s = driver->next_step(dev);
                out = {{"kind", pairing_name(s.kind)}, {"prompt", s.prompt}, {"field", s.field}, {"wait_s", s.wait_s}};
            } else if (cmd == "pair") {
                if (rest.size() != 1) return usage();
                ok = driver->pair(dev, rest[0], err);
                out["paired"] = ok;
            } else if (cmd == "unpair") {
                driver->unpair(dev);
                out["paired"] = dev.auth.contains("accessory_id");
            } else if (cmd == "enumerate") {
                std::vector<home::Thing> things;
                ok = driver->enumerate(dev, things, err);
                if (ok) {
                    dev.things = things;
                    json list = json::array();
                    for (const home::Thing &t : things) list.push_back(thing_json(t));
                    out["things"] = list;
                }
            } else if (cmd == "read") {
                std::vector<home::Thing> things;
                for (const std::string &id : rest)
                    for (const home::Thing &t : dev.things)
                        if (t.id == id) things.push_back(t);
                if (!rest.empty() && things.size() != rest.size()) {
                    fprintf(stderr, "error: no such thing\n");
                    return 2;
                }
                ok = driver->read(dev, things, err);
                if (ok) {
                    json list = json::array();
                    for (const home::Thing &t : things) list.push_back(thing_json(t));
                    out["things"] = list;
                }
            } else if (cmd == "apply" || cmd == "apply-many") {
                if (rest.size() < 1) return usage();
                home::Change change;
                if (!parse_change(std::vector<std::string>(rest.begin() + 1, rest.end()), change)) return usage();
                std::vector<const home::Thing *> targets;
                for (const std::string &id : split(rest[0], ','))
                    for (const home::Thing &t : dev.things)
                        if (t.id == id) targets.push_back(&t);
                if (targets.empty() || (cmd == "apply" && targets.size() != 1)) {
                    fprintf(stderr, "error: no such thing\n");
                    return 2;
                }
                if (cmd == "apply") {
                    ok = driver->apply(dev, *targets[0], change, 0, err);
                } else {
                    ok = driver->apply_many(dev, targets, change, 0, err);
                    if (!ok && err.empty()) {
                        // What the app does then (control.cpp apply_device): each thing alone.
                        json each = json::object();
                        ok = true;
                        for (const home::Thing *t : targets) {
                            std::string e;
                            const bool one = driver->apply(dev, *t, change, 0, e);
                            each[t->id] = one ? std::string("ok") : e.empty() ? std::string("(no reason given)") : e;
                            ok = ok && one;
                        }
                        out["each"] = each;
                        print = true;
                        if (!ok) err = "not every thing was changed";
                    }
                }
            } else if (cmd == "time-verify") {
                if (rest.size() != 1) return usage();
                std::vector<double> ms;
                ok = homekit::detail::time_pair_verify(dev, &secrets, atoi(rest[0].c_str()), ms, err);
                if (ok) out["ms"] = ms;
            } else {
                return usage();
            }
            if (ok || print) printf("%s\n", out.dump(-1, ' ', false, json::error_handler_t::replace).c_str());
            if (!ok) {
                fprintf(stderr, "error: %s\n", err.empty() ? "(no reason given)" : err.c_str());
                failures++;
            }
            fflush(stdout);
            fflush(stderr);
        }
    root["device"] = device_json(dev);
    if (!save(path, root)) {
        fprintf(stderr, "error: cannot write %s\n", path.c_str());
        return 1;
    }
    return failures ? 1 : 0;
}
