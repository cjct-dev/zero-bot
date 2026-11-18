// Command-line front end to the Kasa driver, for testing against tests/mocks/kasa/mock_kasa.py.
//
//   kasa_cli STATE.json COMMAND [ARG...] [--deadline MS] [--probe-port N]
//   kasa_cli - discover-udp PORT | discover-probe PORT
//
// STATE.json holds {"device": {id, kind, name, model, mac, ip, port, info}, "things": [...]}. It
// stands in for the app's saved-devices file: enumerate and read write the things back.
//
// Commands:
//   claims                 {"claims": true|false, "tapo_claims": true|false} for the device's kind and
//                          info: whether this driver, and whether the Tapo driver, would take it
//   enumerate              {"things": [{id, name, type, caps, cap_names, ref, state}], "ms"}
//   read [ID,ID...]        the same after refreshing the state: of every thing (as the app's lists
//                          do), or in detail (meter readings too) of the things named
//   apply ID CHANGE        CHANGE is JSON with fields of home::Change, e.g. '{"on":true}' or '{"hs":[240,100]}'
//   apply-many ID,ID CHANGE   the change to several things in one call (apply_many); {"handled": false}
//                          when the driver has no single request for them
//   discover-udp PORT      sends discovery's scrambled get_sysinfo query to 127.0.0.1:PORT, as the
//                          app broadcasts it to port 9999, and prints what discovery makes of the
//                          answers: {"devices": [{id, kind, name, model, mac, port, info, found_by}]}
//   discover-probe PORT    the same with TP-Link's 16-byte 20002 query
// --deadline MS sets apply's deadline (default 2500). --probe-port N is where the driver sends its
// 20002 query when the device's port refuses (default 20002).
//
// Prints one line of JSON on stdout, or "error: <message>" on stderr. Exit status 0 on success,
// 1 on failure, 2 for a usage error. Only 127.0.0.1 is ever sent a discovery query.
#include "smarthome/drivers/kasa.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "smarthome/discovery.h"
#include "smarthome/drivers/tapo_driver.h"

using kasa::json;

static int usage() {
    fprintf(stderr, "usage: kasa_cli STATE.json <claims|enumerate|read [IDS]|apply ID CHANGE|apply-many IDS CHANGE> "
                    "[--deadline MS] [--probe-port N]\n       kasa_cli - <discover-udp|discover-probe> PORT\n");
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
        {home::kPower, "power"},   {home::kBrightness, "brightness"}, {home::kColorTemp, "color_temp"},
        {home::kColor, "color"},   {home::kSensor, "sensor"}};
    json out = json::array();
    for (const auto &n : names)
        if (caps & n.bit) out.push_back(n.name);
    return out;
}

static json thing_json(const home::Thing &t) {
    return {{"id", t.id}, {"name", t.name}, {"type", t.type}, {"caps", t.caps}, {"cap_names", caps_names(t.caps)},
            {"ref", t.ref}, {"state", t.state}};
}

static bool load(const std::string &path, home::Device &d, std::vector<home::Thing> &things, std::string &err) {
    std::ifstream f(path);
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
    if (j.contains("things") && j["things"].is_array())
        for (const json &t : j["things"]) things.push_back(thing_from(t));
    d.things = things;
    return true;
}

static void save(const std::string &path, const home::Device &d, const std::vector<home::Thing> &things) {
    json info = json::object();
    for (const auto &kv : d.info) info[kv.first] = kv.second;
    json list = json::array();
    for (const home::Thing &t : things) list.push_back(thing_json(t));
    const json j = {{"device", {{"id", d.id}, {"kind", d.kind}, {"name", d.name}, {"model", d.model}, {"mac", d.mac},
                                {"ip", d.ip}, {"port", d.port}, {"info", info}}},
                    {"things", list}};
    std::ofstream(path) << j.dump(2, ' ', false, json::error_handler_t::replace) << "\n";
}

static bool change_from(const std::string &text, home::Change &c, std::string &err) {
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
        if (j.contains("volume")) c.volume = j["volume"].get<int>();
        if (j.contains("key")) c.key = j["key"].get<std::string>();
    } catch (const std::exception &) {
        err = "the change has a field of the wrong type";
        return false;
    }
    return true;
}

static std::vector<std::string> split(const std::string &s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    for (std::string part; std::getline(ss, part, ',');)
        if (!part.empty()) out.push_back(part);
    return out;
}

static int fail(const std::string &err) {
    fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
}

// Sends `query` to 127.0.0.1:port and hands every answer heard within a second to discovery.
static int discover(const std::string &proto, const std::string &query, int port) {
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
    if (fd < 0 || sendto(fd, query.data(), query.size(), 0, reinterpret_cast<sockaddr *>(&to), sizeof(to)) < 0)
        return fail("could not send the query");
    std::vector<discovery::Packet> heard;
    const auto stop = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    std::string buf(9000, '\0');
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(stop - std::chrono::steady_clock::now()).count();
        pollfd p{fd, POLLIN, 0};
        if (left <= 0 || poll(&p, 1, static_cast<int>(left)) <= 0) break;
        sockaddr_in from{};
        socklen_t flen = sizeof(from);
        const ssize_t n = recvfrom(fd, &buf[0], buf.size(), 0, reinterpret_cast<sockaddr *>(&from), &flen);
        if (n <= 0) continue;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        heard.push_back({proto, ip, buf.substr(0, static_cast<size_t>(n))});
    }
    close(fd);
    json list = json::array();
    for (const auto &d : discovery::devices_from(heard)) {
        json info = json::object();
        for (const auto &kv : d.info) info[kv.first] = kv.second;
        list.push_back({{"id", d.id}, {"kind", d.kind}, {"name", d.name}, {"model", d.model}, {"mac", d.mac},
                        {"port", d.port}, {"info", info}, {"found_by", d.found_by}});
    }
    printf("%s\n", json{{"devices", list}, {"answers", heard.size()}}.dump(-1, ' ', false, json::error_handler_t::replace).c_str());
    return 0;
}

int main(int argc, char **argv) {
    std::vector<std::string> args;
    int deadline = 2500;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if ((a == "--deadline" || a == "--probe-port") && i + 1 >= argc) return usage();
        if (a == "--deadline") deadline = atoi(argv[++i]);
        else if (a == "--probe-port") kasa::detail::set_probe_port(atoi(argv[++i]));
        else args.push_back(a);
    }
    if (args.size() < 2) return usage();
    const std::string &path = args[0], &cmd = args[1];
    if (cmd == "discover-udp" || cmd == "discover-probe") {
        if (args.size() != 3) return usage();
        static const unsigned char probe[16] = {0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
                                                0x00, 0x00, 0x00, 0x00, 0x46, 0x3c, 0xb5, 0xd3};
        return cmd == "discover-udp"
                   ? discover("kasa", kasa::detail::encrypt(R"({"system":{"get_sysinfo":{}}})"), atoi(args[2].c_str()))
                   : discover("tapo", std::string(reinterpret_cast<const char *>(probe), sizeof(probe)), atoi(args[2].c_str()));
    }

    home::Device dev;
    std::vector<home::Thing> things;
    std::string err;
    if (!load(path, dev, things, err)) {
        fprintf(stderr, "error: %s\n", err.c_str());
        return 2;
    }
    auto driver = make_kasa_driver();
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed = [&] {
        return static_cast<long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
    };
    auto find = [&](const std::string &id) -> const home::Thing * {
        for (const auto &t : things)
            if (t.id == id) return &t;
        return nullptr;
    };

    if (cmd == "claims" && args.size() == 2) {
        printf("%s\n", json{{"claims", driver->claims(dev.kind, dev.info)},
                            {"tapo_claims", make_tapo_driver()->claims(dev.kind, dev.info)}}.dump().c_str());
        return 0;
    }
    if ((cmd == "enumerate" && args.size() == 2) || (cmd == "read" && args.size() <= 3)) {
        std::vector<home::Thing> out;
        if (cmd == "read" && args.size() == 3)
            for (const auto &id : split(args[2])) {
                const home::Thing *t = find(id);
                if (!t) return fail("no thing " + id + " in the state file");
                out.push_back(*t);
            }
        const bool ok = cmd == "enumerate" ? driver->enumerate(dev, out, err) : driver->read(dev, out, err);
        if (!ok) return fail(err);
        if (cmd == "enumerate") things = out;
        else
            for (const auto &t : out)
                for (auto &saved : things)
                    if (saved.id == t.id) saved.state = t.state;
        save(path, dev, things);
        json list = json::array();
        for (const home::Thing &t : out) list.push_back(thing_json(t));
        printf("%s\n", json{{"things", list}, {"name", dev.name}, {"model", dev.model}, {"ms", elapsed()}}
                           .dump(-1, ' ', false, json::error_handler_t::replace).c_str());
        return 0;
    }
    if ((cmd == "apply" || cmd == "apply-many") && args.size() == 4) {
        home::Change change;
        if (!change_from(args[3], change, err)) {
            fprintf(stderr, "error: %s\n", err.c_str());
            return 2;
        }
        if (cmd == "apply") {
            const home::Thing *t = find(args[2]);
            if (!t) return fail("no thing " + args[2] + " in the state file");
            if (!driver->apply(dev, *t, change, deadline, err)) return fail(err);
            printf("%s\n", json{{"ok", true}, {"ms", elapsed()}}.dump().c_str());
            return 0;
        }
        std::vector<const home::Thing *> list;
        for (const auto &id : split(args[2])) {
            const home::Thing *t = find(id);
            if (!t) return fail("no thing " + id + " in the state file");
            list.push_back(t);
        }
        const bool ok = driver->apply_many(dev, list, change, deadline, err);
        if (!ok && err.empty()) {
            printf("%s\n", json{{"handled", false}}.dump().c_str());
            return 0;
        }
        if (!ok) return fail(err);
        printf("%s\n", json{{"ok", true}, {"handled", true}, {"ms", elapsed()}}.dump().c_str());
        return 0;
    }
    return usage();
}
