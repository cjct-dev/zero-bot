// Known-answer and hostile-answer tests for shelly.cpp. No network is used: what would contact a
// device is checked to stop before it (a refused password, a missing one), against a port nothing
// listens on, so that an attempt would show as "did not answer" instead.
// Each check prints one line on failure; the exit status is non-zero when any fails.
//
// Where the expected values come from:
//   docs     Shelly's API documentation (shelly-api-docs.shelly.cloud): the Shelly.GetDeviceInfo
//            example (shellypro4pm-f008d1d8b8b8), the Gen1 /shelly example (SHSW-21), the Set
//            requests' parameter names, Cover methods, the RGB [r,g,b] array
//   python   computed independently with Python's hashlib, base64 and colorsys
//   design   what shelly.h promises (names, refusals, the allow-list)
#include "smarthome/drivers/shelly.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <vector>
#include <unistd.h>

#include <curl/curl.h>

#include "smarthome/home.h"

using namespace shelly::detail;
using home::json;

static int g_run = 0, g_failed = 0;
static void check(bool ok, const std::string &what, int line) {
    ++g_run;
    if (!ok) {
        ++g_failed;
        printf("FAIL line %d: %s\n", line, what.c_str());
    }
}
#define CHECK(c) check((c), #c, __LINE__)
#define CHECK_MSG(c, m) check((c), std::string(#c) + "  -> " + (m), __LINE__)

static bool starts(const std::string &s, const char *p) { return s.compare(0, strlen(p), p) == 0; }

static home::Thing *find(std::vector<home::Thing> &v, const std::string &id) {
    for (auto &t : v)
        if (t.id == id) return &t;
    return nullptr;
}

static Identity gen2_who(const std::string &app = "Plus1PM") {
    Identity w;
    w.gen = 2;
    w.id = "shellyplus1pm-a8032ab12345";
    w.mac = "a8032ab12345";
    w.app = app;
    return w;
}
static Identity gen1_who(const std::string &type) {
    Identity w;
    w.gen = 1;
    w.mac = "a4cf12f45378";
    w.model = type;
    return w;
}

// ---------------------------------------------------------------- /shelly
static void test_identity() {
    Identity w;
    // docs: the Shelly.GetDeviceInfo example
    CHECK(parse_identity(R"({"id":"shellypro4pm-f008d1d8b8b8","mac":"F008D1D8B8B8","model":"SPSW-004PE16EU","gen":2,
        "fw_id":"20210720-153353/0.6.7-gc36674b","ver":"0.6.7","app":"FourPro","auth_en":true,
        "auth_domain":"shellypro4pm-f008d1d8b8b8","discoverable":false,"enhanced_security":false})", w));
    CHECK(w.gen == 2 && w.id == "shellypro4pm-f008d1d8b8b8" && w.mac == "f008d1d8b8b8" && w.auth && w.app == "FourPro");
    CHECK(w.model == "SPSW-004PE16EU" && w.who() == "shellypro4pm-f008d1d8b8b8");
    CHECK(parse_identity(R"({"id":"ShellyPlus2PM-ABC","mac":"A8032AB12345","gen":3,"app":"Plus2PM","profile":"cover","auth_en":false})", w));
    CHECK(w.gen == 3 && w.id == "shellyplus2pm-abc" && w.profile == "cover" && !w.auth);
    // docs: the Gen1 /shelly example
    CHECK(parse_identity(R"({"type":"SHSW-21","mac":"5ECF7F1632E8","auth":true,"fw":"20161223-111304/master@2bc16466","longid":1})", w));
    CHECK(w.gen == 1 && w.model == "SHSW-21" && w.mac == "5ecf7f1632e8" && w.auth && w.who() == "5ecf7f1632e8");
    CHECK(parse_identity(R"({"type":"SHSW-PM","mac":"A4CF12F45378","auth":false,"num_meters":1})", w) && w.num_meters == 1);
    // hostile or broken answers are not understood
    for (const char *bad : {R"({"gen":"2","id":"shellyplus1-x","mac":"A8032AB12345"})", R"({"gen":99,"id":"x","mac":"A8032AB12345"})",
                            R"({"gen":2,"id":"shelly 1/../x","mac":"A8032AB12345"})", R"({"gen":2,"mac":"A8032AB12345"})",
                            R"({"type":"SHSW-1"})", R"({"type":"SHSW-1","mac":"nothex"})", R"({"type":"SH SW","mac":"A4CF12F45378"})",
                            R"(["gen",2])", "not json", "", R"({"gen":2.5,"id":"x","mac":"A8032AB12345"})"})
        CHECK_MSG(!parse_identity(bad, w), bad);
    std::string deep(40, '[');
    deep += std::string(40, ']');
    CHECK(!parse_identity(R"({"type":"SHSW-1","mac":"A4CF12F45378","x":)" + deep + "}", w));
    CHECK(!parse_identity(R"({"type":"SHSW-1","mac":"A4CF12F45378","pad":")" + std::string(300 * 1024, 'x') + "\"}", w));
}

// ---------------------------------------------------------------- Gen2+ things and states
static void test_gen2_things() {
    std::string name;
    const json status1 = json::parse(R"({"sys":{"mac":"A8032AB12345"},"wifi":{"ssid":"ExampleNet"},"input:0":{"id":0,"state":false},
        "switch:0":{"id":0,"output":true,"apower":12.54,"voltage":230.1,"aenergy":{"total":11.679},"temperature":{"tC":41.2}}})");
    const json config1 = json::parse(R"({"sys":{"device":{"name":null}},"switch:0":{"id":0,"name":"Desk lamp"}})");
    auto things = things_gen2(gen2_who(), config1, status1, name);
    CHECK(things.size() == 1 && name.empty());
    if (things.size() == 1) {
        const auto &t = things[0];
        CHECK(t.id == "switch:0" && t.name == "Desk lamp" && t.type == "switch" && t.caps == (home::kPower | home::kSensor));
        CHECK_MSG(t.state == json({{"on", true}, {"power_w", 12.5}}), t.state.dump());
        CHECK(t.ref == json({{"gen", 2}, {"comp", "switch"}, {"id", 0}, {"dev", "shellyplus1pm-a8032ab12345"}, {"mac", "a8032ab12345"}}));
    }
    // A plug by its app name; a relay set to feed lights is a light; names fall back to the device's.
    things = things_gen2(gen2_who("PlugS"), json::parse(R"({"switch:0":{"name":"Kettle"}})"),
                         json::parse(R"({"switch:0":{"output":false}})"), name);
    CHECK(things.size() == 1 && things[0].type == "plug" && things[0].caps == home::kPower && things[0].state == json({{"on", false}}));
    const json config2 = json::parse(R"({"sys":{"device":{"name":"Kitchen"},"ui_data":{"consumption_types":["","light"]}},
        "switch:0":{"name":null},"switch:1":{"name":"Cooker hood light"}})");
    const json status2 = json::parse(R"({"switch:1":{"output":true,"apower":0},"switch:0":{"output":false,"apower":5}})");
    things = things_gen2(gen2_who("Plus2PM"), config2, status2, name);
    CHECK(name == "Kitchen" && things.size() == 2);
    if (things.size() == 2) {
        CHECK(things[0].id == "switch:0" && things[0].name == "Kitchen 1" && things[0].type == "switch");
        CHECK(things[1].id == "switch:1" && things[1].name == "Cooker hood light" && things[1].type == "light");
    }
    // No name anywhere: the model.
    things = things_gen2(gen2_who("Plus1"), json::object(), json::parse(R"({"switch:0":{"output":false}})"), name);
    CHECK(things.size() == 1 && things[0].name == "Shelly Plus1");
    // Keys are numbers: switch:10 comes after switch:2; malformed keys are ignored.
    json many = json::object();
    for (int i = 0; i < 12; ++i) many["switch:" + std::to_string(i)] = {{"output", false}};
    for (const char *bad : {"switch:abc", "switch:0001", "switch:999", "switch:", ":0", "switch:-1", "averyveryverylongname:1"})
        many[bad] = {{"output", true}};
    many["switch:5"] = "not an object";
    things = things_gen2(gen2_who(), json::object(), many, name);
    CHECK_MSG(things.size() == 11, std::to_string(things.size()));
    CHECK(things.size() == 11 && things[1].id == "switch:1" && things[2].id == "switch:2" && things[10].id == "switch:11");
    // At most 32 things.
    json lots = json::object();
    for (int i = 0; i < 60; ++i) lots["light:" + std::to_string(i)] = {{"output", false}};
    CHECK(things_gen2(gen2_who(), json::object(), lots, name).size() == 32);

    // Covers: calibrated or not; position from current_pos, else open/closed.
    things = things_gen2(gen2_who("Plus2PM"), json::parse(R"({"cover:0":{"name":"Lounge blind"}})"),
                         json::parse(R"({"cover:0":{"state":"stopped","pos_control":true,"current_pos":40}})"), name);
    CHECK(things.size() == 1 && things[0].type == "cover" && things[0].caps == home::kPosition);
    CHECK(things.size() == 1 && things[0].ref.value("calibrated", false) && things[0].state == json({{"position", 40}}));
    things = things_gen2(gen2_who("Plus2PM"), json::object(), json::parse(R"({"cover:0":{"state":"open","pos_control":false}})"), name);
    CHECK(things.size() == 1 && !things[0].ref.value("calibrated", true) && things[0].state == json({{"position", 100}}));
    things = things_gen2(gen2_who("Plus2PM"), json::object(), json::parse(R"({"cover:0":{"state":"stopped"}})"), name);
    CHECK(things.size() == 1 && things[0].state == json::object());

    // Lights: dimmer, colour (rgb array to hue and saturation), white with its range.
    things = things_gen2(gen2_who("PlusRGBWPM"), json::parse(R"({"rgb:0":{"name":"Shelf strip"}})"),
                         json::parse(R"({"rgb:0":{"output":true,"rgb":[0,0,255],"brightness":50}})"), name);
    CHECK(things.size() == 1 && things[0].caps == (home::kPower | home::kBrightness | home::kColor));
    CHECK_MSG(things.size() == 1 && things[0].state == json({{"on", true}, {"brightness", 50}, {"hue", 240}, {"saturation", 100}}),
              things.empty() ? "" : things[0].state.dump());
    things = things_gen2(gen2_who("DuoBulbG3"), json::parse(R"({"cct:0":{"name":"Reading lamp","ct_range":[3000,5000]}})"),
                         json::parse(R"({"cct:0":{"output":false,"brightness":80,"ct":4000}})"), name);
    CHECK(things.size() == 1 && things[0].caps == (home::kPower | home::kBrightness | home::kColorTemp));
    CHECK(things.size() == 1 && things[0].ref.value("kmin", 0) == 3000 && things[0].ref.value("kmax", 0) == 5000);
    CHECK(things.size() == 1 && things[0].state == json({{"on", false}, {"brightness", 80}, {"kelvin", 4000}}));
    things = things_gen2(gen2_who("DuoBulbG3"), json::parse(R"({"cct:0":{"ct_range":[9000,100]}})"), json::parse(R"({"cct:0":{}})"), name);
    CHECK(things.size() == 1 && things[0].ref.value("kmin", 0) == 2700 && things[0].ref.value("kmax", 0) == 6500);

    // Sensors: humidity rides with the temperature of the same number; alone otherwise.
    things = things_gen2(gen2_who("PlusHT"), json::parse(R"({"sys":{"device":{"name":"Nursery"}}})"),
                         json::parse(R"({"temperature:0":{"tC":21.37},"humidity:0":{"rh":48.6},"humidity:1":{"rh":30},"devicepower:0":{}})"), name);
    CHECK_MSG(things.size() == 2, std::to_string(things.size()));
    if (things.size() == 2) {
        CHECK(things[0].id == "temperature:0" && things[0].type == "sensor" && things[0].caps == home::kSensor);
        CHECK_MSG(things[0].state == json({{"current_c", 21.4}, {"humidity", 49}}), things[0].state.dump());
        CHECK(things[0].name == "Nursery 1" && things[1].id == "humidity:1" && things[1].state == json({{"humidity", 30}}));
    }
    // A sensor beside a switch is named apart from it.
    things = things_gen2(gen2_who(), json::parse(R"({"sys":{"device":{"name":"Shed"}}})"),
                         json::parse(R"({"switch:0":{"output":false},"temperature:100":{"tC":5}})"), name);
    CHECK(things.size() == 2 && things[0].name == "Shed" && things[1].name == "Shed sensor" && things[1].id == "temperature:100");

    // Never offered: a relay its own thermostat drives, a garage door, inputs, meters, thermostats.
    things = things_gen2(gen2_who("WallDisplay"), json::parse(R"({"temperature:0":{"name":"Hall temperature"}})"),
                         json::parse(R"({"sys":{"relay_in_thermostat":true},"switch:0":{"output":true},"temperature:0":{"tC":19.5},
                                         "thermostat:0":{"target_C":20},"em:0":{},"input:0":{"state":true},"smoke:0":{"alarm":false}})"), name);
    CHECK(things.size() == 1 && things[0].id == "temperature:0" && things[0].name == "Hall temperature");
    things = things_gen2(gen2_who(), json::parse(R"({"switch:0":{"name":"Garage door"},"cover:1":{"name":"Front gate"},"switch:2":{"name":"Porch door light"}})"),
                         json::parse(R"({"switch:0":{"output":false},"cover:1":{"state":"closed"},"switch:2":{"output":false}})"), name);
    CHECK(things.size() == 1 && things[0].id == "switch:2");
    things = things_gen2(gen2_who(), json::parse(R"({"sys":{"ui_data":{"consumption_types":["Garage door opener"]}},"switch:0":{"name":"Big one"}})"),
                         json::parse(R"({"switch:0":{"output":false}})"), name);
    CHECK(things.empty());
    // review: the name a garage door goes by is often the device's, its relay left unnamed (it is
    // then called by the device's name); a compound word; a relay that switches itself off a
    // second after it is switched on (how an opener is wired, whatever it is called).
    const json one_off = json::parse(R"({"switch:0":{"output":false}})");
    things = things_gen2(gen2_who("Plus1"), json::parse(R"({"sys":{"device":{"name":"Garage door"}},"switch:0":{"name":null}})"), one_off, name);
    CHECK_MSG(things.empty(), things.empty() ? "" : things[0].name);
    things = things_gen2(gen2_who("Plus2PM"), json::parse(R"({"sys":{"device":{"name":"Garage"}},"cover:0":{"name":null}})"),
                         json::parse(R"({"cover:0":{"state":"closed","pos_control":true,"current_pos":0}})"), name);
    CHECK_MSG(things.empty(), things.empty() ? "" : things[0].name);
    things = things_gen2(gen2_who("Plus1"), json::parse(R"({"switch:0":{"name":"GarageDoor"}})"), one_off, name);
    CHECK(things.empty());
    things = things_gen2(gen2_who("Plus1"), json::parse(R"({"switch:0":{"name":"Opener","auto_off":true,"auto_off_delay":1.0}})"), one_off, name);
    CHECK(things.empty());
    // ... but lights in a garage, a relay that switches off after minutes, an auto-off left unused,
    // and a device named for its lights are offered.
    things = things_gen2(gen2_who("Plus2PM"),
                         json::parse(R"({"sys":{"device":{"name":"Garage"}},"switch:0":{"name":"Garage lights","auto_off":true,"auto_off_delay":120},
                                         "switch:1":{"name":"Strip light","auto_off":false,"auto_off_delay":1}})"),
                         json::parse(R"({"switch:0":{"output":false},"switch:1":{"output":false}})"), name);
    CHECK(things.size() == 2);
    things = things_gen2(gen2_who("Plus1"), json::parse(R"({"sys":{"device":{"name":"Garage lights"}},"switch:0":{"name":null}})"), one_off, name);
    CHECK(things.size() == 1 && things[0].name == "Garage lights");
    things = things_gen2(gen2_who("Plus1"), json::parse(R"({"switch:0":{"name":"Fan","auto_off":true,"auto_off_delay":"1"}})"), one_off, name);
    CHECK(things.size() == 1);   // a delay that is not a number is no delay

    // Hostile names and values: cleaned, capped, dropped.
    const std::string evil = "Lamp\x01\x1b[31m \"quoted\" \xe2\x80\x9ctypeset\xe2\x80\x9d <b>{x}</b> " + std::string(300, 'y');
    json cfg = {{"sys", {{"device", {{"name", evil}}}}}, {"switch:0", {{"name", evil}}}};
    json st = json::parse(R"({"switch:0":{"output":"yes","apower":"12"},"light:0":{"output":true,"brightness":5000,"apower":1e308},
        "rgb:0":{"output":1,"rgb":[300,-5,"x"],"brightness":-1},"cct:0":{"ct":99999,"brightness":50.4},"cover:0":{"current_pos":1e9,"state":42},
        "temperature:0":{"tC":-1e300},"humidity:0":{"rh":101},"pm1:0":{"apower":null}})");
    things = things_gen2(gen2_who(), cfg, st, name);
    CHECK(name.size() <= 40 && name.find('"') == std::string::npos && name.find('\x1b') == std::string::npos && name.find('<') == std::string::npos);
    CHECK(things.size() == 7);
    for (const auto &t : things) {
        CHECK_MSG(t.name.size() <= 40 && t.name.find('"') == std::string::npos && t.name.find('\x01') == std::string::npos, t.name);
        for (auto it = t.state.begin(); it != t.state.end(); ++it) {
            const bool sane = (it.key() == "brightness" && it->get<int>() >= 0 && it->get<int>() <= 100) ||
                              (it.key() == "on" && it->is_boolean());
            CHECK_MSG(sane, t.id + " " + t.state.dump());
        }
    }
    if (home::Thing *sw = find(things, "switch:0")) CHECK_MSG(sw->state.empty() && sw->caps == home::kPower, sw->state.dump());
    if (home::Thing *cct = find(things, "cct:0")) CHECK_MSG(cct->state == json({{"brightness", 50}}), cct->state.dump());
}

// ---------------------------------------------------------------- Gen1 things and states
static void test_gen1_things() {
    std::string name;
    const json meters = json::parse(R"([{"power":7.5,"is_valid":true,"total":4321}])");
    json settings = json::parse(R"({"name":null,"relays":[{"name":"Porch light","appliance_type":"lights"}]})");
    json status = {{"relays", json::parse(R"([{"ison":true}])")}, {"meters", meters}, {"mac", "A4CF12F45378"}};
    auto things = things_gen1(gen1_who("SHSW-PM"), settings, status, name);
    CHECK(things.size() == 1);
    if (things.size() == 1) {
        CHECK(things[0].id == "relay:0" && things[0].type == "light" && things[0].caps == (home::kPower | home::kSensor));
        CHECK(things[0].name == "Porch light" && things[0].state == json({{"on", true}, {"power_w", 7.5}}));
        CHECK(things[0].ref == json({{"gen", 1}, {"comp", "relay"}, {"id", 0}, {"dev", "a4cf12f45378"}, {"mac", "a4cf12f45378"}}));
    }
    // A Shelly 1 measures nothing (its meters entry is 0); a garage door is never offered.
    settings = json::parse(R"({"relays":[{"name":"Pump"}]})");
    things = things_gen1(gen1_who("SHSW-1"), settings, status, name);
    CHECK(things.size() == 1 && things[0].type == "switch" && things[0].caps == home::kPower && things[0].state == json({{"on", true}}));
    CHECK(things_gen1(gen1_who("SHSW-1"), json::parse(R"({"relays":[{"name":"Garage door"}]})"), status, name).empty());
    CHECK(things_gen1(gen1_who("SHSW-1"), json::parse(R"({"relays":[{"name":"x","appliance_type":"Lock"}]})"), status, name).empty());
    // review: the device's name when the relay has none; a pulse relay (auto_off in seconds); a
    // 2.5's blind, which has no name of its own, in a device named for the garage door.
    CHECK(things_gen1(gen1_who("SHSW-1"), json::parse(R"({"name":"Garage door","relays":[{"name":null,"auto_off":0.0}]})"), status, name).empty());
    CHECK(things_gen1(gen1_who("SHSW-1"), json::parse(R"({"relays":[{"name":"Opener","auto_off":1.5}]})"), status, name).empty());
    CHECK(things_gen1(gen1_who("SHSW-25"), json::parse(R"({"name":"Garage door","mode":"roller","relays":[{},{}],"rollers":[{"maxtime":20}]})"),
                      json::parse(R"({"rollers":[{"state":"stop","positioning":false}]})"), name).empty());
    things = things_gen1(gen1_who("SHSW-1"), json::parse(R"({"name":"Garage","relays":[{"name":"Garage light","auto_off":300}]})"), status, name);
    CHECK(things.size() == 1 && things[0].name == "Garage light");
    // A plug; a 2.5 in relay mode, with the device's name for an unnamed channel.
    things = things_gen1(gen1_who("SHPLG-S"), json::parse(R"({"relays":[{"name":"Fan heater"}]})"),
                         json::parse(R"({"relays":[{"ison":false}],"meters":[{"power":1500,"is_valid":true}]})"), name);
    CHECK(things.size() == 1 && things[0].type == "plug" && things[0].state == json({{"on", false}, {"power_w", 1500.0}}));
    things = things_gen1(gen1_who("SHSW-25"), json::parse(R"({"name":"Landing","mode":"relay","relays":[{"name":"Landing light"},{"name":null}]})"),
                         json::parse(R"({"relays":[{"ison":false},{"ison":true}],"meters":[{"power":3},{"power":0,"is_valid":false}]})"), name);
    CHECK(name == "Landing" && things.size() == 2);
    if (things.size() == 2) {
        CHECK(things[0].name == "Landing light" && (things[0].caps & home::kSensor));
        CHECK(things[1].name == "Landing 2" && things[1].caps == home::kPower && things[1].state == json({{"on", true}}));
    }
    // Roller mode: one blind; a position only when calibrated.
    const json roller_settings = json::parse(R"({"name":"Landing","mode":"roller","relays":[{},{}],"rollers":[{"maxtime":20}]})");
    things = things_gen1(gen1_who("SHSW-25"), roller_settings,
                         json::parse(R"({"rollers":[{"state":"stop","current_pos":70,"positioning":true}]})"), name);
    CHECK(things.size() == 1 && things[0].id == "roller:0" && things[0].type == "cover" && things[0].caps == home::kPosition);
    CHECK(things.size() == 1 && things[0].ref.value("calibrated", false) && things[0].state == json({{"position", 70}}) && things[0].name == "Landing");
    things = things_gen1(gen1_who("SHSW-25"), roller_settings,
                         json::parse(R"({"rollers":[{"state":"stop","current_pos":70,"positioning":false}]})"), name);
    CHECK(things.size() == 1 && !things[0].ref.value("calibrated", true) && things[0].state == json::object());
    // Lights: a dimmer (1..100), RGBW2 in colour mode (one light, gain) and white mode (four), a Duo.
    things = things_gen1(gen1_who("SHDM-2"), json::parse(R"({"lights":[{"name":"Dining light"}]})"),
                         json::parse(R"({"lights":[{"ison":true,"brightness":40}],"meters":[{"power":9}]})"), name);
    CHECK(things.size() == 1 && things[0].caps == (home::kPower | home::kBrightness | home::kSensor) && things[0].ref.value("min", 0) == 1);
    CHECK(things.size() == 1 && things[0].state == json({{"on", true}, {"brightness", 40}, {"power_w", 9.0}}));
    things = things_gen1(gen1_who("SHRGBW2"), json::parse(R"({"mode":"color","lights":[{"name":"Cabinet strip"},{"name":"x"}]})"),
                         json::parse(R"({"lights":[{"ison":false,"red":0,"green":0,"blue":255,"gain":60}],"meters":[{"power":0}]})"), name);
    CHECK(things.size() == 1 && (things[0].caps & home::kColor) && things[0].ref.value("path", "") == "color" && things[0].ref.value("gain", false));
    CHECK_MSG(things.size() == 1 && things[0].state == json({{"on", false}, {"brightness", 60}, {"hue", 240}, {"saturation", 100}, {"power_w", 0.0}}),
              things.empty() ? "" : things[0].state.dump());
    things = things_gen1(gen1_who("SHRGBW2"), json::parse(R"({"mode":"white","lights":[{},{},{},{}]})"),
                         json::parse(R"({"lights":[{"ison":true,"brightness":10},{"brightness":20},{"brightness":30},{"brightness":40}]})"), name);
    CHECK(things.size() == 4 && things[3].id == "light:3" && things[3].ref.value("path", "") == "white" && !(things[3].caps & home::kColor));
    CHECK(things.size() == 4 && things[3].state == json({{"brightness", 40}}) && things[0].name == "Shelly RGBW2 1");
    things = things_gen1(gen1_who("SHBDUO-1"), json::parse(R"({"name":"Study lamp","lights":[{"name":null}]})"),
                         json::parse(R"({"lights":[{"ison":true,"brightness":70,"white":50,"temp":3500}]})"), name);
    CHECK(things.size() == 1 && things[0].name == "Study lamp" && (things[0].caps & home::kColorTemp));
    CHECK(things.size() == 1 && things[0].ref.value("kmin", 0) == 2700 && things[0].state == json({{"on", true}, {"brightness", 70}, {"kelvin", 3500}}));
    // H&T: a sensor; an invalid reading is left out.
    things = things_gen1(gen1_who("SHHT-1"), json::parse(R"({"name":"Cellar"})"),
                         json::parse(R"({"tmp":{"value":12.4,"units":"C","tC":12.4,"is_valid":true},"hum":{"value":81,"is_valid":true}})"), name);
    CHECK(things.size() == 1 && things[0].id == "sensor" && things[0].name == "Cellar" && things[0].state == json({{"current_c", 12.4}, {"humidity", 81}}));
    things = things_gen1(gen1_who("SHHT-1"), json::object(), json::parse(R"({"tmp":{"tC":12.4,"is_valid":false},"hum":{"value":81}})"), name);
    CHECK(things.size() == 1 && things[0].state == json({{"humidity", 81}}));
    // Gas alarms, thermostats, door sensors: nothing.
    CHECK(things_gen1(gen1_who("SHGS-1"), json::parse(R"({"relays":[{"name":"x"}]})"), status, name).empty());
    CHECK(things_gen1(gen1_who("SHTRV-01"), json::object(), json::object(), name).empty());
    // Hostile: wrong types and sizes.
    json huge = json::array();
    for (int i = 0; i < 100; ++i) huge.push_back({{"name", "R" + std::to_string(i)}});
    things = things_gen1(gen1_who("SHSW-44"), {{"relays", huge}}, json::parse(R"({"relays":"nope","meters":5})"), name);
    CHECK(things.size() == 16 && things[0].state == json::object() && things[0].caps == home::kPower);
    things = things_gen1(gen1_who("SHSW-PM"), json::parse(R"({"relays":[5,{"name":"ok"}],"lights":{}})"),
                         json::parse(R"({"relays":[{"ison":"yes"},{"ison":true}],"meters":[{"power":"12"},{"power":1e30}]})"), name);
    CHECK(things.size() == 1 && things[0].id == "relay:1" && things[0].state == json({{"on", true}}) && things[0].caps == home::kPower);
}

// ---------------------------------------------------------------- changes
static home::Thing thing_of(const json &ref, uint32_t caps) {
    home::Thing t;
    t.ref = ref;
    t.caps = caps;
    return t;
}

static void test_plan() {
    Call call;
    std::string err;
    home::Change on;
    on.on = true;
    const auto sw2 = thing_of({{"gen", 2}, {"comp", "switch"}, {"id", 1}}, home::kPower);
    CHECK(plan(sw2, on, call, err) && call.method == "Switch.Set" && call.params == json({{"id", 1}, {"on", true}}) && allowed(call));
    home::Change dim;
    dim.brightness = 40;
    CHECK(!plan(sw2, dim, call, err) && err == "This switch can only be turned on and off.");
    const auto sw1 = thing_of({{"gen", 1}, {"comp", "relay"}, {"id", 0}}, home::kPower);
    home::Change off;
    off.on = false;
    CHECK(plan(sw1, off, call, err) && call.method.empty() && call.path == "/relay/0?turn=off" && allowed(call));

    // Gen2+ lights: one Set call each; a brightness means on; 0 means off.
    const auto light2 = thing_of({{"gen", 2}, {"comp", "light"}, {"id", 0}}, home::kPower | home::kBrightness);
    CHECK(plan(light2, dim, call, err) && call.method == "Light.Set" && call.params == json({{"id", 0}, {"on", true}, {"brightness", 40}}));
    home::Change zero;
    zero.brightness = 0;
    CHECK(plan(light2, zero, call, err) && call.params == json({{"id", 0}, {"on", false}}));
    home::Change huge;
    huge.brightness = 250;
    CHECK(plan(light2, huge, call, err) && call.params["brightness"] == 100);
    home::Change green;
    green.hs = std::make_pair(120, 100);
    CHECK(!plan(light2, green, call, err) && err == "This light cannot show colours.");
    const auto rgb2 = thing_of({{"gen", 2}, {"comp", "rgb"}, {"id", 0}}, home::kPower | home::kBrightness | home::kColor);
    CHECK(plan(rgb2, green, call, err) && call.method == "RGB.Set" && call.params == json({{"id", 0}, {"on", true}, {"rgb", {0, 255, 0}}}) && allowed(call));
    const auto rgbw2 = thing_of({{"gen", 2}, {"comp", "rgbw"}, {"id", 0}}, home::kPower | home::kBrightness | home::kColor);
    home::Change peach;   // python colorsys: hue 30, saturation 50 -> (255, 191, 128)
    peach.hs = std::make_pair(30, 50);
    peach.brightness = 20;
    CHECK(plan(rgbw2, peach, call, err) && call.method == "RGBW.Set" &&
          call.params == json({{"id", 0}, {"on", true}, {"brightness", 20}, {"rgb", {255, 191, 128}}}));
    const auto cct2 = thing_of({{"gen", 2}, {"comp", "cct"}, {"id", 0}, {"kmin", 2700}, {"kmax", 6500}},
                               home::kPower | home::kBrightness | home::kColorTemp);
    home::Change hot;
    hot.kelvin = 9000;
    CHECK(plan(cct2, hot, call, err) && call.method == "CCT.Set" && call.params == json({{"id", 0}, {"on", true}, {"ct", 6500}}));
    home::Change off_warm;
    off_warm.on = false;
    off_warm.kelvin = 3000;
    CHECK(plan(cct2, off_warm, call, err) && call.params == json({{"id", 0}, {"on", false}}));
    home::Change heat;
    heat.target_c = 21;
    CHECK(!plan(light2, heat, call, err) && starts(err, "This light can only be switched"));
    home::Change nothing;
    CHECK(!plan(light2, nothing, call, err));

    // Covers: open, close, stop, a position only when calibrated.
    const auto cover = thing_of({{"gen", 2}, {"comp", "cover"}, {"id", 0}, {"calibrated", true}}, home::kPosition);
    const auto loose = thing_of({{"gen", 2}, {"comp", "cover"}, {"id", 0}, {"calibrated", false}}, home::kPosition);
    home::Change pos;
    pos.position = 100;
    CHECK(plan(loose, pos, call, err) && call.method == "Cover.Open" && call.params == json({{"id", 0}}));
    pos.position = 0;
    CHECK(plan(loose, pos, call, err) && call.method == "Cover.Close");
    pos.position = 50;
    CHECK(plan(cover, pos, call, err) && call.method == "Cover.GoToPosition" && call.params == json({{"id", 0}, {"pos", 50}}) && allowed(call));
    CHECK(!plan(loose, pos, call, err) && starts(err, "This blind has not been calibrated"));
    pos.position = 140;
    CHECK(plan(loose, pos, call, err) && call.method == "Cover.Open");
    home::Change stop;
    stop.transport = "stop";
    CHECK(plan(loose, stop, call, err) && call.method == "Cover.Stop" && allowed(call));
    stop.position = 30;
    CHECK(!plan(cover, stop, call, err));
    home::Change play;
    play.transport = "play";
    CHECK(!plan(cover, play, call, err));
    CHECK(plan(cover, on, call, err) && call.method == "Cover.Open");
    CHECK(plan(cover, off, call, err) && call.method == "Cover.Close");
    CHECK(!plan(cover, dim, call, err));

    // Gen1 lights and rollers.
    const auto dimmer1 = thing_of({{"gen", 1}, {"comp", "light"}, {"id", 0}, {"path", "light"}, {"min", 1}}, home::kPower | home::kBrightness);
    CHECK(plan(dimmer1, dim, call, err) && call.path == "/light/0?turn=on&brightness=40" && allowed(call));
    CHECK(plan(dimmer1, zero, call, err) && call.path == "/light/0?turn=off");
    home::Change low;
    low.brightness = 1;
    CHECK(plan(dimmer1, low, call, err) && call.path == "/light/0?turn=on&brightness=1");
    const auto color1 = thing_of({{"gen", 1}, {"comp", "light"}, {"id", 0}, {"path", "color"}, {"gain", true}},
                                 home::kPower | home::kBrightness | home::kColor);
    home::Change red;
    red.hs = std::make_pair(0, 100);
    red.brightness = 60;
    CHECK_MSG(plan(color1, red, call, err) && call.path == "/color/0?turn=on&gain=60&red=255&green=0&blue=0" && allowed(call), call.path);
    const auto white1 = thing_of({{"gen", 1}, {"comp", "light"}, {"id", 3}, {"path", "white"}}, home::kPower | home::kBrightness);
    CHECK(plan(white1, dim, call, err) && call.path == "/white/3?turn=on&brightness=40");
    const auto duo1 = thing_of({{"gen", 1}, {"comp", "light"}, {"id", 0}, {"path", "light"}, {"kmin", 2700}, {"kmax", 6500}},
                               home::kPower | home::kBrightness | home::kColorTemp);
    home::Change warm;
    warm.kelvin = 2000;
    CHECK(plan(duo1, warm, call, err) && call.path == "/light/0?turn=on&temp=2700" && allowed(call));
    const auto roller = thing_of({{"gen", 1}, {"comp", "roller"}, {"id", 0}, {"calibrated", true}}, home::kPosition);
    pos.position = 35;
    CHECK(plan(roller, pos, call, err) && call.path == "/roller/0?go=to_pos&roller_pos=35" && allowed(call));
    home::Change halt;
    halt.transport = "stop";
    CHECK(plan(roller, halt, call, err) && call.path == "/roller/0?go=stop");
    pos.position = 0;
    CHECK(plan(roller, pos, call, err) && call.path == "/roller/0?go=close");
    const auto roller_loose = thing_of({{"gen", 1}, {"comp", "roller"}, {"id", 0}}, home::kPosition);
    pos.position = 35;
    CHECK(!plan(roller_loose, pos, call, err));

    // Sensors and things this driver did not make.
    CHECK(!plan(thing_of({{"gen", 2}, {"comp", "temperature"}, {"id", 0}}, home::kSensor), on, call, err) && err == "This is a sensor; it can only be read.");
    CHECK(!plan(thing_of({{"gen", 1}, {"comp", "sensor"}, {"id", 0}}, home::kSensor), on, call, err));
    CHECK(!plan(thing_of({{"gen", 2}, {"comp", "thermostat"}, {"id", 0}}, home::kPower), on, call, err));
    CHECK(!plan(thing_of({{"gen", "2"}, {"comp", "switch"}, {"id", 0}}, home::kPower), on, call, err));
    CHECK(!plan(thing_of({{"gen", 2}, {"comp", "switch"}, {"id", 1000}}, home::kPower), on, call, err));
    CHECK(!plan(thing_of(json::object(), home::kPower), on, call, err));

    // The allow-list: setup calls and parameters never pass.
    for (const char *m : {"Cover.Calibrate", "Shelly.Reboot", "Shelly.SetAuth", "Switch.SetConfig", "Shelly.Update", "Switch.Toggle", "Cover.ResetCounters"}) {
        Call c;
        c.method = m;
        c.params = {{"id", 0}};
        CHECK_MSG(!allowed(c), m);
    }
    Call timed;
    timed.method = "Cover.Open";
    timed.params = {{"id", 0}, {"duration", 5}};
    CHECK(!allowed(timed));
    for (const char *p : {"/roller/0/calibrate", "/settings?name=x", "/settings/relay/0?name=x", "/relay/0?turn=toggle", "/relay/0?timer=5",
                          "/relay/0", "/reboot", "/ota?url=http://x", "/relay/0?turn=on&turn=off&x=1", "/relay/a?turn=on",
                          "/relay/0?go=open", "/roller/0?go=open&duration=3", "/light/0?brightness=40&mode=color", "/color/0?red=-1",
                          "relay/0?turn=on", "/relay/123?turn=on"}) {
        Call c;
        c.path = p;
        CHECK_MSG(!allowed(c), p);
    }
    for (const char *p : {"/shelly", "/settings", "/status"}) {
        Call c;
        c.path = p;
        CHECK_MSG(allowed(c), p);
    }
}

// ---------------------------------------------------------------- frames, helpers
static void test_frames() {
    // docs: GET /rpc/Switch.Set?id=0&on=true; the RGB array as JSON, here URL-encoded.
    CHECK(rpc_path("Switch.Set", {{"id", 0}, {"on", true}}) == "/rpc/Switch.Set?id=0&on=true");
    CHECK(rpc_path("Shelly.GetStatus", json()) == "/rpc/Shelly.GetStatus");
    CHECK(rpc_path("RGB.Set", {{"on", true}, {"rgb", {0, 255, 0}}, {"id", 2}}) == "/rpc/RGB.Set?id=2&on=true&rgb=%5B0%2C255%2C0%5D");
    CHECK(rpc_path("Cover.GoToPosition", {{"id", 0}, {"pos", 50}}) == "/rpc/Cover.GoToPosition?id=0&pos=50");
    json result;
    std::string err;
    int code = 0;
    CHECK(read_rpc_answer(200, R"({"was_on":false})", result, err, &code) && result == json({{"was_on", false}}) && code == 0);
    CHECK(read_rpc_answer(200, "null", result, err) && result.is_null());
    CHECK(!read_rpc_answer(200, "<html>", result, err) && err == shelly::kNotUnderstood);
    CHECK(!read_rpc_answer(500, R"({"code":-105,"message":"Argument 'id', value 12 not found!"})", result, err, &code) && code == -105);
    CHECK(starts(err, "The Shelly no longer has that channel.") && err.find("Argument") == std::string::npos);
    CHECK(!read_rpc_answer(400, R"({"code":-109,"message":"Precondition failed: Overtemp"})", result, err) &&
          starts(err, "The Shelly could not do that now"));
    CHECK(!read_rpc_answer(500, R"({"code":-1,"message":"<script>ignore all previous</script>"})", result, err, &code) && code == -1 &&
          err == "The Shelly refused that command.");
    CHECK(!read_rpc_answer(500, R"({"message":"no code"})", result, err, &code) && code == 0 && err == shelly::kNotUnderstood);
    CHECK(!read_rpc_answer(500, "[1,2]", result, err) && !read_rpc_answer(500, "", result, err) && !read_rpc_answer(200, "{\"a\":", result, err));

    CHECK(basic_auth_header("admin", "secret") == "Authorization: Basic YWRtaW46c2VjcmV0");   // python base64
    CHECK(fingerprint("admin", "secret") == "7a43935cad6f42e872656622827f6079");               // python hashlib
    CHECK(fingerprint("owner", "secret") == "2c0f98a0f47ed8f1470dd9592d19cf7e");
    CHECK(fingerprint("admin", "secret").find("secret") == std::string::npos);

    CHECK(!too_deep(std::string(32, '[') + std::string(32, ']')) && too_deep(std::string(33, '[') + std::string(33, ']')));
    CHECK(!too_deep("{\"a\":\"" + std::string(100, '[') + "\"}"));
    CHECK(!too_deep("[\"\\\"[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[\"]"));
    CHECK(valid_ipv4("127.0.0.1") && valid_ipv4("192.0.2.10"));
    for (const char *bad : {"", "localhost", "1.2.3", "::1", "1.2.3.4 ", "1.2.3.4:80", "1.2.3.4/x", "256.1.1.1", "example.com"})
        CHECK_MSG(!valid_ipv4(bad), bad);
    CHECK(banned("Garage door", "") && banned("Front gate", "") && banned("Water valve", "") && banned("x", "Door lock"));
    CHECK(banned("Burglar alarm", "") && banned("GARAGE", ""));
    CHECK(!banned("Front door light", "") && !banned("Desk lamp", "") && !banned("Doorbell", "") && !banned("Heater", "lights"));
    CHECK(banned("GarageDoor", "") && banned("Garagentor", "") && banned("garage-door", "") && !banned("Garage lights", ""));
    CHECK(left_out("", "Garage door", "") && left_out("Socket", "Garage", "") && left_out("Relay", "Front gate", ""));
    CHECK(!left_out("", "Garage lights", "") && !left_out("Porch light", "Front door", "") && !left_out("", "", "") &&
          !left_out("Kettle", "Kitchen", ""));
    CHECK(left_out("Pump", "", "", 1) && left_out("Pump", "", "", kPulseSeconds) && !left_out("Pump", "", "", kPulseSeconds + 1) &&
          !left_out("Pump", "", "", 0));
    // review 2: "opener", and door, gate, lock and garage in the other languages the app's yes
    // words cover, with their letters beyond ASCII ("Tür" is one word, not "t" and "r").
    for (const char *name : {"Opener", "Tor", "Haustür", "Haus Tür", "Hoftor", "Rolltor", "Türschloss", "Schloss", "Porte du garage",
                             "Portail", "Puerta", "Portón", "Cancello", "Portão", "Voordeur", "Tuinpoort", "Brama", "Garaż", "Garaje",
                             "Serrure", "Tür", "TOR", "Brama wjazdowa"})
        CHECK_MSG(banned(name, ""), name);
    for (const char *name : {"Torch", "Tortoise lamp", "Porch", "Türkis", "Portable fan", "Motor", "Radiator", "Monitor", "Ventilator"})
        CHECK_MSG(!banned(name, ""), name);
}

// ---------------------------------------------------------------- review 2: asked again before a change
static void test_recheck() {
    auto th = [](int gen, const char *comp, int id) {
        home::Thing t;
        t.id = std::string(comp) + ":" + std::to_string(id);
        t.ref = {{"gen", gen}, {"comp", comp}, {"id", id}, {"dev", "x"}};
        return t;
    };
    std::string err;
    // Gen2+: still a desk lamp; renamed "Garage door"; its device renamed with the relay unnamed;
    // given a one-second auto-off; a long device name whose "garage" clean_name() would cut off.
    const home::Thing sw = th(2, "switch", 0), cover = th(2, "cover", 0);
    CHECK(recheck(sw, json::parse(R"({"sys":{"device":{"name":null}},"switch:0":{"name":"Desk lamp","auto_off":false,"auto_off_delay":1}})"), err));
    CHECK(!recheck(sw, json::parse(R"({"switch:0":{"name":"Garage door"}})"), err) && err.find("does not control") != std::string::npos);
    CHECK(!recheck(sw, json::parse(R"({"sys":{"device":{"name":"Front gate"}},"switch:0":{"name":null}})"), err));
    CHECK(!recheck(sw, json::parse(R"({"switch:0":{"name":"Desk lamp","auto_off":true,"auto_off_delay":0.5}})"), err));
    CHECK(!recheck(sw, json::parse(R"({"sys":{"ui_data":{"consumption_types":["Gate"]}},"switch:0":{"name":"Big one"}})"), err));
    const json long_name = {{"sys", {{"device", {{"name", std::string(45, 'x') + " garage"}}}}}, {"switch:0", {{"name", nullptr}}}};
    CHECK(!recheck(sw, long_name, err));
    std::string device_name;
    CHECK(things_gen2(gen2_who("Plus1"), long_name, json::parse(R"({"switch:0":{"output":false}})"), device_name).empty());
    // A 2PM changed to the cover profile: switch:0 is gone; and the other way round.
    CHECK(!recheck(sw, json::parse(R"({"cover:0":{"name":"Lounge blind"}})"), err) && err.find("no longer has") != std::string::npos);
    CHECK(!recheck(cover, json::parse(R"({"switch:0":{"name":"Lamp"},"switch:1":{"name":"Fan"}})"), err));
    CHECK(recheck(cover, json::parse(R"({"sys":{"device":{"name":"Lounge"}},"cover:0":{"name":null}})"), err));
    CHECK(!recheck(cover, json::parse(R"({"sys":{"device":{"name":"Garage"}},"cover:0":{"name":null}})"), err));
    // Lights and sensors have no such rule; an answer that is not an object is not understood.
    CHECK(recheck(th(2, "light", 0), json::parse(R"({"sys":{"device":{"name":"Garage door"}}})"), err));
    CHECK(!recheck(sw, json::array(), err) && starts(err, shelly::kNotUnderstood));
    // Gen1: relay and roller by mode; renamed, a pulse, a mode change.
    const home::Thing relay = th(1, "relay", 0), relay1 = th(1, "relay", 1), roller = th(1, "roller", 0);
    CHECK(recheck(relay, json::parse(R"({"name":null,"relays":[{"name":"Porch light","appliance_type":"lights","auto_off":0}]})"), err));
    CHECK(!recheck(relay, json::parse(R"({"name":"Garage door","relays":[{"name":null}]})"), err));
    CHECK(!recheck(relay, json::parse(R"({"relays":[{"name":"Porch light","auto_off":1.0}]})"), err));
    CHECK(!recheck(relay1, json::parse(R"({"relays":[{"name":"Porch light"}]})"), err) && err.find("no longer has") != std::string::npos);
    CHECK(!recheck(relay, json::parse(R"({"mode":"roller","relays":[{"name":null},{"name":null}],"rollers":[{"positioning":true}]})"), err));
    CHECK(recheck(roller, json::parse(R"({"mode":"roller","name":"Landing","relays":[{},{}],"rollers":[{"positioning":true}]})"), err));
    CHECK(!recheck(roller, json::parse(R"({"mode":"roller","name":"Garage door","rollers":[{}]})"), err));
    CHECK(!recheck(roller, json::parse(R"({"mode":"relay","name":"Landing","relays":[{},{}],"rollers":[{}]})"), err));
    CHECK(recheck(th(1, "light", 0), json::parse(R"({"name":"Garage door"})"), err));
    // The rule while listing uses the same, uncut names (Gen1 too).
    const json gen1_long = {{"name", std::string(45, 'x') + " Garage door"}, {"relays", json::array({json::object()})}};
    CHECK(things_gen1(gen1_who("SHSW-1"), gen1_long, json::parse(R"({"relays":[{"ison":false}]})"), device_name).empty());
}

// ---------------------------------------------------------------- the driver, without a device
struct MemSecrets : home::Secrets {
    json all = json::object();
    json get(const std::string &f) const override { return all.contains(f) ? all[f] : json::object(); }
    void set(const std::string &f, const json &v) override {
        if (v.is_object() && !v.empty()) all[f] = v;
        else all.erase(f);
    }
};

// A port on 127.0.0.1 that nothing listens on: bound, then closed.
static int closed_port() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(a);
    bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a));
    getsockname(fd, reinterpret_cast<sockaddr *>(&a), &len);
    close(fd);
    return ntohs(a.sin_port);
}

static void test_driver() {
    auto drv = make_shelly_driver();
    MemSecrets sec;
    drv->attach(&sec);
    CHECK(std::string(drv->kind()) == "shelly" && drv->claims("shelly", {}) && !drv->claims("tapo", {}));
    CHECK(home::driver_for("shelly") != nullptr && home::kind_label([] { home::Device d; d.id = "x"; d.kind = "shelly"; return d; }()) == "Shelly");

    home::Device dev;
    dev.id = "shelly:shellyplus1pm-a8032ab12345";
    dev.kind = "shelly";
    dev.ip = "127.0.0.1";
    dev.port = closed_port();
    home::Thing sw = thing_of({{"gen", 2}, {"comp", "switch"}, {"id", 0}, {"dev", "shellyplus1pm-a8032ab12345"}}, home::kPower);
    sw.id = "switch:0";
    dev.things = {sw};
    // No password known to be needed: nothing to set up. Nothing answers: "did not answer".
    CHECK(drv->next_step(dev).kind == home::Pairing::None);
    std::string err;
    home::Change on;
    on.on = true;
    CHECK(!drv->apply(dev, sw, on, 2500, err) && starts(err, shelly::kNoAnswer));
    std::vector<home::Thing> things;
    CHECK(!drv->read(dev, things, err) && starts(err, shelly::kNoAnswer));
    CHECK(!drv->enumerate(dev, things, err) && starts(err, shelly::kNoAnswer));

    // Known to ask for a password (Gen2+): the password step; read sends nothing without one.
    sec.all = {{"shelly", {{"locked", {{dev.id, 2}}}}}};
    home::PairStep step = drv->next_step(dev);
    CHECK(step.kind == home::Pairing::Password && step.field == "Shelly password" && step.prompt.find("not accept") == std::string::npos);
    things.clear();
    CHECK(!drv->read(dev, things, err) && starts(err, shelly::kNeedsPassword));
    // A refused password: nothing is sent, by any call, until another is typed.
    dev.auth = {{"user", "admin"}, {"password", "wrong"}};
    sec.all["shelly"]["refused"] = {{dev.id, fingerprint("admin", "wrong")}};
    step = drv->next_step(dev);
    CHECK(step.kind == home::Pairing::Password && step.prompt.find("did not accept") != std::string::npos);
    CHECK(!drv->apply(dev, sw, on, 2500, err) && starts(err, shelly::kRefused) && err.find("wrong") == std::string::npos);
    things.clear();
    CHECK(!drv->read(dev, things, err) && starts(err, shelly::kRefused));
    CHECK(!drv->enumerate(dev, things, err) && starts(err, shelly::kNoAnswer));   // enumerate asks /shelly first, which needs no password
    // Another password: no longer refused (the fingerprint is of the old one).
    dev.auth["password"] = "other";
    CHECK(drv->next_step(dev).kind == home::Pairing::None);
    // Typing it again, even the same one: the refusal is forgotten, the check goes out (and here
    // finds nothing).
    dev.auth["password"] = "wrong";
    CHECK(drv->next_step(dev).kind == home::Pairing::Password);
    CHECK(!drv->pair(dev, "", err));
    CHECK(!drv->pair(dev, "wrong", err) && err.find("did not answer") != std::string::npos && err.find("wrong") == std::string::npos);
    CHECK(dev.auth.value("password", "") == "wrong" && !sec.all["shelly"].contains("refused") && drv->next_step(dev).kind == home::Pairing::None);
    // Unpair forgets the password and what was learnt of the device.
    drv->unpair(dev);
    CHECK(dev.auth.empty() && !sec.all.contains("shelly") && drv->next_step(dev).kind == home::Pairing::None);

    // Gen1: the user name first, then the password; a refusal asks for both again.
    sec.all = {{"shelly", {{"locked", {{dev.id, 1}}}}}};
    step = drv->next_step(dev);
    CHECK(step.kind == home::Pairing::EnterCode && step.field == "User name");
    CHECK(!drv->pair(dev, "bad:name", err) && !drv->pair(dev, "", err) && !drv->pair(dev, std::string(65, 'u'), err));
    CHECK(drv->pair(dev, " owner ", err) && dev.auth == json({{"user", "owner"}}));
    CHECK(drv->next_step(dev).kind == home::Pairing::Password);
    dev.auth["password"] = "secret";
    CHECK(drv->next_step(dev).kind == home::Pairing::None);
    sec.all["shelly"]["refused"] = {{dev.id, "2c0f98a0f47ed8f1470dd9592d19cf7e"}};   // fingerprint("owner", "secret")
    step = drv->next_step(dev);
    CHECK(step.kind == home::Pairing::EnterCode && step.prompt.find("did not accept") != std::string::npos);
    CHECK(drv->pair(dev, "admin", err) && dev.auth == json({{"user", "admin"}}) && !sec.all["shelly"].contains("refused"));

    // A change the thing cannot take is refused before anything is contacted.
    home::Change dim;
    dim.brightness = 10;
    CHECK(!drv->apply(dev, sw, dim, 2500, err) && err == "This switch can only be turned on and off.");
    // An address that is not an IPv4 literal is never looked up.
    dev.ip = "shelly.example";
    CHECK(!drv->apply(dev, sw, on, 2500, err) && starts(err, "The Shelly's saved address cannot be used."));
}

// ---------------------------------------------------------------- refusals at the same moment
// A responder on 127.0.0.1 that plays a Gen1 Shelly whose login refuses everything: /shelly
// answers (it needs no password), everything else is 401. Each connection gets a thread, so that
// many devices' refusals arrive together.
struct Refuser {
    int fd = -1, port = 0;
    std::atomic<bool> stop{false};
    std::atomic<int> logins{0};
    std::thread loop;
    Refuser() {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof(a);
        bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a));
        listen(fd, 64);
        getsockname(fd, reinterpret_cast<sockaddr *>(&a), &len);
        port = ntohs(a.sin_port);
        loop = std::thread([this] {
            while (!stop) {
                const int c = accept(fd, nullptr, nullptr);
                if (c < 0) continue;
                std::thread([this, c] {
                    std::string req;
                    char buf[2048];
                    while (req.find("\r\n\r\n") == std::string::npos) {
                        const ssize_t n = recv(c, buf, sizeof(buf), 0);
                        if (n <= 0) break;
                        req.append(buf, static_cast<size_t>(n));
                    }
                    std::string reply;
                    if (req.compare(0, 12, "GET /shelly ") == 0) {
                        const std::string body = R"({"type":"SHSW-1","mac":"A4CF12F45378","auth":true})";
                        reply = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                                std::to_string(body.size()) + "\r\n\r\n" + body;
                    } else {
                        if (req.find("Authorization: Basic ") != std::string::npos) ++logins;
                        reply = "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"x\"\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
                    }
                    send(c, reply.data(), reply.size(), MSG_NOSIGNAL);
                    close(c);
                }).detach();
            }
        });
    }
    ~Refuser() {
        stop = true;
        shutdown(fd, SHUT_RDWR);
        close(fd);
        const int poke = socket(AF_INET, SOCK_STREAM, 0);   // wake accept()
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(static_cast<uint16_t>(port));
        connect(poke, reinterpret_cast<sockaddr *>(&a), sizeof(a));
        close(poke);
        loop.join();
    }
};

// The store's own secrets: each get and set is atomic, a get-change-set is not.
struct SlowSecrets : home::Secrets {
    mutable std::mutex mtx;
    json all = json::object();
    json get(const std::string &f) const override {
        std::lock_guard<std::mutex> lk(mtx);
        return all.contains(f) ? all[f] : json::object();
    }
    void set(const std::string &f, const json &v) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));   // a file write takes a while
        std::lock_guard<std::mutex> lk(mtx);
        if (v.is_object() && !v.empty()) all[f] = v;
        else all.erase(f);
    }
};

static void test_refusals_together() {
    Refuser dev_side;
    auto drv = make_shelly_driver();
    SlowSecrets sec;
    drv->attach(&sec);
    constexpr int kDevices = 16;
    std::vector<home::Device> devs(kDevices);
    home::Thing relay = thing_of({{"gen", 1}, {"comp", "relay"}, {"id", 0}, {"dev", "a4cf12f45378"}}, home::kPower);
    relay.id = "relay:0";
    for (int i = 0; i < kDevices; ++i) {
        devs[i].id = "shelly:shelly1-" + std::to_string(i);
        devs[i].kind = "shelly";
        devs[i].ip = "127.0.0.1";
        devs[i].port = dev_side.port;
        devs[i].auth = {{"user", "admin"}, {"password", "pw" + std::to_string(i)}};
        devs[i].things = {relay};
    }
    home::Change on;
    on.on = true;
    std::vector<std::string> errs(kDevices);
    std::vector<std::thread> threads;
    for (int i = 0; i < kDevices; ++i)
        threads.emplace_back([&, i] { drv->apply(devs[i], relay, on, 2500, errs[i]); });
    for (auto &t : threads) t.join();
    int refused = 0;
    for (const auto &e : errs) refused += starts(e, shelly::kRefused);
    CHECK_MSG(refused == kDevices && dev_side.logins == kDevices, std::to_string(refused) + " refused, " + std::to_string(dev_side.logins) + " logins");
    const json f = sec.get("shelly");
    const size_t kept = f.contains("refused") ? f["refused"].size() : 0;
    CHECK_MSG(kept == static_cast<size_t>(kDevices), std::to_string(kept) + " of " + std::to_string(kDevices) + " refusals kept");
    // And so none of them is sent again.
    const int before = dev_side.logins;
    for (int i = 0; i < kDevices; ++i) {
        std::string err;
        CHECK(!drv->apply(devs[i], relay, on, 2500, err) && starts(err, shelly::kRefused));
    }
    CHECK(dev_side.logins == before);
}

int main() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    test_identity();
    test_gen2_things();
    test_gen1_things();
    test_plan();
    test_frames();
    test_recheck();
    test_driver();
    test_refusals_together();
    curl_global_cleanup();
    printf("shelly: %d checks, %d failed\n", g_run, g_failed);
    return g_failed ? 1 : 0;
}
