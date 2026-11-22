// Known-answer and unit tests for the Kasa driver (drivers/kasa.cpp). Only 127.0.0.1 is used.
// Each check prints one line; the exit status is non-zero when any fails.
//
// Where the vectors come from:
//   survey   the project's research notes: {"system":{"get_sysinfo":{}}} scrambled, computed there
//   mock     computed with tests/mocks/kasa/mock_kasa.py's scramble(), a Python implementation
//            sharing no code with the driver
// The get_sysinfo samples below are written for this test in the shapes real devices report
// (keys, types and units), with made-up names, addresses and ids.
#include "smarthome/drivers/kasa.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>

#include "core/util.h"
#include "smarthome/drivers/tapo_driver.h"

using namespace kasa::detail;
using home::json;

static int g_failed = 0, g_run = 0;

static void check(const std::string &name, bool ok, const std::string &detail = "") {
    g_run++;
    if (!ok) g_failed++;
    printf("%s  %s%s\n", ok ? "ok  " : "FAIL", name.c_str(), ok || detail.empty() ? "" : ("  -> " + detail).c_str());
}

static std::string hex(const std::string &b) {
    static const char *d = "0123456789abcdef";
    std::string out;
    for (const unsigned char c : b) {
        out += d[c >> 4];
        out += d[c & 15];
    }
    return out;
}

static bool has(const std::string &s, const std::string &part) { return s.find(part) != std::string::npos; }

static home::Thing first(const std::vector<home::Thing> &things) { return things.empty() ? home::Thing() : things[0]; }

// ---------------------------------------------------------------- samples
const char *kPlugV1 = R"J({"sw_ver":"1.2.5 Build 171213 Rel.101523","hw_ver":"1.0","type":"IOT.SMARTPLUGSWITCH",
 "model":"HS110(UK)","mac":"50:C7:BF:00:00:01","dev_name":"Wi-Fi Smart Plug With Energy Monitoring","alias":"Kettle",
 "relay_state":1,"on_time":60,"feature":"TIM:ENE","deviceId":"8006000000000000000000000000000000000001","err_code":0})J";
const char *kPlugV2 = R"J({"hw_ver":"1.0","mic_type":"IOT.SMARTPLUGSWITCH","model":"KP115(UK)","mac":"C0:06:C3:00:00:02",
 "dev_name":"Smart Wi-Fi Plug Mini","alias":"Desk fan","relay_state":0,"feature":"TIM:ENE","err_code":0})J";
const char *kSwitch = R"J({"type":"IOT.SMARTPLUGSWITCH","model":"HS200(UK)","dev_name":"Smart Wi-Fi Light Switch",
 "alias":"Porch light","relay_state":1,"feature":"TIM","err_code":0})J";
const char *kDimmer = R"J({"mic_type":"IOT.SMARTPLUGSWITCH","model":"HS220(US)","dev_name":"Smart Wi-Fi Dimmer",
 "alias":"Hall","relay_state":0,"brightness":25,"feature":"TIM","err_code":0})J";
const char *kStrip = R"J({"mic_type":"IOT.SMARTPLUGSWITCH","model":"HS300(US)","alias":"Desk strip","feature":"TIM:ENE",
 "child_num":3,"children":[
  {"id":"8006000000000000000000000000000000000500","state":1,"alias":"Monitor","on_time":5},
  {"id":"8006000000000000000000000000000000000501","state":0,"alias":"","on_time":0},
  {"id":"8006000000000000000000000000000000000502","state":0,"alias":"Lamp","on_time":0}],"err_code":0})J";
const char *kStripNoMeter = R"J({"mic_type":"IOT.SMARTPLUGSWITCH","model":"KP303(UK)","alias":"TV strip","feature":"TIM",
 "children":[{"id":"00","state":1,"alias":"TV"},{"id":"01","state":0,"alias":"Console"}],"err_code":0})J";
const char *kColourBulb = R"J({"mic_type":"IOT.SMARTBULB","model":"KL130(EU)","alias":"Reading lamp","mic_mac":"1C3BF3000006",
 "is_dimmable":1,"is_color":1,"is_variable_color_temp":1,
 "light_state":{"on_off":1,"mode":"normal","hue":0,"saturation":0,"color_temp":2700,"brightness":80},"err_code":0})J";
const char *kWhiteBulbOff = R"J({"mic_type":"IOT.SMARTBULB","model":"KL50(US)","alias":"Edison","is_dimmable":1,"is_color":0,
 "is_variable_color_temp":0,"light_state":{"on_off":0,"dft_on_state":{"mode":"normal","hue":0,"saturation":0,
 "color_temp":2700,"brightness":12}},"err_code":0})J";
const char *kLightStrip = R"J({"mic_type":"IOT.SMARTBULB","model":"KL430(US)","alias":"Shelf","length":16,"is_dimmable":1,
 "is_color":1,"is_variable_color_temp":1,"light_state":{"on_off":1,"mode":"normal","hue":240,"saturation":100,
 "color_temp":0,"brightness":50},"err_code":0})J";
const char *kCamera = R"J({"type":"IOT.IPCAMERA","model":"KC100(UK)","alias":"Garden","err_code":0})J";
const char *kExtender = R"J({"type":"IOT.RANGEEXTENDER.SMARTPLUG","model":"RE370K(US)","alias":"Hall extender","relay_state":1})J";

static std::vector<home::Thing> things_of(const char *sysinfo, std::string *err_out = nullptr) {
    std::vector<home::Thing> out;
    std::string err;
    things_from(json::parse(sysinfo), out, err);
    if (err_out) *err_out = err;
    return out;
}

// ---------------------------------------------------------------- the scrambling
static void test_scrambling() {
    check("survey: get_sysinfo scrambled", hex(encrypt(R"J({"system":{"get_sysinfo":{}}})J")) ==
                                               "d0f281f88bff9af7d5ef94b6d1b4c09fec95e68fe187e8caf08bf68bf6");
    check("mock: set_relay_state scrambled",
          hex(encrypt(R"J({"system":{"set_relay_state":{"state":1}}})J")) ==
              "d0f281f88bff9af7d5ef94b6c5a0d48bf99cf091e8b7c4b0d1a5c0e2d8a381f286e793f6d4eedfa2dfa2");
    check("mock: a strip socket's command scrambled",
          hex(encrypt(R"J({"context":{"child_ids":["8006A1B2C3D4E5F60718293A4B5C6D7E8F90A1B201"]},"system":{"set_relay_state":{"state":0}}})J")) ==
              "d0f291fe90e481f98daf95eeccafc7aec2a6f990f487a59fc4e6deeedee8a998dae8ab98dce8ad98dee8d8efdee6d4edde9fabe9dc9fa9edda9fa7e1d8e8"
              "a998dae8d8e9cb96ebc7e596ef9ce88de0c2f883a1d2b7c39cee8be786ffa0d3a7c6b2d7f5cfb496e591f084e1c3f9c9b4c9b4");
    std::string reply;
    for (size_t i = 0; i < 90; i += 2)
        reply += static_cast<char>(std::stoi(std::string("d0f281f88bff9af7d5ef94b6c5a0d48bf99cf091e8b7c4b0d1a5c0e2d8a381e496e4bbd8b7d3b694ae9ee39ee3").substr(i, 2),
                                             nullptr, 16));
    check("mock: a reply unscrambled", decrypt(reply) == R"J({"system":{"set_relay_state":{"err_code":0}}})J");
    std::string all;
    for (int i = 0; i < 256; ++i) all += static_cast<char>(i);
    check("every byte value survives the round trip", decrypt(encrypt(all + all)) == all + all);
    check("empty in, empty out", encrypt("").empty() && decrypt("").empty());
    const std::string f = frame(R"J({"system":{"get_sysinfo":{}}})J");
    check("frame: four-byte big-endian length, then the scrambled text",
          f.size() == 33 && hex(f.substr(0, 4)) == "0000001d" && f.substr(4) == encrypt(R"J({"system":{"get_sysinfo":{}}})J"));
    check("frame: lengths above 255 use the high bytes", hex(frame(std::string(70000, 'x')).substr(0, 4)) == "00011170");
}

// ---------------------------------------------------------------- parsing what devices send
static void test_parsing() {
    json j;
    check("a reply object parses", parse_reply(R"J({"system":{"get_sysinfo":{"alias":"x"}}})J", j) && j.is_object());
    check("an array is not a reply", !parse_reply("[1,2,3]", j));
    check("text that is not JSON is not a reply", !parse_reply("\x01\x02garbage", j));
    check("nesting deeper than 32 is refused unparsed", too_deep(std::string(33, '[') + std::string(33, ']')) &&
                                                            !parse_reply("{\"a\":" + std::string(40, '[') + std::string(40, ']') + "}", j));
    check("brackets inside strings do not count", !too_deep("{\"a\":\"" + std::string(100, '[') + "\\\"" + std::string(100, '{') + "\"}"));
    check("a reply longer than the cap is refused", !parse_reply("{\"a\":\"" + std::string(kMaxReply, 'x') + "\"}", j));

    check("switched types: plugs and bulbs", switched_type("IOT.SMARTPLUGSWITCH") && switched_type(" iot.smartbulb "));
    check("not switched: cameras, extenders, Tapo types, nothing", !switched_type("IOT.IPCAMERA") &&
                                                                  !switched_type("IOT.RANGEEXTENDER.SMARTPLUG") &&
                                                                  !switched_type("SMART.TAPOPLUG") && !switched_type(""));
    check("scheme KLAP needs the newer login", needs_new_login({{"scheme", "KLAP"}}) && needs_new_login({{"scheme", "aes"}}));
    check("scheme XOR, or none, is the older protocol", !needs_new_login({{"scheme", "XOR"}}) && !needs_new_login({}) &&
                                                          !needs_new_login({{"scheme", ""}}));
    const std::string moved = R"J({"error_code":0,"result":{"device_id":"0","device_model":"HS100(UK)","device_type":"IOT.SMARTPLUGSWITCH",
      "mac":"CC-32-E5-00-00-0A","mgt_encrypt_schm":{"encrypt_type":"KLAP","http_port":80,"is_support_https":false}}})J";
    check("a 20002 reply from a Kasa device on KLAP says the newer login", reply_says_new_login(moved));
    std::string mac = "stale";
    check("... and gives the MAC it names, to be compared with the saved one", reply_says_new_login(moved, &mac) && mac == "cc32e500000a",
          mac);
    check("no MAC from a reply that does not say so", !reply_says_new_login("not json", &mac) && mac.empty());
    std::string tapo = moved;
    tapo.replace(tapo.find("IOT.SMARTPLUGSWITCH"), 19, "SMART.TAPOPLUG");
    check("a Tapo device's 20002 reply does not (it is the Tapo driver's)", !reply_says_new_login(tapo));
    std::string xor_reply = moved;
    xor_reply.replace(xor_reply.find("KLAP"), 4, "XOR");
    check("an IOT reply naming the older scheme does not", !reply_says_new_login(xor_reply));
    check("garbage and deep nesting do not", !reply_says_new_login("not json") &&
                                                 !reply_says_new_login("{\"result\":" + std::string(50, '[') + "}"));
}

// ---------------------------------------------------------------- things
static void test_things() {
    std::string err;
    auto t = things_of(kPlugV1);
    check("HS110: one metered plug, on", t.size() == 1 && t[0].id == "main" && t[0].type == "plug" && t[0].name == "Kettle" &&
                                             t[0].caps == (home::kPower | home::kSensor) && t[0].state == json{{"on", true}},
          t.empty() ? "" : t[0].state.dump());
    t = things_of(kPlugV2);
    check("KP115 (mic_type): metered plug, off", t.size() == 1 && t[0].caps == (home::kPower | home::kSensor) &&
                                                     t[0].state == json{{"on", false}});
    t = things_of(kSwitch);
    check("HS200: a wall switch, not metered", t.size() == 1 && t[0].type == "switch" && t[0].caps == home::kPower &&
                                                   t[0].state["on"] == true);
    t = things_of(kDimmer);
    check("HS220: a dimmable light with its brightness",
          t.size() == 1 && t[0].type == "light" && t[0].caps == (home::kPower | home::kBrightness) &&
              t[0].state == json({{"on", false}, {"brightness", 25}}) && t[0].ref.value("dimmer", false),
          t.empty() ? "" : t[0].state.dump());
    t = things_of(kStrip);
    check("HS300: one metered plug per socket, ids as given",
          t.size() == 3 && t[0].id == "8006000000000000000000000000000000000500" && t[0].name == "Monitor" &&
              t[0].ref == json{{"child", t[0].id}} && t[0].caps == (home::kPower | home::kSensor) && t[0].state["on"] == true &&
              t[2].state["on"] == false);
    check("a socket with no name is called by its position", t.size() == 3 && t[1].name == "Socket 2");
    t = things_of(kStripNoMeter);
    check("KP303: sockets without meters; short ids are kept", t.size() == 2 && t[0].id == "00" && t[0].caps == home::kPower &&
                                                                   t[1].name == "Console");
    t = things_of(kColourBulb);
    check("KL130: a colour light with white range, state in kelvin",
          t.size() == 1 && t[0].type == "light" &&
              t[0].caps == (home::kPower | home::kBrightness | home::kColorTemp | home::kColor) &&
              t[0].state == json({{"on", true}, {"brightness", 80}, {"kelvin", 2700}}) &&
              t[0].ref == json({{"light", "bulb"}, {"kelvin_min", 2500}, {"kelvin_max", 9000}}),
          t.empty() ? "" : t[0].state.dump() + " " + t[0].ref.dump());
    t = things_of(kWhiteBulbOff);
    check("KL50: dimmable only; off shows only off", t.size() == 1 && t[0].caps == (home::kPower | home::kBrightness) &&
                                                       t[0].state == json{{"on", false}});
    t = things_of(kLightStrip);
    check("KL430: a light strip, in colour (color_temp 0 means hue and saturation)",
          t.size() == 1 && t[0].ref.value("light", "") == "strip" &&
              t[0].state == json({{"on", true}, {"brightness", 50}, {"hue", 240}, {"saturation", 100}}),
          t.empty() ? "" : t[0].state.dump());
    t = things_of(kCamera, &err);
    check("a camera gives no thing, and says so", t.empty() && err == "Zero Bot cannot control this kind of Kasa device");
    t = things_of(kExtender, &err);
    check("a range extender gives no thing", t.empty() && has(err, "cannot control"));
    t = things_of(R"J({"alias":"x","relay_state":1})J", &err);
    check("no type at all gives no thing", t.empty() && !err.empty());

    // Hostile values.
    json hostile = json::parse(kStrip);
    hostile["alias"] = "\x07Strip\n\"}{<b>`" + std::string(300, 'x');
    hostile["children"][0]["alias"] = std::string("Mon\0itor \"one\"", 14) + std::string(200, 'y');
    hostile["children"][1]["id"] = "bad\"id";
    hostile["children"][2]["id"] = std::string(200, 'z');
    hostile["children"].push_back({{"id", 12345}, {"state", 1}});
    hostile["children"].push_back({{"id", "8006000000000000000000000000000000000500"}, {"state", 0}, {"alias", "Duplicate"}});
    hostile["children"].push_back("not an object");
    std::vector<home::Thing> out;
    things_from(hostile, out, err);
    check("hostile sockets: bad, long, non-string and repeated ids are left out", out.size() == 1 && out[0].name.size() <= 40,
          std::to_string(out.size()));
    check("hostile names are cleaned and capped", !out.empty() && !has(out[0].name, "\"") && !has(out[0].name, std::string(1, '\0')) &&
                                                     out[0].name.compare(0, 3, "Mon") == 0,
          out.empty() ? "" : out[0].name);
    json many = json::parse(kStripNoMeter);
    many["children"] = json::array();
    for (int i = 0; i < 1000; ++i) many["children"].push_back({{"id", "c" + std::to_string(i)}, {"state", 0}});
    things_from(many, out, err);
    check("at most 16 sockets are kept", out.size() == 16);
    json all_bad = json::parse(kStripNoMeter);
    all_bad["children"] = json::array({{{"id", ""}}, {{"id", "a b"}}});
    check("a strip with no usable socket is an error", !things_from(all_bad, out, err) && out.empty() && has(err, "understood"));
    // A strip is told by "children" even when the list is empty or not a list: read as one plug,
    // its "on" would go without a socket (the whole strip).
    json no_sockets = json::parse(kStripNoMeter);
    no_sockets["children"] = json::array();
    check("a strip listing no sockets is an error, not one plug", !things_from(no_sockets, out, err) && out.empty() &&
                                                                      has(err, "understood"));
    no_sockets["children"] = "none";
    check("a strip whose sockets are not a list is an error", !things_from(no_sockets, out, err) && out.empty());
    json dim = json::parse(kDimmer);
    dim["brightness"] = 9999;
    dim["relay_state"] = 1;
    things_from(dim, out, err);
    check("brightness out of range is clamped", first(out).state.value("brightness", -1) == 100);
    dim["relay_state"] = "1";
    things_from(dim, out, err);
    check("a relay state that is not a number reads as off", first(out).state.value("on", true) == false);
    json bulb = json::parse(kColourBulb);
    bulb["light_state"] = {{"on_off", 1}, {"brightness", -40}, {"hue", "red"}, {"saturation", 1e300}, {"color_temp", 0}};
    things_from(bulb, out, err);
    check("nonsense light values are left out", first(out).state == json{{"on", true}}, first(out).state.dump());
    check("a name that is only control characters falls back", first(things_of(R"J({"type":"IOT.SMARTPLUGSWITCH","alias":"\u0001\u0002"})J")).name == "Kasa plug");

    // Refreshing a thing's state.
    home::Thing sock = things_of(kStrip)[2];
    json now = json::parse(kStrip);
    now["children"][2]["state"] = 1;
    state_from(now, sock);
    check("state_from: a socket follows its own entry", sock.state["on"] == true);
    now["children"].erase(2);
    state_from(now, sock);
    check("state_from: a socket no longer listed keeps its last state", sock.state["on"] == true);
}

// ---------------------------------------------------------------- energy
static void test_energy() {
    json s = json::object();
    energy_into(json::parse(R"J({"current":0.4,"voltage":240.1,"power":95.54,"total":12.3,"err_code":0})J"),
                json::parse(R"J({"day_list":[{"year":2026,"month":11,"day":17,"energy":0.25},{"year":2026,"month":11,"day":18,"energy":0.3456}],"err_code":0})J"),
                18, s);
    check("older hardware: watts and kilowatt-hours", s == json({{"power_w", 95.5}, {"energy_today_wh", 346}}), s.dump());
    s = json::object();
    energy_into(json::parse(R"J({"current_ma":296,"voltage_mv":230577,"power_mw":63499,"total_wh":12068,"err_code":0})J"),
                json::parse(R"J({"day_list":[{"year":2026,"month":11,"day":18,"energy_wh":512}],"err_code":0})J"), 18, s);
    check("newer hardware: milliwatts and watt-hours", s == json({{"power_w", 63.5}, {"energy_today_wh", 512}}), s.dump());
    s = json::object();
    energy_into(json::parse(R"J({"power_mw":63499,"err_code":0})J"),
                json::parse(R"J({"day_list":[{"day":17,"energy_wh":512}],"err_code":0})J"), 18, s);
    check("no entry for today: no energy figure", s == json{{"power_w", 63.5}});
    s = json::object();
    energy_into(json::parse(R"J({"err_code":-1,"err_msg":"module not support","power_mw":5})J"),
                json::parse(R"J({"err_code":-1})J"), 18, s);
    check("a meter that refused gives nothing", s.empty());
    s = json::object();
    energy_into(json::parse(R"J({"power_mw":"lots","power":-50,"err_code":0})J"),
                json::parse(R"J({"day_list":[{"day":18,"energy_wh":-5}],"err_code":0})J"), 18, s);
    check("strings and negative readings give nothing", s.empty(), s.dump());
    s = json::object();
    energy_into(json::parse(R"J({"power_mw":1e300,"err_code":0})J"),
                json::parse(R"J({"day_list":[{"day":18,"energy_wh":1e12}],"err_code":0})J"), 18, s);
    check("enormous readings give nothing", s.empty(), s.dump());
    s = json::object();
    energy_into(json::parse(R"J({"power":-0.02,"err_code":0})J"), json(), 18, s);
    check("a reading a hair below zero is zero", s == json{{"power_w", 0.0}}, s.dump());
}

// ---------------------------------------------------------------- requests for changes
static void test_requests() {
    home::Change on, off, dim, dim0, warm, blue, both, heat, volume;
    on.on = true;
    off.on = false;
    dim.on = true;
    dim.brightness = 30;
    dim0.brightness = 0;
    warm.on = true;
    warm.kelvin = 2000;
    blue.on = true;
    blue.hs = std::make_pair(240, 100);
    both.kelvin = 4000;
    both.hs = std::make_pair(10, 10);
    heat.target_c = 21;
    volume.volume = 10;
    json r;
    std::string err;
    auto req = [&](const home::Thing &t, const home::Change &c) {
        err.clear();
        r = json();
        return request_for(t, c, r, err);
    };

    const home::Thing plug = first(things_of(kPlugV1));
    check("plug on", req(plug, on) && r == json::parse(R"J({"system":{"set_relay_state":{"state":1}}})J"), r.dump());
    check("plug off", req(plug, off) && r == json::parse(R"J({"system":{"set_relay_state":{"state":0}}})J"), r.dump());
    check("plug: brightness is refused, nothing to send", !req(plug, dim) && has(err, "dimmed") && r.empty());
    check("plug: a temperature is refused", !req(plug, heat) && err == "this Kasa plug can only be switched on and off");
    check("switch: wording names a switch", !req(first(things_of(kSwitch)), volume) && has(err, "Kasa switch"));
    const home::Thing socket = things_of(kStrip)[1];
    check("socket off: context names the socket, and only it",
          req(socket, off) && r == json::parse(R"J({"context":{"child_ids":["8006000000000000000000000000000000000501"]},
                                                  "system":{"set_relay_state":{"state":0}}})J"),
          r.dump());
    check("the request text puts the context first, as devices expect",
          r.dump().compare(0, 12, "{\"context\":{") == 0, r.dump());
    home::Thing tampered = socket;
    tampered.ref["child"] = "x\",\"y";
    check("a socket id that is not plain is never sent", !req(tampered, off) && r.empty() && has(err, "Settings"));
    const home::Thing dimmer = first(things_of(kDimmer));
    check("dimmer: brightness with on", req(dimmer, dim) && r == json::parse(R"J({"smartlife.iot.dimmer":{"set_brightness":{"brightness":30}},
                                                                              "system":{"set_relay_state":{"state":1}}})J"),
          r.dump());
    check("dimmer: brightness 0 is off (a dimmer has no 0)", req(dimmer, dim0) &&
                                                                 r == json::parse(R"J({"system":{"set_relay_state":{"state":0}}})J"),
          r.dump());
    check("dimmer: a colour is refused", !req(dimmer, blue) && r.empty());

    const home::Thing bulb = first(things_of(kColourBulb));
    check("bulb on: restores what its owner set (ignore_default 0)",
          req(bulb, on) && r == json::parse(R"J({"smartlife.iot.smartbulb.lightingservice":{"transition_light_state":{"on_off":1,"ignore_default":0}}})J"),
          r.dump());
    check("bulb off", req(bulb, off) && r == json::parse(R"J({"smartlife.iot.smartbulb.lightingservice":{"transition_light_state":{"on_off":0,"ignore_default":1}}})J"),
          r.dump());
    check("bulb brightness", req(bulb, dim) && r == json::parse(R"J({"smartlife.iot.smartbulb.lightingservice":{"transition_light_state":{"on_off":1,"brightness":30,"ignore_default":1}}})J"),
          r.dump());
    check("bulb white below its range is clamped to it (KL130: 2500 K)",
          req(bulb, warm) && r == json::parse(R"J({"smartlife.iot.smartbulb.lightingservice":{"transition_light_state":{"on_off":1,"color_temp":2500,"ignore_default":1}}})J"),
          r.dump());
    check("bulb colour: hue, saturation and color_temp 0",
          req(bulb, blue) && r == json::parse(R"J({"smartlife.iot.smartbulb.lightingservice":{"transition_light_state":{"on_off":1,"hue":240,"saturation":100,"color_temp":0,"ignore_default":1}}})J"),
          r.dump());
    check("bulb: colour and white at once is refused", !req(bulb, both) && r.empty());
    check("bulb: a volume is refused", !req(bulb, volume) && has(err, "light"));
    const home::Thing white = first(things_of(kWhiteBulbOff));
    check("white-only bulb: no colour, no shade of white", !req(white, blue) && !req(white, warm));
    const home::Thing strip = first(things_of(kLightStrip));
    check("light strip: its own service and method",
          req(strip, dim) && r == json::parse(R"J({"smartlife.iot.lightStrip":{"set_light_state":{"on_off":1,"brightness":30,"ignore_default":1}}})J"),
          r.dump());

    // Several sockets at once.
    const auto sockets = things_of(kStrip);
    std::vector<const home::Thing *> two = {&sockets[0], &sockets[2]};
    check("two sockets off: one request naming both",
          request_for_sockets(two, off, r, err) &&
              r == json::parse(R"J({"context":{"child_ids":["8006000000000000000000000000000000000500","8006000000000000000000000000000000000502"]},
                                  "system":{"set_relay_state":{"state":0}}})J"),
          r.dump());
    std::vector<const home::Thing *> mixed = {&sockets[0], &plug};
    err.clear();
    check("a plug among them: no single request (one at a time instead)", !request_for_sockets(mixed, off, r, err) && err.empty());
    check("brightness for sockets: no single request", !request_for_sockets(two, dim, r, err) && err.empty());

    // Replies.
    const json relay = json::parse(R"J({"system":{"set_relay_state":{"state":1}}})J");
    check("reply: done", reply_ok(relay, json::parse(R"J({"system":{"set_relay_state":{"err_code":0}}})J"), err));
    check("reply: an answer without err_code counts as done", reply_ok(relay, json::parse(R"J({"system":{"set_relay_state":{}}})J"), err));
    check("reply: refused", !reply_ok(relay, json::parse(R"J({"system":{"set_relay_state":{"err_code":-3,"err_msg":"invalid argument"}}})J"), err) &&
                                err == "the Kasa device refused the command");
    check("reply: module not supported", !reply_ok(json::parse(R"J({"smartlife.iot.dimmer":{"set_brightness":{"brightness":5}}})J"),
                                                   json::parse(R"J({"smartlife.iot.dimmer":{"err_code":-1,"err_msg":"module not support"}})J"), err) &&
                                             err == "the Kasa device does not support that");
    check("reply: a missing module is not understood", !reply_ok(relay, json::parse(R"J({"other":{}})J"), err) && has(err, "understood"));
    check("reply: a missing method is not understood", !reply_ok(relay, json::parse(R"J({"system":{"other":{"err_code":0}}})J"), err) &&
                                                           has(err, "understood"));
    check("reply: err_code that is not a number is not understood",
          !reply_ok(relay, json::parse(R"J({"system":{"set_relay_state":{"err_code":"0"}}})J"), err) && has(err, "understood"));
    const json two_modules = json::parse(R"J({"smartlife.iot.dimmer":{"set_brightness":{"brightness":30}},"system":{"set_relay_state":{"state":1}}})J");
    check("reply: every module must be done",
          !reply_ok(two_modules, json::parse(R"J({"smartlife.iot.dimmer":{"set_brightness":{"err_code":0}},"system":{"set_relay_state":{"err_code":-1}}})J"), err));
    check("reply: the context needs no answer", reply_ok(json::parse(R"J({"context":{"child_ids":["00"]},"system":{"set_relay_state":{"state":1}}})J"),
                                                         json::parse(R"J({"system":{"set_relay_state":{"err_code":0}}})J"), err));
}

// ---------------------------------------------------------------- routing between the two TP-Link drivers
static void test_routing() {
    auto kasa = make_kasa_driver();
    auto tapo = make_tapo_driver();
    check("kind is tplink", std::string(kasa->kind()) == "tplink");
    const std::map<std::string, std::string> plug = {{"device_type", "IOT.SMARTPLUGSWITCH"}, {"scheme", "XOR"}};
    const std::map<std::string, std::string> bulb = {{"device_type", "IOT.SMARTBULB"}};
    const std::map<std::string, std::string> moved = {{"device_type", "IOT.SMARTPLUGSWITCH"}, {"scheme", "KLAP"}};
    const std::map<std::string, std::string> camera = {{"device_type", "IOT.IPCAMERA"}};
    const std::map<std::string, std::string> tapo_plug = {{"device_type", "SMART.TAPOPLUG"}, {"scheme", "KLAP"}};
    const std::map<std::string, std::string> kasa_new = {{"device_type", "SMART.KASAPLUG"}, {"scheme", "KLAP"}};
    check("Kasa claims older plugs and bulbs", kasa->claims("tplink", plug) && kasa->claims("tplink", bulb));
    check("Kasa claims a moved device (to say it cannot use it)", kasa->claims("tplink", moved));
    check("Kasa claims a moved device an older version filed under Tapo", kasa->claims("tapo", moved));
    check("Tapo no longer claims IOT types, in either kind", !tapo->claims("tapo", moved) && !tapo->claims("tapo", plug));
    check("Kasa does not claim cameras, nor a tplink device with no type (an mDNS answer)",
          !kasa->claims("tplink", camera) && !kasa->claims("tplink", {}));
    check("Tapo devices, including Kasa models on Tapo's commands, stay Tapo's",
          tapo->claims("tapo", tapo_plug) && tapo->claims("tapo", kasa_new) && !kasa->claims("tapo", tapo_plug) &&
              !kasa->claims("tapo", kasa_new));
    check("Kasa claims nothing of other kinds", !kasa->claims("homekit", plug) && !kasa->claims("kasa", plug));
    check("no pairing", kasa->next_step(home::Device()).kind == home::Pairing::None);

    // The port: what discovery saved for kind tplink; never the newer login's HTTP port that a
    // version which filed these under Tapo saved.
    home::Device d;
    d.kind = "tplink";
    d.port = 10001;
    const int saved_port = port_for(d);
    d.port = 0;
    const int no_port = port_for(d);
    d.kind = "tapo";
    d.port = 80;
    check("port: the saved one for tplink, else 9999 (an older version's Tapo record kept port 80)",
          saved_port == 10001 && no_port == 9999 && port_for(d) == 9999);
}

// ---------------------------------------------------------------- the device at the address is the saved one
static void test_identity() {
    check("MAC digits from every way of writing one", mac_digits("50:C7:BF:00:00:01") == "50c7bf000001" &&
                                                         mac_digits("50-c7-bf-00-00-01") == "50c7bf000001" &&
                                                         mac_digits("50C7BF000001") == "50c7bf000001");
    check("not a MAC: too short, too long, not hex, empty", mac_digits("50:C7:BF:00:00").empty() &&
                                                                mac_digits("50:C7:BF:00:00:01:02").empty() &&
                                                                mac_digits("zz:C7:BF:00:00:01").empty() && mac_digits("").empty());
    check("the saved plug answers: same device", same_device("50:c7:bf:00:00:01", json::parse(kPlugV1)));
    check("a bulb by its mic_mac", same_device("1c:3b:f3:00:00:06", json::parse(kColourBulb)));
    check("another device at the address: not the same", !same_device("c0:06:c3:00:00:02", json::parse(kPlugV1)) &&
                                                          !same_device("50:c7:bf:00:00:01", json::parse(kColourBulb)));
    // Only a MAC that matches counts: an answer without one cannot be told apart from another device's.
    check("nothing saved, or nothing reported: not the same device", !same_device("", json::parse(kPlugV1)) &&
                                                                         !same_device("50:c7:bf:00:00:01", json::parse(kSwitch)));
    json blank = json::parse(kPlugV1);
    blank["mac"] = "";
    check("an empty MAC is not a match", !same_device("50:c7:bf:00:00:01", blank));
    blank["mac"] = "not a mac";
    blank["mic_mac"] = "50:C7:BF:00:00:01:FF";
    check("a MAC that is not one is not a match", !same_device("50:c7:bf:00:00:01", blank));
    const std::string low = util::lower(kasa::kOtherDevice);
    check("its sentence reads as 'not at that address', so the app looks for the device again", has(low, "answer"));
}

// ---------------------------------------------------------------- the driver on 127.0.0.1
struct Listener {
    int fd = -1, port = 0;
    Listener() {
        fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof(a);
        if (bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0 || listen(fd, 4) != 0 ||
            getsockname(fd, reinterpret_cast<sockaddr *>(&a), &len) != 0)
            perror("listener");
        port = ntohs(a.sin_port);
    }
    ~Listener() { close(fd); }
    bool pending(int wait_ms) {   // whether a connection came in
        pollfd p{fd, POLLIN, 0};
        return poll(&p, 1, wait_ms) > 0;
    }
};

// Answers one connection: reads one framed request into `got`, sends `reply` framed.
static std::thread serve_once(Listener &l, std::string &got, const std::string &reply) {
    return std::thread([&l, &got, reply] {
        if (!l.pending(3000)) return;
        const int c = accept(l.fd, nullptr, nullptr);
        if (c < 0) return;
        unsigned char head[4];
        if (recv(c, head, 4, MSG_WAITALL) == 4) {
            const size_t n = (size_t(head[0]) << 24) | (size_t(head[1]) << 16) | (size_t(head[2]) << 8) | head[3];
            std::string body(n, '\0');
            if (n < 100000 && recv(c, &body[0], n, MSG_WAITALL) == static_cast<ssize_t>(n)) got = decrypt(body);
            const std::string out = frame(reply);
            if (send(c, out.data(), out.size(), MSG_NOSIGNAL) < 0) perror("send");
        }
        close(c);
    });
}

// Answers one connection after another with `replies` in turn, one framed request each; `got`
// collects the requests as the device decoded them.
static std::thread serve_each(Listener &l, std::vector<std::string> &got, const std::vector<std::string> &replies) {
    return std::thread([&l, &got, replies] {
        for (const std::string &reply : replies) {
            if (!l.pending(3000)) return;
            const int c = accept(l.fd, nullptr, nullptr);
            if (c < 0) return;
            unsigned char head[4];
            if (recv(c, head, 4, MSG_WAITALL) == 4) {
                const size_t n = (size_t(head[0]) << 24) | (size_t(head[1]) << 16) | (size_t(head[2]) << 8) | head[3];
                std::string body(n, '\0');
                if (n < 100000 && recv(c, &body[0], n, MSG_WAITALL) == static_cast<ssize_t>(n)) got.push_back(decrypt(body));
                const std::string out = frame(reply);
                if (send(c, out.data(), out.size(), MSG_NOSIGNAL) < 0) perror("send");
            }
            close(c);
        }
    });
}

static int closed_port() {   // a port nothing listens on: bound, then closed
    Listener l;
    return l.port;
}

static void test_driver() {
    auto kasa = make_kasa_driver();
    std::string err;

    // A device discovery says moved to the newer login: nothing at all is sent.
    {
        Listener l;
        home::Device dev;
        dev.kind = "tplink";
        dev.ip = "127.0.0.1";
        dev.port = l.port;
        dev.info = {{"device_type", "IOT.SMARTPLUGSWITCH"}, {"scheme", "KLAP"}};
        dev.things = things_of(kPlugV1);
        std::vector<home::Thing> out;
        home::Change on;
        on.on = true;
        const bool e = kasa->enumerate(dev, out, err);
        check("moved (from discovery): enumerate says the newer login", !e && err.compare(0, strlen(kasa::kNewLogin), kasa::kNewLogin) == 0 &&
                                                                       has(err, "Tapo"),
              err);
        out.clear();
        const bool r = kasa->read(dev, out, err);
        const bool a = kasa->apply(dev, dev.things[0], on, 2500, err);
        check("moved: read and apply refuse too", !r && !a && has(err, kasa::kNewLogin));
        check("moved: the device was never contacted", !l.pending(200));
        const std::string low = util::lower(err);
        bool reads_unreachable = false;
        for (const char *w : {"connect", "reach", "answer", "respond", "timed out", "timeout", "no route", "unreachable"})
            reads_unreachable = reads_unreachable || has(low, w);
        check("moved: the sentence does not read as 'did not answer' (no pointless re-scan)", !reads_unreachable, err);
    }

    // Device::port is honoured, and the request is the plain get_sysinfo.
    {
        Listener l;
        std::string got;
        std::thread t = serve_once(l, got, std::string(R"J({"system":{"get_sysinfo":)J") + kStrip + "}}");
        home::Device dev;
        dev.kind = "tplink";
        dev.ip = "127.0.0.1";
        dev.port = l.port;
        dev.info = {{"device_type", "IOT.SMARTPLUGSWITCH"}, {"scheme", "XOR"}};
        std::vector<home::Thing> out;
        const bool ok = kasa->enumerate(dev, out, err);
        t.join();
        check("enumerate on the saved port: three sockets, the strip's name and model",
              ok && out.size() == 3 && dev.name == "Desk strip" && dev.model == "HS300(US)", err);
        check("what the device received: get_sysinfo, nothing else", got == R"J({"system":{"get_sysinfo":{}}})J", got);
    }

    // Port 9999 refuses: the device's discovery port is asked whether it moved.
    {
        const int tcp = closed_port();
        const int udp = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof(a);
        bind(udp, reinterpret_cast<sockaddr *>(&a), sizeof(a));
        getsockname(udp, reinterpret_cast<sockaddr *>(&a), &len);
        set_probe_port(ntohs(a.sin_port));
        std::atomic<int> probes{0};
        const std::string answers[2] = {
            R"J({"error_code":0,"result":{"device_type":"IOT.SMARTPLUGSWITCH","mgt_encrypt_schm":{"encrypt_type":"KLAP"}}})J",
            R"J({"error_code":0,"result":{"device_type":"SMART.TAPOPLUG","mgt_encrypt_schm":{"encrypt_type":"KLAP"}}})J"};
        std::thread responder([&] {
            for (int k = 0; k < 2; ++k) {
                pollfd p{udp, POLLIN, 0};
                if (poll(&p, 1, 3000) <= 0) return;
                char buf[64];
                sockaddr_in from{};
                socklen_t flen = sizeof(from);
                const ssize_t n = recvfrom(udp, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &flen);
                if (n == 16) probes++;
                const std::string body = std::string(16, '\x01') + answers[k];
                sendto(udp, body.data(), body.size(), 0, reinterpret_cast<sockaddr *>(&from), flen);
            }
        });
        home::Device dev;
        dev.kind = "tplink";
        dev.ip = "127.0.0.1";
        dev.port = tcp;
        dev.info = {{"device_type", "IOT.SMARTPLUGSWITCH"}, {"scheme", "XOR"}};
        std::vector<home::Thing> out;
        check("refused, and the 20002 answer says KLAP: the newer login", !kasa->enumerate(dev, out, err) && has(err, kasa::kNewLogin), err);
        check("refused, and a Tapo device answers: just refused", !kasa->enumerate(dev, out, err) && err == kasa::kRefused, err);
        responder.join();
        check("one 16-byte query each time", probes == 2);
        close(udp);
        set_probe_port(closed_port());
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = kasa->enumerate(dev, out, err);
        const long ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        check("refused, nothing on the discovery port: refused, at once", !ok && err == kasa::kRefused && ms < 900, err + " " + std::to_string(ms));
        set_probe_port(20002);
    }

    // The router gave the saved plug's address to another Kasa plug: nothing is switched there.
    {
        const std::string sysinfo = std::string(R"J({"system":{"get_sysinfo":)J") + kPlugV1 + "}}";
        const std::string get = R"J({"system":{"get_sysinfo":{}}})J";
        home::Device dev;
        dev.kind = "tplink";
        dev.ip = "127.0.0.1";
        dev.name = "Desk fan";
        dev.mac = "c0:06:c3:00:00:02";   // the saved plug; the one answering is 50:C7:BF:00:00:01
        dev.info = {{"device_type", "IOT.SMARTPLUGSWITCH"}, {"scheme", "XOR"}};
        dev.things = things_of(kPlugV2);
        home::Change on;
        on.on = true;
        {
            Listener l;
            dev.port = l.port;
            std::vector<std::string> got;
            std::thread t = serve_each(l, got, {sysinfo});
            const bool ok = kasa->apply(dev, dev.things[0], on, 2500, err);
            t.join();
            check("another plug at the address: the change is refused", !ok && err == kasa::kOtherDevice, err);
            check("another plug at the address: it was only asked what it is", got == std::vector<std::string>{get} && !l.pending(300),
                  got.empty() ? "" : got.back());
        }
        {
            Listener l;
            dev.port = l.port;
            std::vector<std::string> got;
            std::thread t = serve_each(l, got, {sysinfo, sysinfo});
            std::vector<home::Thing> out;
            const bool e = kasa->enumerate(dev, out, err);
            const bool r = kasa->read(dev, out, err);
            t.join();
            check("another plug at the address: enumerate and read refuse, the saved name and things stay",
                  !e && !r && err == kasa::kOtherDevice && dev.name == "Desk fan" && out.size() == 1 && out[0].name == "Desk fan", err);
        }
        {
            Listener l;
            dev.port = l.port;
            dev.mac = "50-C7-BF-00-00-01";   // now it is the saved one, written another way
            std::vector<std::string> got;
            std::thread t = serve_each(l, got, {sysinfo, R"J({"system":{"set_relay_state":{"err_code":0}}})J"});
            const bool ok = kasa->apply(dev, dev.things[0], on, 2500, err);
            t.join();
            check("the saved plug at the address: asked who it is, then switched",
                  ok && got == std::vector<std::string>{get, R"J({"system":{"set_relay_state":{"state":1}}})J"}, err);
        }
        // A device at the saved address that does not say which it is (no MAC, or an empty one):
        // asked who it is, and nothing more. It may be another device that has taken the address.
        for (const bool empty : {false, true}) {
            json quiet = json::parse(kPlugV1);
            quiet.erase("mac");
            if (empty) quiet["mac"] = "";
            const std::string reply = std::string(R"J({"system":{"get_sysinfo":)J") + quiet.dump() + "}}";
            Listener l;
            dev.port = l.port;
            dev.mac = "50:c7:bf:00:00:01";
            std::vector<std::string> got;
            std::thread t = serve_each(l, got, {reply, R"J({"system":{"set_relay_state":{"err_code":0}}})J"});
            const bool ok = kasa->apply(dev, dev.things[0], on, 2500, err);
            t.join();
            const std::string what = empty ? "an empty MAC" : "no MAC";
            check("an answer with " + what + ": the change is refused, in words that make the app look for the device",
                  !ok && err == kasa::kNoMac && has(err, "answers"), err);
            check("an answer with " + what + ": it was only asked what it is", got == std::vector<std::string>{get} && !l.pending(300),
                  got.empty() ? "" : got.back());
            std::vector<home::Thing> out;
            Listener l2;
            dev.port = l2.port;
            got.clear();
            std::thread t2 = serve_each(l2, got, {reply});
            const bool e = kasa->enumerate(dev, out, err);
            t2.join();
            check("an answer with " + what + ": not read as the saved device either", !e && err == kasa::kNoMac, err);
        }
        {
            Listener l;
            dev.port = l.port;
            dev.mac = "";   // a saved device without a MAC: it cannot be checked, so nothing is sent
            const bool ok = kasa->apply(dev, dev.things[0], on, 2500, err);
            check("no saved MAC: the change is refused, and nothing was sent", !ok && err == kasa::kNoIdentity && !l.pending(300), err);
            bool reads_unreachable = false;
            for (const char *w : {"connect", "reach", "answer", "respond", "timed out", "timeout", "no route", "unreachable"})
                reads_unreachable = reads_unreachable || has(util::lower(err), w);
            check("no saved MAC: the sentence does not send the app looking for it (there is nothing to know it by)", !reads_unreachable,
                  err);
        }
    }

    // Addresses that are not usable are never tried.
    {
        home::Device dev;
        dev.kind = "tplink";
        dev.ip = "not an address";
        dev.info = {{"device_type", "IOT.SMARTPLUGSWITCH"}};
        std::vector<home::Thing> out;
        check("an address that is not an IP literal is refused", !kasa->enumerate(dev, out, err) && has(err, "address"));
    }
}

int main() {
    test_scrambling();
    test_parsing();
    test_things();
    test_energy();
    test_requests();
    test_routing();
    test_identity();
    test_driver();
    printf("%d checks, %d failed\n", g_run, g_failed);
    return g_failed ? 1 : 0;
}
