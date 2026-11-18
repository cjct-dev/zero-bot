// Known-answer and hostile-answer tests for the WiZ driver (src/smarthome/drivers/wiz.cpp).
// Nothing is contacted but 127.0.0.1: the device answers in the second half come from a UDP peer
// in this program, on a port the system picks. Each check prints one line; the exit status is
// non-zero when any fails.
//
// Where the expected values come from:
//   pywizlight  sbidy/pywizlight (MIT): tests/test_signing.py (the signed request, byte for byte),
//               tests/test_bulb.py (hue 100, saturation 50 gives r 88, g 255, b 0),
//               tests/test_bulb_min_dim_level.py (brightness 1 % gives dimming 10, or 1 where the
//               model says minDimLevel 1; 128 of 255 gives 50), tests/fake_bulb.py (module names,
//               getModelConfig and getUserConfig answers, getPower in milliwatts)
//   openssl     the openssl command line, independently of this code:
//               printf '%s' PARAMS | openssl dgst -sha256 -mac HMAC -macopt hexkey:KEY -binary | openssl enc -base64 -A
//   by hand     the pure primaries, white and the clamping rules, worked out from the formulas
#include "smarthome/drivers/wiz.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/evp.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace wiz;
using namespace wiz::detail;

static int g_failed = 0, g_run = 0;

static void check(const std::string &name, bool ok, const std::string &detail = "") {
    g_run++;
    if (!ok) g_failed++;
    printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", name.c_str(), ok || detail.empty() ? "" : "  -> ", ok ? "" : detail.c_str());
    fflush(stdout);
}

static void eq(const std::string &name, const std::string &got, const std::string &want) {
    check(name, got == want, "got \"" + got + "\", want \"" + want + "\"");
}

static void eqi(const std::string &name, long long got, long long want) {
    check(name, got == want, "got " + std::to_string(got) + ", want " + std::to_string(want));
}

static std::string bytes_of_hex(const std::string &hex) {
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out += static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16));
    return out;
}

// The words src/smarthome/control.cpp unreachable() looks for (lower case): an error with one of
// them makes the app look for the device at a new address.
static bool reads_as_unreachable(std::string err) {
    for (char &c : err) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (const char *w : {"connect", "reach", "answer", "respond", "timed out", "timeout", "no route", "unreachable"})
        if (err.find(w) != std::string::npos) return true;
    return false;
}

static home::Thing thing_of(const std::vector<home::Thing> &things, const std::string &id) {
    for (const auto &t : things)
        if (t.id == id) return t;
    return {};
}

static int open_fds() {
    int n = 0;
    if (DIR *d = opendir("/proc/self/fd")) {
        while (readdir(d)) n++;
        closedir(d);
    }
    return n;
}

static long long ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

// ---------------------------------------------------------------------------------------------
static void test_signing() {
    const std::string key = bytes_of_hex("00112233445566778899aabbccddeeff");
    eq("pywizlight known answer: setPilot {\"state\":true} signed at 1790441723",
       datagram("setPilot", "{\"state\":true}", key, 1790441723),
       "{\"method\":\"setPilot\",\"env\":\"pro\",\"params\":{\"state\":true,\"sigTs\":1790441723},"
       "\"hmac\":\"vARMv4JpVsxYabcBovABd25ISBzYT8+71v0CUtpU1sg=\"}");
    eq("openssl known answer: three params signed at 1790441800",
       datagram("setPilot", "{\"state\":true,\"dimming\":50,\"temp\":2700}", key, 1790441800),
       "{\"method\":\"setPilot\",\"env\":\"pro\",\"params\":{\"state\":true,\"dimming\":50,\"temp\":2700,\"sigTs\":1790441800},"
       "\"hmac\":\"Sg/Ui/OmSSVw83hm0lYU3jkYWocbxloQvB3t0uMx43A=\"}");
    eq("an unsigned request carries no env, time or signature", datagram("getPilot", "{}", "", 1790441723),
       "{\"method\":\"getPilot\",\"params\":{}}");
    check("empty params signed: only the time", datagram("setPilot", "{}", key, 5).find("\"params\":{\"sigTs\":5}") != std::string::npos,
          datagram("setPilot", "{}", key, 5));

    // When a request may be signed: a set clock, synchronised or long after start.
    check("clock: synchronised, 2026", signing_clock_ok(1790441723, true, 5));
    check("clock: 1970 (never set), even if called synchronised", !signing_clock_ok(86400, true, 5000));
    check("clock: 2024, not set", !signing_clock_ok(1704067200, true, 5000));
    check("clock: not synchronised one minute after start (a saved time)", !signing_clock_ok(1790441723, false, 60));
    check("clock: not synchronised 9 minutes 59 after start", !signing_clock_ok(1790441723, false, 599));
    check("clock: not synchronised ten minutes after start: taken as it is", signing_clock_ok(1790441723, false, 600));

    std::string k;
    check("a key: 32 hex characters", parse_key("00112233445566778899aabbccddeeff", k) && k == key);
    check("capitals and spaces are fine", parse_key("  0011 2233 4455 6677 8899 AABB CCDD EEFF\n", k) && k == key);
    check("the whole exported file pasted", parse_key("{\"homes\":[{\"name\":\"x\",\"udp_signing_key\":\"00112233445566778899aabbccddeeff\"}]}", k) &&
                                                k == key);
    check("too short", !parse_key("00112233445566778899aabbccddee", k));
    check("too long", !parse_key("00112233445566778899aabbccddeeff00", k));
    check("not hex", !parse_key("0011223344556677889zaabbccddeeff", k));
    check("empty", !parse_key("", k));
    check("a file without the key", !parse_key("{\"homes\":[{\"name\":\"x\"}]}", k));
    check("a key of the wrong type in the file", !parse_key("{\"udp_signing_key\":12345}", k));
    check("deeply nested paste does not crash", !parse_key(std::string(5000, '[') + std::string(5000, ']'), k));
}

static void test_models() {
    struct Case { const char *module; int type_id; const char *cls; };
    const Case cases[] = {
        {"ESP01_SHRGB_03", -1, "RGB"},      {"ESP01_SHRGB1C_31", -1, "RGB"},   {"ESP20_SHRGBC_01", -1, "RGB"},
        {"ESP25_SHORGB_01", -1, "RGB"},     {"ESP20_DHRGB_01B", -1, "RGB"},    {"ESP21_SHTW_01", -1, "TW"},
        {"ESP14_SHTW1C_01", -1, "TW"},      {"ESP05_SHDW_21", -1, "DW"},       {"ESP06_SHDW9_01", -1, "DW"},
        {"ESP01_DIMTRIACS_01", -1, "DW"},   {"ESP10_SOCKET_06", -1, "SOCKET"}, {"ESP25_SOCKET_01", -1, "SOCKET"},
        {"ESP03_FANDIMS_31", -1, "FANDIM"}, {"ESP03_DDTW_01", -1, "FANTW"},    {"", 0, "DW"},
        {"", 1, ""},                        {"", -1, ""},                      {"INVALID", 0, ""},
        {"ESP99_CAMERA_01", -1, ""},        {"ESP99_SHSENSOR_01", -1, ""},     {"ESP01", -1, ""},
        {"_RGB", -1, "RGB"},
    };
    for (const Case &c : cases) {
        Model m;
        const bool ok = classify(c.module, c.type_id, m);
        eq(std::string("module \"") + c.module + "\" type " + std::to_string(c.type_id), ok ? m.cls : "", c.cls);
    }

    Model m;
    classify("ESP01_SHRGB_03", -1, m);
    apply_config(json::parse(R"({"ps":1,"pwmFreq":1000,"pwmRange":[3,100],"wcr":30,"nowc":1,"cctRange":[2200,2700,4800,6500]})"), m);
    check("getModelConfig cctRange gives the white range", m.kelvin_min == 2200 && m.kelvin_max == 6500 && m.min_dimming == 10);
    apply_config(json::parse(R"({"minDimLevel":1,"cctRange":[2700,2700,5000,5000],"fanSpeed":0})"), m);
    check("minDimLevel 1, the narrower range; fanSpeed 0 ignored", m.kelvin_min == 2700 && m.kelvin_max == 5000 && m.min_dimming == 1 &&
                                                                       m.fan_speeds == 6);
    Model fan;
    classify("ESP03_FANDIMS_31", -1, fan);
    apply_config(json::parse(R"({"cctRange":[2700,2700,2700,2700],"fanSpeed":4})"), fan);
    eqi("a fan's speeds from getModelConfig", fan.fan_speeds, 4);
    Model old;
    classify("ESP05_SHTW_21", -1, old);
    apply_config(json::parse(R"({"fadeIn":450,"whiteRange":[2700,6500],"extRange":[2200,6500]})"), old);
    check("getUserConfig: extRange is preferred", old.kelvin_min == 2200 && old.kelvin_max == 6500);
    Model hostile;
    classify("ESP21_SHTW_01", -1, hostile);
    for (const char *bad : {R"({"cctRange":"2700"})", R"({"cctRange":[]})", R"({"cctRange":[2700,"x"]})", R"({"cctRange":[[2700],[6500]]})",
                            R"({"cctRange":[10,99999]})", R"({"cctRange":[6500,1e300]})", R"({"minDimLevel":-5})", R"({"minDimLevel":"10"})",
                            R"({"minDimLevel":1e300})", R"({"fanSpeed":100000})", R"([1,2,3])", R"("text")", R"(null)"})
        apply_config(json::parse(bad), hostile);
    check("values out of range or of the wrong type change nothing",
          hostile.kelvin_min == 2200 && hostile.kelvin_max == 6500 && hostile.min_dimming == 10 && hostile.fan_speeds == 6);
}

static void test_scales() {
    eqi("pywizlight: 1 % is the lowest dimming, 10 by default", dimming_from_percent(1, 10), 10);
    eqi("pywizlight: 1 % with minDimLevel 1", dimming_from_percent(1, 1), 1);
    eqi("pywizlight: 128 of 255 is 50 %, sent as it is", dimming_from_percent(50, 10), 50);
    eqi("100 %", dimming_from_percent(100, 10), 100);
    eqi("more than 100 % is 100", dimming_from_percent(150, 10), 100);
    eqi("0 % (never sent: the app turns it into off) is the lowest", dimming_from_percent(0, 10), 10);
    eqi("a nonsense lowest dimming is kept in range", dimming_from_percent(5, 500), 100);
    eqi("dimming 13 reads as 13 %", percent_from_dimming(13), 13);
    eqi("dimming 0 reads as 1 %: a light that is on is never at 0", percent_from_dimming(0), 1);
    eqi("dimming 255 reads as 100 %", percent_from_dimming(255), 100);

    eqi("fan: 0 % is stopped", fan_speed_from_percent(0, 6), 0);
    eqi("fan: 1 % is the slowest", fan_speed_from_percent(1, 6), 1);
    eqi("fan: 50 % of six speeds is 3", fan_speed_from_percent(50, 6), 3);
    eqi("fan: 60 % of six speeds rounds up to 4", fan_speed_from_percent(60, 6), 4);
    eqi("fan: 99 % is the fastest", fan_speed_from_percent(99, 6), 6);
    eqi("fan: 100 % is the fastest", fan_speed_from_percent(100, 6), 6);
    eqi("fan: 250 % is the fastest", fan_speed_from_percent(250, 6), 6);
    eqi("fan: no speeds reported", fan_speed_from_percent(50, 0), 1);
    eqi("fan: speed 3 of 6 reads as 50 %", percent_from_fan_speed(3, 6), 50);
    eqi("fan: speed 9 of 6 reads as 100 %", percent_from_fan_speed(9, 6), 100);

    struct Hs { double h, s; int r, g, b, w; const char *from; };
    const Hs cases[] = {
        {100, 50, 88, 255, 0, 128, "pywizlight"}, {0, 100, 255, 0, 0, 0, "by hand"},   {120, 100, 0, 255, 0, 0, "by hand"},
        {240, 100, 0, 0, 255, 0, "by hand"},      {360, 100, 255, 0, 0, 0, "by hand"}, {0, 0, 0, 0, 0, 128, "by hand"},
        {0, 25, 127, 0, 0, 128, "by hand"},       {0, 75, 255, 0, 0, 64, "by hand"},   {60, 100, 255, 254, 0, 0, "truncated as pywizlight does"},
        {-120, 100, 0, 0, 255, 0, "by hand"},     {0, 500, 255, 0, 0, 0, "by hand"},
    };
    for (const Hs &c : cases) {
        int rgb[3], w;
        hs_to_rgbw(c.h, c.s, rgb, w);
        char name[128], got[64], want[64];
        snprintf(name, sizeof name, "%s: hue %g saturation %g", c.from, c.h, c.s);
        snprintf(got, sizeof got, "%d,%d,%d w%d", rgb[0], rgb[1], rgb[2], w);
        snprintf(want, sizeof want, "%d,%d,%d w%d", c.r, c.g, c.b, c.w);
        eq(name, got, want);
    }
    // Back from the LEDs: the hue within 2 degrees; the saturation within 2 above 50 % (the white
    // channel carries it), and up to 15 % low up to 50 %, where the colour LEDs carry it and the
    // mix of two primaries is not a full step on the wheel (by hand: |b0 + c b1| >= 0.866).
    bool round_trip = true;
    std::string worst;
    for (int h = 0; h < 360; h += 15)
        for (int s = 10; s <= 100; s += 10) {
            int rgb[3], w, h2, s2;
            hs_to_rgbw(h, s, rgb, w);
            rgbw_to_hs(rgb, w, h2, s2);
            const int dh = std::min(std::abs(h2 - h), 360 - std::abs(h2 - h));
            const bool sat_ok = s > 50 ? std::abs(s2 - s) <= 2 : s2 >= s * 0.85 - 1 && s2 <= s + 2;
            if (dh > 2 || !sat_ok) {
                round_trip = false;
                worst = std::to_string(h) + "/" + std::to_string(s) + " came back " + std::to_string(h2) + "/" + std::to_string(s2);
            }
        }
    check("hue and saturation survive a trip to the LEDs and back", round_trip, worst);
    int hue = -1, sat = -1;
    const int grey[3] = {200, 200, 200};
    rgbw_to_hs(grey, 0, hue, sat);
    check("equal red, green and blue is no colour", sat == 0, std::to_string(hue) + "/" + std::to_string(sat));
    const int junk[3] = {-500, 99999, 0};
    rgbw_to_hs(junk, 9999, hue, sat);
    check("values out of range are clamped", hue >= 0 && hue < 360 && sat >= 0 && sat <= 100);
}

static void test_things_and_state() {
    Model rgb;
    classify("ESP01_SHRGB_03", -1, rgb);
    auto things = things_for(rgb, "a8bb5006033d", false, false);
    check("an RGB bulb is one light", things.size() == 1 && things[0].id == "light" && things[0].type == "light");
    eq("named after the end of its MAC", things.empty() ? "" : things[0].name, "WiZ light 033d");
    check("power, brightness, white and colour",
          !things.empty() && things[0].caps == (home::kPower | home::kBrightness | home::kColorTemp | home::kColor));
    check("not signed", !things.empty() && !things[0].ref.contains("signed"));
    check("a MAC that is not hex does not reach the name", things_for(rgb, "a8bb50\n6\"3d", false, false)[0].name == "WiZ light");

    Model sock;
    classify("ESP25_SOCKET_01", -1, sock);
    things = things_for(sock, "d8a0119906b7", true, true);
    check("a metering plug: power and a sensor", things.size() == 1 && things[0].type == "plug" &&
                                                     things[0].caps == (home::kPower | home::kSensor));
    check("signed firmware is marked on the thing", !things.empty() && things[0].ref.value("signed", false));
    Model dw;
    classify("ESP05_SHDW_21", -1, dw);
    things = things_for(dw, "", false, false);
    check("a dimmable bulb: power and brightness only", things.size() == 1 && things[0].caps == (home::kPower | home::kBrightness) &&
                                                            things[0].name == "WiZ light");
    Model fd;
    classify("ESP03_FANDIMS_31", -1, fd);
    things = things_for(fd, "a8bb50000001", false, false);
    check("a ceiling fan with a light: a light and a fan", things.size() == 2 && thing_of(things, "fan").type == "fan" &&
                                                               thing_of(things, "fan").caps == (home::kPower | home::kFanSpeed) &&
                                                               thing_of(things, "light").caps == (home::kPower | home::kBrightness));
    Model ft;
    classify("ESP03_DDTW_01", -1, ft);
    check("a fan with a tunable light has white", thing_of(things_for(ft, "", false, false), "light").caps & home::kColorTemp);
    for (const char *module : {"ESP99_CAMERA_01", "INVALID"}) {
        Model none;
        classify(module, -1, none);
        check(std::string("nothing from ") + module, things_for(none, "", false, false).empty());
    }

    // State, from getPilot results as fake_bulb.py and real firmware send them.
    home::Thing light = thing_of(things_for(rgb, "", false, false), "light");
    fill_state(light, json::parse(R"({"mac":"a8bb5006033d","rssi":-62,"src":"udp","state":true,"sceneId":0,"temp":2700,"dimming":13})"), nullptr);
    eq("white: on, brightness and kelvin", light.state.dump(), R"({"brightness":13,"kelvin":2700,"on":true})");
    fill_state(light, json::parse(R"({"state":false,"sceneId":0,"r":255,"g":127,"b":0,"c":0,"w":0,"temp":0,"dimming":13})"), nullptr);
    check("colour: hue and saturation", light.state.value("on", true) == false && light.state.value("hue", -1) >= 20 &&
                                            light.state.value("hue", -1) <= 40 && light.state.value("saturation", -1) == 100 &&
                                            !light.state.contains("kelvin"),
          light.state.dump());
    fill_state(light, json::parse(R"({"state":true,"sceneId":27,"dimming":100})"), nullptr);
    eq("in a scene: neither white nor colour", light.state.dump(), R"({"brightness":100,"on":true})");
    fill_state(light, json::parse(R"({"state":"yes","dimming":"50","temp":[2700],"r":{"x":1},"g":1e300,"b":null,"sceneId":"x"})"), nullptr);
    check("values of the wrong type are left out", light.state.value("on", true) == false && !light.state.contains("brightness") &&
                                                       !light.state.contains("kelvin"),
          light.state.dump());
    fill_state(light, json::parse(R"({"state":true,"temp":99999,"dimming":1e300})"), nullptr);
    eq("values out of range are left out", light.state.dump(), R"({"on":true})");
    fill_state(light, json::parse(R"([1,2,3])"), nullptr);
    eq("a pilot that is not an object", light.state.dump(), R"({"on":false})");
    home::Thing white = thing_of(things_for(dw, "", false, false), "light");
    fill_state(white, json::parse(R"({"state":true,"r":255,"g":0,"b":0,"temp":0,"dimming":40})"), nullptr);
    eq("a dimmable bulb reports no colour", white.state.dump(), R"({"brightness":40,"on":true})");

    home::Thing plug = thing_of(things_for(sock, "", true, false), "plug");
    const json mw = 1065385;
    fill_state(plug, json::parse(R"({"state":true})"), &mw);
    eq("fake_bulb.py getPower 1065385 mW", plug.state.dump(), R"({"on":true,"power_w":1065.4})");
    fill_state(plug, json::parse(R"({"state":false,"pc":2500})"), nullptr);
    eq("the pilot's own reading, pc", plug.state.dump(), R"({"on":false,"power_w":2.5})");
    const json silly = -5;
    fill_state(plug, json::parse(R"({"state":true})"), &silly);
    eq("a negative reading is left out", plug.state.dump(), R"({"on":true})");
    home::Thing unmetered = thing_of(things_for(sock, "", false, false), "plug");
    fill_state(unmetered, json::parse(R"({"state":true,"pc":2500})"), nullptr);
    eq("a plug that does not meter reports no power", unmetered.state.dump(), R"({"on":true})");

    home::Thing fan_t = thing_of(things_for(fd, "", false, false), "fan");
    fill_state(fan_t, json::parse(R"({"state":true,"dimming":50,"fanState":1,"fanSpeed":3,"fanMode":1,"fanRevrs":0})"), nullptr);
    eq("a fan: on and its speed", fan_t.state.dump(), R"({"fan":50,"on":true})");
    fill_state(fan_t, json::parse(R"({"state":true,"fanState":0})"), nullptr);
    eq("a fan that is off while its light is on", fan_t.state.dump(), R"({"on":false})");
}

static void test_pilots() {
    Model rgb, dw, sock, fd;
    classify("ESP01_SHRGB_03", -1, rgb);
    classify("ESP05_SHDW_21", -1, dw);
    classify("ESP25_SOCKET_01", -1, sock);
    classify("ESP03_FANDIMS_31", -1, fd);
    rgb.kelvin_min = 2700;
    rgb.kelvin_max = 6500;
    const home::Thing light = thing_of(things_for(rgb, "", false, false), "light");
    const home::Thing white = thing_of(things_for(dw, "", false, false), "light");
    const home::Thing plug = thing_of(things_for(sock, "", true, false), "plug");
    const home::Thing fan = thing_of(things_for(fd, "", false, false), "fan");

    struct Case { const char *name; const home::Thing *thing; home::Change change; const char *want; };
    auto ch = [](std::function<void(home::Change &)> f) {
        home::Change c;
        f(c);
        return c;
    };
    const Case cases[] = {
        {"light on", &light, ch([](home::Change &c) { c.on = true; }), R"({"state":true})"},
        {"light off", &light, ch([](home::Change &c) { c.on = false; }), R"({"state":false})"},
        {"off wins over the rest", &light, ch([](home::Change &c) { c.on = false; c.brightness = 40; }), R"({"state":false})"},
        {"brightness 40", &light, ch([](home::Change &c) { c.on = true; c.brightness = 40; }), R"({"state":true,"dimming":40})"},
        {"brightness 1 is the lowest dimming", &light, ch([](home::Change &c) { c.brightness = 1; }), R"({"state":true,"dimming":10})"},
        {"warm white", &light, ch([](home::Change &c) { c.on = true; c.kelvin = 2700; }), R"({"state":true,"temp":2700})"},
        {"white beyond the range is clamped", &light, ch([](home::Change &c) { c.kelvin = 9000; }), R"({"state":true,"temp":6500})"},
        {"white below the range is clamped", &light, ch([](home::Change &c) { c.kelvin = 1000; }), R"({"state":true,"temp":2700})"},
        {"red at 30 %", &light, ch([](home::Change &c) { c.on = true; c.brightness = 30; c.hs = std::make_pair(0, 100); }),
         R"({"state":true,"dimming":30,"r":255,"g":0,"b":0,"w":0})"},
        {"plug on", &plug, ch([](home::Change &c) { c.on = true; }), R"({"state":true})"},
        {"plug off", &plug, ch([](home::Change &c) { c.on = false; }), R"({"state":false})"},
        {"fan on", &fan, ch([](home::Change &c) { c.on = true; }), R"({"fanState":1})"},
        {"fan off", &fan, ch([](home::Change &c) { c.on = false; }), R"({"fanState":0})"},
        {"fan at half speed", &fan, ch([](home::Change &c) { c.fan = 50; }), R"({"fanState":1,"fanSpeed":3})"},
        {"fan at 0 % stops", &fan, ch([](home::Change &c) { c.fan = 0; }), R"({"fanState":0})"},
        {"a dimmable bulb dims", &white, ch([](home::Change &c) { c.brightness = 70; }), R"({"state":true,"dimming":70})"},
    };
    for (const Case &c : cases) {
        std::string params, err;
        const bool ok = pilot_for(*c.thing, c.change, params, err);
        eq(std::string("setPilot: ") + c.name, ok ? params : "error: " + err, c.want);
    }

    const Case refused[] = {
        {"white and colour together", &light, ch([](home::Change &c) { c.kelvin = 2700; c.hs = std::make_pair(0, 100); }), ""},
        {"a dimmable bulb has no white", &white, ch([](home::Change &c) { c.kelvin = 2700; }), ""},
        {"a dimmable bulb has no colour", &white, ch([](home::Change &c) { c.hs = std::make_pair(0, 100); }), ""},
        {"a plug cannot be dimmed", &plug, ch([](home::Change &c) { c.on = true; c.brightness = 50; }), ""},
        {"a plug has no colour", &plug, ch([](home::Change &c) { c.hs = std::make_pair(0, 100); }), ""},
        {"a light has no fan", &light, ch([](home::Change &c) { c.fan = 50; }), ""},
        {"a fan cannot be dimmed", &fan, ch([](home::Change &c) { c.brightness = 50; }), ""},
        {"nothing asked", &light, home::Change(), ""},
        {"volume", &light, ch([](home::Change &c) { c.volume = 20; }), ""},
        {"a key", &plug, ch([](home::Change &c) { c.on = true; c.key = std::string("power"); }), ""},
        {"heating", &plug, ch([](home::Change &c) { c.target_c = 21.0; }), ""},
    };
    for (const Case &c : refused) {
        std::string params, err;
        const bool ok = pilot_for(*c.thing, c.change, params, err);
        check(std::string("refused: ") + c.name, !ok && !err.empty() && !reads_as_unreachable(err), ok ? params : err);
    }
    home::Thing stranger;
    stranger.ref = {{"k", "lock"}};
    std::string params, err;
    home::Change on;
    on.on = true;
    check("a thing of no known kind is refused", !pilot_for(stranger, on, params, err));
}

// ---------------------------------------------------------------------------------------------
// The driver against a device in this program: a UDP peer on 127.0.0.1 that answers each request
// with whatever the test's handler returns (any number of datagrams, in order).
class Peer {
public:
    using Handler = std::function<std::vector<std::string>(const json &request, const std::string &raw)>;
    explicit Peer(Handler h) : handler_(std::move(h)) {
        fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(fd_, reinterpret_cast<sockaddr *>(&a), sizeof a);
        socklen_t len = sizeof a;
        getsockname(fd_, reinterpret_cast<sockaddr *>(&a), &len);
        port_ = ntohs(a.sin_port);
        thread_ = std::thread([this] { run(); });
    }
    ~Peer() {
        stop_ = true;
        thread_.join();
        close(fd_);
    }
    int port() const { return port_; }
    std::vector<std::string> received() {
        std::lock_guard<std::mutex> lock(mtx_);
        return got_;
    }
    int count(const std::string &method) {
        int n = 0;
        for (const std::string &raw : received()) {
            const json j = json::parse(raw, nullptr, false);
            if (j.is_object() && j.value("method", "") == method) n++;
        }
        return n;
    }

private:
    void run() {
        std::string buf(65536, '\0');
        while (!stop_) {
            pollfd p{fd_, POLLIN, 0};
            if (poll(&p, 1, 20) <= 0) continue;
            sockaddr_in from{};
            socklen_t flen = sizeof from;
            const ssize_t n = recvfrom(fd_, &buf[0], buf.size(), 0, reinterpret_cast<sockaddr *>(&from), &flen);
            if (n <= 0) continue;
            const std::string raw(buf.data(), static_cast<size_t>(n));
            {
                std::lock_guard<std::mutex> lock(mtx_);
                got_.push_back(raw);
            }
            for (const std::string &out : handler_(json::parse(raw, nullptr, false), raw))
                sendto(fd_, out.data(), out.size(), 0, reinterpret_cast<sockaddr *>(&from), flen);
        }
    }
    Handler handler_;
    int fd_ = -1, port_ = 0;
    std::atomic<bool> stop_{false};
    std::mutex mtx_;
    std::vector<std::string> got_;
    std::thread thread_;
};

// The family's secrets as the app's store keeps them.
struct MemSecrets : home::Secrets {
    json all = json::object();
    json get(const std::string &family) const override { return all.contains(family) ? all[family] : json::object(); }
    void set(const std::string &family, const json &value) override { all[family] = value; }
};

// HMAC-SHA256 in base64, computed here apart from the driver's own code.
static std::string hmac_b64(const std::string &key, const std::string &text) {
    unsigned char mac[EVP_MAX_MD_SIZE];
    size_t len = 0;
    if (!EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, key.data(), key.size(),
                   reinterpret_cast<const unsigned char *>(text.data()), text.size(), mac, sizeof mac, &len))
        return "?";
    unsigned char out[EVP_MAX_MD_SIZE * 2];
    const int n = EVP_EncodeBlock(out, mac, static_cast<int>(len));
    return std::string(reinterpret_cast<char *>(out), n > 0 ? static_cast<size_t>(n) : 0);
}

static std::string reply(const std::string &method, const json &result) {
    return json{{"method", method}, {"env", "pro"}, {"result", result}}.dump();
}

// A bulb like fake_bulb.py's ESP01_SHRGB_03, firmware 1.25.0.
static std::vector<std::string> rgb_bulb(const json &req, const std::string &) {
    const std::string m = req.is_object() ? req.value("method", "") : "";
    if (m == "getSystemConfig") return {reply(m, {{"mac", "a8bb5006033d"}, {"moduleName", "ESP01_SHRGB_03"}, {"fwVersion", "1.25.0"}})};
    if (m == "getModelConfig") return {reply(m, {{"cctRange", {2200, 2700, 4800, 6500}}})};
    if (m == "getPilot") return {reply(m, {{"mac", "a8bb5006033d"}, {"state", true}, {"sceneId", 0}, {"temp", 2700}, {"dimming", 50}})};
    if (m == "setPilot") return {reply(m, {{"success", true}})};
    return {R"({"env":"pro","error":{"code":-32601,"message":"Method not found"}})"};
}

static home::Device at(int port) {
    home::Device d;
    d.id = "wiz:a8bb5006033d";
    d.kind = "wiz";
    d.ip = "127.0.0.1";
    d.port = port;
    return d;
}

static void test_driver() {
    auto driver = make_wiz_driver();
    MemSecrets secrets;
    driver->attach(&secrets);
    check("kind wiz, claims wiz only", std::string(driver->kind()) == "wiz" && driver->claims("wiz", {}) && !driver->claims("tapo", {}));
    const int fds_before = open_fds();

    {
        Peer bulb(rgb_bulb);
        home::Device dev = at(bulb.port());
        std::vector<home::Thing> things;
        std::string err;
        check("enumerate an RGB bulb", driver->enumerate(dev, things, err) && things.size() == 1, err);
        const home::Thing light = thing_of(things, "light");
        eq("its state", light.state.dump(), R"({"brightness":50,"kelvin":2700,"on":true})");
        check("the white range from getModelConfig", light.ref.value("kmin", 0) == 2200 && light.ref.value("kmax", 0) == 6500);
        eq("the module, cleaned", dev.model, "ESP01_SHRGB_03");
        check("no pairing asked of unsigned firmware", driver->next_step(dev).kind == home::Pairing::None);
        home::Change c;
        c.on = true;
        c.brightness = 25;
        check("apply", driver->apply(dev, light, c, 2500, err), err);
        const auto got = bulb.received();
        eq("what the bulb was sent", got.empty() ? "" : got.back(), R"({"method":"setPilot","params":{"state":true,"dimming":25}})");
        std::vector<home::Thing> some = {light};
        check("read", driver->read(dev, some, err) && some[0].state.value("on", false), err);
        dev.port = 1;   // nothing there
        const auto t0 = std::chrono::steady_clock::now();
        check("Device::port is honoured: nothing answers on port 1", !driver->apply(dev, light, c, 400, err) && reads_as_unreachable(err), err);
        check("and the deadline is kept", ms_since(t0) < 700, std::to_string(ms_since(t0)) + " ms");
    }

    {
        // Datagrams lost: the bulb answers only every third one it is sent.
        std::atomic<int> n{0};
        Peer lossy([&](const json &req, const std::string &raw) { return ++n % 3 == 0 ? rgb_bulb(req, raw) : std::vector<std::string>(); });
        home::Device dev = at(lossy.port());
        std::vector<home::Thing> things;
        std::string err;
        n = 2;   // the first request is answered at once
        check("enumerate through loss", driver->enumerate(dev, things, err) && things.size() == 1, err);
        const int before = lossy.count("setPilot");
        home::Change c;
        c.on = false;
        n = 0;
        check("apply through loss", driver->apply(dev, thing_of(things, "light"), c, 2500, err), err);
        eqi("setPilot sent three times: twice lost, then answered", lossy.count("setPilot") - before, 3);
    }

    {
        // Hostile and stray answers.
        std::string mode;
        Peer odd([&](const json &req, const std::string &raw) -> std::vector<std::string> {
            const std::string m = req.is_object() ? req.value("method", "") : "";
            if (mode == "late first") return {reply("getPilot", {{"state", false}}), "\x01\x02garbage", reply(m, {{"success", true}})};
            if (mode == "garbage only") return {"{not json", "[1,2,3]", "\"text\""};
            if (mode == "too long") return {reply(m, {{"success", true}, {"pad", std::string(9000, 'x')}})};
            if (mode == "padded") return {reply(m, {{"success", true}}) + std::string(9000, ' ')};
            if (mode == "other method only") return {reply(m == "getPilot" ? "setPilot" : "getPilot", {{"success", true}})};
            if (mode == "error") return {R"({"method":"setPilot","env":"pro","error":{"code":-32600,"message":"Invalid Request"}})"};
            if (mode == "no success") return {reply(m, {{"success", false}})};
            if (mode == "result not object") return {R"({"method":"setPilot","result":[true]})"};
            if (mode == "deep") return {std::string(4000, '[') + std::string(4000, ']')};
            return rgb_bulb(req, raw);
        });
        home::Device dev = at(odd.port());
        std::vector<home::Thing> things;
        std::string err;
        driver->enumerate(dev, things, err);
        const home::Thing light = thing_of(things, "light");
        home::Change c;
        c.on = true;
        struct Want { const char *mode; bool ok; const char *starts; };
        const Want wants[] = {
            {"late first", true, ""},
            {"garbage only", false, kGarbled},
            {"too long", false, kGarbled},
            {"padded", false, kGarbled},   // a whole answer, but longer than any answer is
            {"other method only", false, kNoAnswer},
            {"error", false, kRefused},
            {"no success", false, kRefused},
            {"result not object", false, kGarbled},
            {"deep", false, kGarbled},
        };
        for (const Want &w : wants) {
            mode = w.mode;
            err.clear();
            const bool ok = driver->apply(dev, light, c, 600, err);
            check(std::string("answer: ") + w.mode, ok == w.ok && err.compare(0, strlen(w.starts), w.starts) == 0, err);
        }
        mode = "other method only";
        std::vector<home::Thing> some = {light};
        err.clear();
        check("a read with no answer of its own", !driver->read(dev, some, err) && reads_as_unreachable(err), err);
        mode = "";
        home::Device nowhere = dev;
        nowhere.ip = "not-an-address.example";
        check("a name is never looked up: only addresses are contacted", !driver->apply(nowhere, light, c, 300, err) && reads_as_unreachable(err), err);
    }

    {
        // A module of no kind this driver exposes.
        Peer camera([](const json &req, const std::string &raw) -> std::vector<std::string> {
            if (req.is_object() && req.value("method", "") == "getSystemConfig")
                return {reply("getSystemConfig", {{"mac", "a8bb50000009"}, {"moduleName", "ESP99_CAMERA_01"}})};
            return rgb_bulb(req, raw);
        });
        home::Device dev = at(camera.port());
        std::vector<home::Thing> things;
        std::string err;
        check("an unknown module yields nothing", !driver->enumerate(dev, things, err) && things.empty() && !err.empty(), err);
    }

    {
        // Signing: a bulb like pywizlight's SignedBulb (firmware 1.39 on ESP25_SHORGB_01).
        const std::string key_hex = "00112233445566778899aabbccddeeff", wrong_hex = "ffeeddccbbaa99887766554433221100";
        const std::string key = bytes_of_hex(key_hex);
        Peer signed_bulb([&](const json &req, const std::string &raw) -> std::vector<std::string> {
            const std::string m = req.is_object() ? req.value("method", "") : "";
            auto signed_reply = [&](const char *field, json body) {
                body["sigTs"] = 1790441723;
                return json{{"method", m}, {"env", "pro"}, {field, body}, {"hmac", "AAAA"}}.dump();
            };
            if (m == "getSystemConfig") return {signed_reply("result", {{"mac", "a8bb5006033d"}, {"moduleName", "ESP25_SHORGB_01"}})};
            if (m == "getPilot") return {signed_reply("result", {{"state", false}, {"dimming", 10}, {"temp", 2700}})};
            if (m == "getModelConfig") return {signed_reply("result", {{"cctRange", {2700, 2700, 5000, 5000}}})};
            if (m != "setPilot") return {signed_reply("error", {{"code", -32601}, {"message", "Method not found"}})};
            const json p = req.value("params", json::object());
            if (!p.contains("sigTs")) return {signed_reply("error", {{"code", -32602}, {"message", "Invalid params"}})};
            // The signature must cover the params exactly as they were sent.
            const size_t from = raw.find("\"params\":"), to = raw.find(",\"hmac\":");
            const std::string body = from == std::string::npos || to == std::string::npos ? "" : raw.substr(from + 9, to - from - 9);
            if (req.value("hmac", "") != hmac_b64(key, body)) return {signed_reply("error", {{"code", -32605}, {"message", "Security error"}})};
            return {signed_reply("result", {{"success", true}})};
        });
        home::Device dev = at(signed_bulb.port());
        dev.id = "wiz:signed";
        std::vector<home::Thing> things;
        std::string err;
        check("enumerate signed firmware: reads need no key", driver->enumerate(dev, things, err) && things.size() == 1, err);
        const home::Thing light = thing_of(things, "light");
        check("its thing is marked signed", light.ref.value("signed", false));
        dev.things = things;
        home::PairStep step = driver->next_step(dev);
        check("the key is asked for", step.kind == home::Pairing::Token && step.field == "WiZ signing key" && !step.prompt.empty(), step.prompt);
        home::Change c;
        c.on = true;
        int sent = signed_bulb.count("setPilot");
        check("without a key nothing is sent", !driver->apply(dev, light, c, 800, err) && err == kNeedsKey && signed_bulb.count("setPilot") == sent, err);

        check("a key that is not one is refused", !driver->pair(dev, "123", err) && secrets.get("wiz").empty(), err);
        check("the wrong key is saved", driver->pair(dev, wrong_hex, err) && driver->next_step(dev).kind == home::Pairing::None, err);
        sent = signed_bulb.count("setPilot");
        check("the wrong key is refused by the device", !driver->apply(dev, light, c, 1500, err) && err == kKeyRefused, err);
        eqi("sent once", signed_bulb.count("setPilot") - sent, 1);
        check("the refusal is kept with the key", secrets.get("wiz").value("refused", false) == true &&
                                                     secrets.get("wiz").value("signing_key", "") == wrong_hex);
        check("and the key is asked for again", driver->next_step(dev).kind == home::Pairing::Token);
        sent = signed_bulb.count("setPilot");
        check("the refused key is not sent again", !driver->apply(dev, light, c, 800, err) && err == kKeyRefused &&
                                                       signed_bulb.count("setPilot") == sent, err);
        auto later = make_wiz_driver();   // a later run reads the same file
        later->attach(&secrets);
        check("not by a later run either", !later->apply(dev, light, c, 800, err) && err == kKeyRefused && signed_bulb.count("setPilot") == sent, err);
        check("errors never hold a key", err.find(wrong_hex) == std::string::npos && err.find(key_hex) == std::string::npos);

        check("the right key typed (the exported file pasted)", driver->pair(dev, "{\"udp_signing_key\":\"" + key_hex + "\"}", err), err);
        // A board without a battery-backed clock reads 1970 until it is set: nothing is signed then.
        set_unix_clock([]() -> long long { return 86400; });
        sent = signed_bulb.count("setPilot");
        check("a clock that is not set yet: nothing signed, nothing sent", !driver->apply(dev, light, c, 800, err) && err == kClockNotSet &&
                                                                            signed_bulb.count("setPilot") == sent, err);
        check("and the key is not marked as refused", !secrets.get("wiz").contains("refused"));
        set_unix_clock(nullptr);
        check("no refusal kept for it", !secrets.get("wiz").contains("refused") && secrets.get("wiz").value("signing_key", "") == key_hex);
        sent = signed_bulb.count("setPilot");
        check("a signed change goes through", driver->apply(dev, light, c, 1500, err), err);
        const auto got = signed_bulb.received();
        const json last = json::parse(got.back(), nullptr, false);
        check("it was signed", last.value("env", "") == "pro" && last.contains("hmac") && last["params"].contains("sigTs"), got.back());

        // A device not yet known to sign: unsigned first, then the saved key once.
        home::Thing unmarked = light;
        unmarked.ref.erase("signed");
        home::Device fresh = dev;
        fresh.id = "wiz:fresh";
        fresh.things.clear();
        sent = signed_bulb.count("setPilot");
        check("firmware that started signing: the saved key is used", driver->apply(fresh, unmarked, c, 2000, err), err);
        eqi("one unsigned request, then one signed", signed_bulb.count("setPilot") - sent, 2);
    }

    {
        // Unsigned firmware turning down a request is not a key problem.
        Peer old([](const json &req, const std::string &raw) -> std::vector<std::string> {
            if (req.is_object() && req.value("method", "") == "setPilot")
                return {R"({"method":"setPilot","env":"pro","error":{"code":-32602,"message":"Invalid params"}})"};
            return rgb_bulb(req, raw);
        });
        home::Device dev = at(old.port());
        dev.id = "wiz:old";
        std::vector<home::Thing> things;
        std::string err;
        driver->enumerate(dev, things, err);
        home::Change c;
        c.on = true;
        check("-32602 from firmware that does not sign is a plain refusal", !driver->apply(dev, thing_of(things, "light"), c, 800, err) &&
                                                                              err == kRefused, err);
        bool any_signed = false;
        for (const std::string &raw : old.received()) any_signed = any_signed || raw.find("\"hmac\"") != std::string::npos;
        check("sent once, and the saved key was not tried on it", old.count("setPilot") == 1 && !any_signed);
        secrets.all.erase("wiz");
        check("and asks for no key", driver->next_step(dev).kind == home::Pairing::None);
    }

    check("no socket is left open between calls", open_fds() == fds_before,
          std::to_string(fds_before) + " before, " + std::to_string(open_fds()) + " after");

    for (const char *e : {kGarbled, kNeedsKey, kKeyRefused, kClockNotSet, kRefused})
        check(std::string("not read as unreachable: ") + e, !reads_as_unreachable(e));
    check("\"did not answer\" is read as unreachable", reads_as_unreachable(kNoAnswer));
}

int main() {
    test_signing();
    test_models();
    test_scales();
    test_things_and_state();
    test_pilots();
    test_driver();
    printf("%d checks, %d failed\n", g_run, g_failed);
    return g_failed ? 1 : 0;
}
