// Known-answer and offline tests for the Hue driver. Prints one line per check and exits non-zero
// when any fails. Nothing here sends anything over a network: the driver is only called where it
// must refuse before sending.
//   hue_test [folder holding certs/]      (default: tests/mocks/hue)
//
// Known answers and where they come from:
//  - certificate pins: certs/pins.txt, computed by the openssl command-line tool (make_certs.sh),
//    independently of the code under test;
//  - the published wide-gamut primaries of Hue lights, red (0.7006, 0.2993), green (0.1724,
//    0.7468), blue (0.1355, 0.0399), which full-saturation red, green and blue must give;
//  - mirek = 1,000,000 / kelvin (Signify's definition of the unit);
//  - SHA-256 of the sample key from Signify's "Get started" page, by sha256sum;
//  - reply and resource shapes from aiohue's fixture recorded from a real bridge, the openhue-api
//    description, and Signify's "Get started" page.
#include "smarthome/drivers/hue.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

using hue::json;
using namespace hue::detail;

static int g_checks = 0, g_failed = 0;

static void check(bool ok, const std::string &what) {
    ++g_checks;
    if (!ok) ++g_failed;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
}

static std::string slurp(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static home::Thing find(const std::vector<home::Thing> &v, const std::string &id) {
    for (const auto &t : v)
        if (t.id == id) return t;
    return home::Thing();
}

static bool has_thing(const std::vector<home::Thing> &v, const std::string &id) {
    for (const auto &t : v)
        if (t.id == id) return true;
    return false;
}

// The family's secrets as the app's store keeps them, in memory.
struct MemorySecrets : home::Secrets {
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

// ---------------------------------------------------------------- text
static void test_text() {
    check(sanitize_utf8("caf\xc3\xa9") == "caf\xc3\xa9", "sanitize_utf8 keeps valid UTF-8");
    check(sanitize_utf8("a\xff" "b") == "a?b", "sanitize_utf8 replaces a stray byte");
    check(sanitize_utf8("\xc0\x80") == "??", "sanitize_utf8 refuses an overlong form");
    check(sanitize_utf8("\xed\xa0\x80") == "???", "sanitize_utf8 refuses a UTF-16 surrogate");
    check(sanitize_utf8("\xe2\x82") == "??", "sanitize_utf8 refuses a truncated sequence");
    check(sanitize_utf8("\xf0\x9f\x92\xa1") == "\xf0\x9f\x92\xa1", "sanitize_utf8 keeps a 4-byte character");

    check(clean_name("  Sofa\tlamp \n") == "Sofa lamp", "clean_name trims and turns tabs and newlines into spaces");
    check(clean_name("A\x01" "B\x7f" "C\x1b[31m") == "ABC[31m", "clean_name drops C0 controls and DEL");
    check(clean_name("x\xc2\x85y\xc2\x9fz") == "xyz", "clean_name drops C1 controls");
    check(clean_name("evil\xe2\x80\xaeoc.exe") == "eviloc.exe", "clean_name drops a right-to-left override");
    check(clean_name("a\xe2\x81\xa6" "b\xef\xbb\xbf" "c") == "abc", "clean_name drops isolates and a byte-order mark");
    check(clean_name(std::string(60, 'x')).size() == 40, "clean_name caps at 40 bytes");
    {
        // 39 ASCII bytes and a 2-byte character would straddle the cap: the character is dropped whole.
        const std::string s = std::string(39, 'a') + "\xc3\xa9" + "zz";
        const std::string c = clean_name(s);
        check(c == std::string(39, 'a'), "clean_name never cuts a UTF-8 character");
    }
    check(clean_name("a    b") == "a b", "clean_name squeezes runs of spaces");
    check(clean_name("\xff\xfe") == "??", "clean_name repairs bytes that are not UTF-8");
    check(clean_name("") == "", "clean_name of nothing is nothing");

    check(!too_deep("{\"a\":[1,2,{\"b\":3}]}"), "too_deep accepts ordinary nesting");
    check(too_deep(std::string(49, '[') + std::string(49, ']')), "too_deep refuses 49 levels");
    check(!too_deep(std::string(48, '[') + std::string(48, ']')), "too_deep accepts 48 levels");
    check(!too_deep("\"" + std::string(200, '[') + "\""), "too_deep ignores brackets inside strings");
    check(!too_deep("[\"\\\"[[[[\"]"), "too_deep handles escaped quotes");

    check(safe_id("8c7f4b4e-6c2b-4b50-9c0f-3a1b2c3d4e5f"), "safe_id accepts a UUID");
    check(!safe_id("../api") && !safe_id("a b") && !safe_id("") && !safe_id(std::string(65, 'a')) && !safe_id("a\r\nb"),
          "safe_id refuses paths, spaces, empty, too long, CR LF");
    check(safe_key("1028d66426293e821ecfd9ef1a0731df"), "safe_key accepts the key from Signify's Get started page");
    check(!safe_key("abc\r\nX-Evil: 1") && !safe_key("") && !safe_key(std::string(129, 'k')), "safe_key refuses header injection, empty, too long");
    check(safe_pin("sha256//S4p40dB26XlmuqWFim48xJGQRLm4jjAVv4JeY32tn6U="), "safe_pin accepts a pin");
    check(!safe_pin("sha256//S4p40dB26XlmuqWFim48xJGQRLm4jjAVv4JeY32tn6U") && !safe_pin("sha1//abc") &&
              !safe_pin("sha256//S4p40dB26Xlmu;WFim48xJGQRLm4jjAVv4JeY32tn6U="),
          "safe_pin refuses a short pin, another hash, a bad character");

    check(normalize_bridge_id("001788FFFE23BFC2") == "00178823bfc2", "bridge id: 16 digits upper case");
    check(normalize_bridge_id("001788fffe23bfc2") == "00178823bfc2", "bridge id: 16 digits lower case (mDNS TXT)");
    check(normalize_bridge_id("00:17:88:23:bf:c2") == "00178823bfc2", "bridge id: MAC form");
    check(normalize_bridge_id("00178823BFC2") == "00178823bfc2", "bridge id: 12 digits");
    check(normalize_bridge_id("Hue Bridge").empty() && normalize_bridge_id("12345").empty(), "bridge id: not an id");
}

// ---------------------------------------------------------------- certificates
static void test_certs(const std::string &dir) {
    std::map<std::string, std::string> pins;
    {
        std::ifstream f(dir + "/certs/pins.txt");
        std::string name, pin;
        while (f >> name >> pin) pins[name] = pin;
    }
    check(pins.size() == 6, "certs/pins.txt holds six known pins");
    for (const char *n : {"a", "b", "rsa", "other", "san", "chain"}) {
        const std::string pem = slurp(dir + "/certs/" + n + ".pem");
        const std::string pin = pin_from_pem(pem);
        check(!pem.empty() && pin == pins[n], std::string("pin of certs/") + n + ".pem matches openssl's (" + pins[n] + ")");
        check(safe_pin(pin), std::string("pin of certs/") + n + ".pem is well formed");
    }
    const std::string a = slurp(dir + "/certs/a.pem"), b = slurp(dir + "/certs/b.pem"), other = slurp(dir + "/certs/other.pem"),
                      san = slurp(dir + "/certs/san.pem"), chain = slurp(dir + "/certs/chain.pem");
    check(pin_from_pem(a) != pin_from_pem(b), "two keys, two pins");
    const auto an = names_from_pem(a);
    check(an.size() == 1 && an[0] == "00005efffe005301", "names_from_pem reads the common name");
    const auto sn = names_from_pem(san);
    check(sn.size() == 2 && sn[0] == "Hue Bridge" && sn[1] == "00005efffe005301", "names_from_pem reads a DNS subjectAltName");
    check(cert_names_bridge(a, "00005EFFFE005301"), "the certificate names the bridge (id as /api/0/config writes it)");
    check(cert_names_bridge(a, "00:00:5e:00:53:01"), "the certificate names the bridge (id as a MAC)");
    check(cert_names_bridge(san, "00005efffe005301"), "a subjectAltName also counts");
    check(cert_names_bridge(chain, "00005efffe005301"), "the leaf of a chain names the bridge");
    check(!cert_names_bridge(other, "00005efffe005301"), "another bridge's certificate does not");
    check(!cert_names_bridge(a, "") && !cert_names_bridge(a, "Hue Bridge"), "no id, no match");
    check(pin_from_pem("not a certificate").empty() && names_from_pem("").empty(), "garbage gives no pin and no names");
    check(pin_from_pem(a.substr(0, a.size() / 2)).empty(), "a truncated PEM gives no pin");
}

// ---------------------------------------------------------------- colour
static json colour_light(double x, double y) {
    return {{"on", {{"on", true}}}, {"color", {{"xy", {{"x", x}, {"y", y}}}}},
            {"color_temperature", {{"mirek", nullptr}, {"mirek_valid", false}}}};
}

static void test_colour() {
    check(mirek_from_kelvin(2700) == 370, "2700 K is 370 mirek");
    check(mirek_from_kelvin(6500) == 154, "6500 K is 154 mirek");
    check(mirek_from_kelvin(2000) == 500, "2000 K is 500 mirek");
    check(mirek_from_kelvin(1000) == 500, "1000 K is kept within 500 mirek");
    check(mirek_from_kelvin(20000) == 153, "20000 K is kept within 153 mirek");
    check(mirek_from_kelvin(2700, 153, 454) == 370, "2700 K within a 153..454 light");
    check(mirek_from_kelvin(2000, 153, 454) == 454, "2000 K on a 153..454 light is its warmest");
    check(mirek_from_kelvin(1000, 50, 1000) == 1000 && mirek_from_kelvin(20000, 50, 1000) == 50, "a 50..1000 light's range");
    check(mirek_from_kelvin(-5) == 500 && mirek_from_kelvin(0) == 500, "nonsense kelvin stays in range");
    check(kelvin_from_mirek(370) == 2703 && kelvin_from_mirek(153) == 6536, "mirek back to kelvin");
    check(kelvin_from_mirek(0) == 0 && kelvin_from_mirek(NAN) == 0 && kelvin_from_mirek(-3) == 0 && kelvin_from_mirek(1e300) == 0,
          "nonsense mirek gives 0");

    double x = 0, y = 0;
    hs_to_xy(0, 100, x, y);
    check(x == 0.7006 && y == 0.2993, "red is the wide-gamut red primary (0.7006, 0.2993), to 4 decimals");
    hs_to_xy(120, 100, x, y);
    check(x == 0.1724 && y == 0.7468, "green is the wide-gamut green primary (0.1724, 0.7468)");
    hs_to_xy(240, 100, x, y);
    check(x == 0.1355 && y == 0.0399, "blue is the wide-gamut blue primary (0.1355, 0.0399)");
    hs_to_xy(360 + 240, 100, x, y);
    double x2 = 0, y2 = 0;
    hs_to_xy(240, 100, x2, y2);
    check(x == x2 && y == y2, "hue wraps at 360");
    hs_to_xy(NAN, 1e9, x, y);
    check(x >= 0 && x <= 1 && y >= 0 && y <= 1, "nonsense hue and saturation stay in the diagram");
    for (const auto &hs : std::vector<std::pair<int, int>>{{0, 100}, {30, 100}, {60, 100}, {200, 80}, {275, 100}, {330, 50}, {10, 30}}) {
        hs_to_xy(hs.first, hs.second, x, y);
        const json st = light_state(colour_light(x, y));
        const int h = st.value("hue", -1000), s = st.value("saturation", -1000);
        int dh = std::abs(h - hs.first);
        dh = std::min(dh, 360 - dh);
        check(dh <= 2 && std::abs(s - hs.second) <= 2, "hue " + std::to_string(hs.first) + ", saturation " +
                                                           std::to_string(hs.second) + " survives xy and back (" +
                                                           std::to_string(h) + ", " + std::to_string(s) + ")");
    }
    json st = light_state(colour_light(0.3227, 0.3290));
    check(st.value("saturation", 100) <= 1, "the wide-gamut matrix's white point has no saturation");
    st = light_state(colour_light(0.5, 0));
    check(!st.contains("hue") && !st.contains("saturation"), "y = 0 gives no colour");
    st = light_state(colour_light(7, 0.3));
    check(!st.contains("hue"), "x outside the diagram gives no colour");
    st = light_state({{"on", {{"on", true}}}, {"color_temperature", {{"mirek", 250}, {"mirek_valid", true}}},
                      {"color", {{"xy", {{"x", 0.4}, {"y", 0.4}}}}}});
    check(st.value("kelvin", 0) == 4000 && !st.contains("hue"), "a light in white mode reports kelvin, not a colour");
}

// ---------------------------------------------------------------- replies
static void test_replies() {
    std::string key;
    check(read_link_reply("[{\"error\":{\"type\":101,\"address\":\"\",\"description\":\"link button not pressed\"}}]", key) ==
              LinkReply::NotPressed,
          "POST /api before the button: link button not pressed");
    check(read_link_reply("[{\"success\":{\"username\":\"1028d66426293e821ecfd9ef1a0731df\"}}]", key) == LinkReply::Key &&
              key == "1028d66426293e821ecfd9ef1a0731df",
          "POST /api after the button: the key");
    check(read_link_reply("[{\"success\":{\"username\":\"k\\r\\nX: y\"}}]", key) == LinkReply::Unreadable && key.empty(),
          "a key that could break a header is refused");
    check(read_link_reply("[{\"error\":{\"type\":7,\"description\":\"invalid value\"}}]", key) == LinkReply::Refused,
          "another error type is a refusal (not retried)");
    check(read_link_reply("[{\"error\":{\"type\":\"101\"}}]", key) == LinkReply::Refused, "a type of the wrong JSON type is not 101");
    check(read_link_reply("[]", key) == LinkReply::Unreadable && read_link_reply("{}", key) == LinkReply::Unreadable &&
              read_link_reply("<html>", key) == LinkReply::Unreadable && read_link_reply("", key) == LinkReply::Unreadable &&
              read_link_reply("[{\"success\":{}}]", key) == LinkReply::Unreadable,
          "anything else is unreadable");

    Config c;
    check(read_config("{\"name\":\"Hue Bridge\",\"datastoreversion\":\"172\",\"swversion\":\"1971060010\",\"apiversion\":\"1.71.0\","
                      "\"mac\":\"00:00:5e:00:53:01\",\"bridgeid\":\"00005EFFFE005301\",\"factorynew\":false,"
                      "\"replacesbridgeid\":null,\"modelid\":\"BSB002\",\"starterkitid\":\"\"}",
                      c) &&
              c.bridgeid == "00005EFFFE005301" && c.modelid == "BSB002" && c.swversion == 1971060010LL,
          "read_config reads /api/0/config");
    check(!read_config("{\"name\":\"x\"}", c), "read_config needs a bridge id");
    check(read_config("{\"bridgeid\":\"00005EFFFE005301\",\"swversion\":1971060010}", c) && c.swversion == -1,
          "a swversion that is not a string of digits counts as unknown");
    check(read_config("{\"bridgeid\":\"00005EFFFE005301\",\"swversion\":\"99999999999999999999999\"}", c) && c.swversion == -1,
          "an absurdly long swversion counts as unknown");

    json j;
    check(parse_json("{\"a\":\"caf\xff\"}", j) && j["a"] == "caf?", "parse_json repairs a byte that is not UTF-8");
    check(!parse_json(std::string(100, '[') + std::string(100, ']'), j), "parse_json refuses deep nesting");
    check(!parse_json("{\"data\":[1,2", j) && !parse_json("", j), "parse_json refuses truncated and empty text");
    bool threw = false;
    try {
        parse_json("{\"n\":1e999,\"m\":-1e999}", j);
    } catch (...) {
        threw = true;
    }
    check(!threw, "parse_json does not throw on an out-of-range number");
}

// ---------------------------------------------------------------- things
static const char *kSnapshot = R"JSON({
 "devices": [
  {"id":"dev-sofa","type":"device","metadata":{"name":"Sofa lamp device","archetype":"table_shade"},
   "product_data":{"model_id":"LCA001","product_archetype":"sultan_bulb"},
   "services":[{"rid":"light-sofa","rtype":"light"},{"rid":"zc-1","rtype":"zigbee_connectivity"}]},
  {"id":"dev-ceiling","type":"device","metadata":{"name":"Ceiling","archetype":"ceiling_round"},
   "services":[{"rid":"light-ceiling","rtype":"light"}]},
  {"id":"dev-plug","type":"device","metadata":{"name":"Fan heater","archetype":"plug"},
   "product_data":{"product_archetype":"plug"},"services":[{"rid":"light-plug","rtype":"light"}]},
  {"id":"dev-cam","type":"device","metadata":{"name":"Garden camera","archetype":"unknown_archetype"},
   "services":[{"rid":"light-cam","rtype":"light"},{"rid":"cm-1","rtype":"camera_motion"},{"rid":"t-cam","rtype":"temperature"}]},
  {"id":"dev-sensor","type":"device","metadata":{"name":"Hall sensor","archetype":"unknown_archetype"},
   "services":[{"rid":"temp-1","rtype":"temperature"},{"rid":"motion-1","rtype":"motion"},{"rid":"ll-1","rtype":"light_level"}]},
  {"id":"dev-chime","type":"device","metadata":{"name":"Chime"},"services":[{"rid":"spk-1","rtype":"speaker"}]},
  {"id":"dev-bridge","type":"device","metadata":{"name":"My \u0007Bridge","archetype":"bridge_v2"},
   "product_data":{"model_id":"BSB002"},"services":[{"rid":"br-1","rtype":"bridge"}]},
  {"id":"dev-lantern","type":"device","metadata":{"name":"Lantern"},"services":[{"rid":"light-lantern","rtype":"light"}]},
  {"id":"dev-white","type":"device","metadata":{"name":"Hall light"},"services":[{"rid":"light-white","rtype":"light"}]},
  "not an object",
  {"id":"../bad","type":"device","metadata":{"name":"bad id"}}
 ],
 "lights": [
  {"id":"light-sofa","type":"light","owner":{"rid":"dev-sofa","rtype":"device"},
   "metadata":{"name":"(set below)","archetype":"table_shade"},"on":{"on":true},
   "dimming":{"brightness":0.2,"min_dim_level":0.2},
   "color_temperature":{"mirek":null,"mirek_valid":false,"mirek_schema":{"mirek_minimum":153,"mirek_maximum":500}},
   "color":{"xy":{"x":0.7006,"y":0.2993},"gamut_type":"C"},"mode":"normal"},
  {"id":"light-ceiling","type":"light","owner":{"rid":"dev-ceiling","rtype":"device"},
   "metadata":{"name":"Ceiling","archetype":"ceiling_round"},"on":{"on":false},"dimming":{"brightness":62.06},
   "color_temperature":{"mirek":370,"mirek_valid":true,"mirek_schema":{"mirek_minimum":153,"mirek_maximum":454}}},
  {"id":"light-plug","type":"light","owner":{"rid":"dev-plug","rtype":"device"},
   "metadata":{"name":"Fan heater","archetype":"plug"},"on":{"on":false}},
  {"id":"light-cam","type":"light","owner":{"rid":"dev-cam","rtype":"device"},"metadata":{"name":"Floodlight"},"on":{"on":true}},
  {"id":"light-lantern","type":"light","owner":{"rid":"dev-lantern","rtype":"device"},"metadata":{"name":"Lantern"},
   "on":{"on":true},"dimming":{"brightness":100},
   "color_temperature":{"mirek":null,"mirek_valid":false,"mirek_schema":{"mirek_minimum":0,"mirek_maximum":65535}},
   "color":{"xy":{"x":0.1355,"y":0.0399}}},
  {"id":"light-white","type":"light","owner":{"rid":"dev-white","rtype":"device"},"metadata":{"name":"Hall light"},
   "on":{"on":true},"dimming":{"brightness":50}},
  {"id":"light-bad","type":"light","owner":{"rid":"dev-white","rtype":"device"},"metadata":{"name":42},"on":"yes"},
  {"id":"light-nameless","type":"light","owner":{"rid":"dev-white","rtype":"device"},"metadata":{"name":42},"on":{"on":false}}
 ],
 "rooms": [
  {"id":"room-living","type":"room","metadata":{"name":"Living room","archetype":"living_room"},
   "children":[{"rid":"dev-sofa","rtype":"device"},{"rid":"dev-ceiling","rtype":"device"},{"rid":"dev-plug","rtype":"device"}],
   "services":[{"rid":"gl-living","rtype":"grouped_light"}]},
  {"id":"room-hall","type":"room","metadata":{"name":"Hall"},
   "children":[{"rid":"dev-white","rtype":"device"},{"rid":"dev-sensor","rtype":"device"},{"rid":"dev-cam","rtype":"device"}],
   "services":[{"rid":"gl-hall","rtype":"grouped_light"}]},
  {"id":"room-kitchen","type":"room","metadata":{"name":"Kitchen"},"children":[],"services":[]}
 ],
 "zones": [
  {"id":"zone-out","type":"zone","metadata":{"name":"Outside"},"children":[{"rid":"light-lantern","rtype":"light"}],
   "services":[{"rid":"gl-out","rtype":"grouped_light"}]},
  {"id":"zone-down","type":"zone","metadata":{"name":"Downstairs"},
   "children":[{"rid":"light-sofa","rtype":"light"},{"rid":"light-ceiling","rtype":"light"}],
   "services":[{"rid":"gl-down","rtype":"grouped_light"}]}
 ],
 "homes": [
  {"id":"home-1","type":"bridge_home","children":[{"rid":"room-living","rtype":"room"}],
   "services":[{"rid":"gl-home","rtype":"grouped_light"}]}
 ],
 "scenes": [
  {"id":"scene-relax","type":"scene","metadata":{"name":"Relax"},"group":{"rid":"room-living","rtype":"room"},
   "actions":[{"target":{"rid":"light-sofa","rtype":"light"},"action":{"on":{"on":true}}}]},
  {"id":"scene-out","type":"scene","metadata":{"name":"Evening"},"group":{"rid":"zone-out","rtype":"zone"},"actions":[]},
  {"id":"scene-cam","type":"scene","metadata":{"name":"Alarm lights"},"group":{"rid":"room-hall","rtype":"room"},
   "actions":[{"target":{"rid":"light-cam","rtype":"light"},"action":{"on":{"on":true}}}]},
  {"id":"scene-noname","type":"scene","metadata":{},"group":{"rid":"room-hall","rtype":"room"},"actions":[]}
 ],
 "temperatures": [
  {"id":"temp-1","type":"temperature","owner":{"rid":"dev-sensor","rtype":"device"},"enabled":true,
   "temperature":{"temperature":22.55,"temperature_report":{"changed":"2026-07-27T18:50:19.724Z","temperature":21.37},
   "temperature_valid":true}},
  {"id":"t-cam","type":"temperature","owner":{"rid":"dev-cam","rtype":"device"},"enabled":true,
   "temperature":{"temperature":5.0,"temperature_valid":true}}
 ],
 "motions": [
  {"id":"motion-1","type":"motion","owner":{"rid":"dev-sensor","rtype":"device"},"enabled":false,
   "motion":{"motion":true,"motion_valid":true}}
 ]
})JSON";

static void test_split() {
    // As GET /clip/v2/resource answers: every type in one list, in any order.
    const json data = json::parse(R"([
        {"id":"l1","type":"light"},{"id":"d1","type":"device"},{"id":"r1","type":"room"},{"id":"z1","type":"zone"},
        {"id":"s1","type":"scene"},{"id":"h1","type":"bridge_home"},{"id":"t1","type":"temperature"},
        {"id":"m1","type":"motion"},{"id":"g1","type":"grouped_light"},{"id":"c1","type":"camera_motion"},
        {"id":"l2","type":"light"},"junk",17,null,{"type":5},{"id":"x"}])");
    Snapshot s;
    split_resources(data, s);
    check(s.lights.size() == 2 && s.devices.size() == 1 && s.rooms.size() == 1 && s.zones.size() == 1 && s.scenes.size() == 1 &&
              s.homes.size() == 1 && s.temperatures.size() == 1 && s.motions.size() == 1,
          "split_resources sorts one list of every type into the snapshot");
    split_resources(json::object({{"data", 1}}), s);
    check(s.lights.empty() && s.devices.empty(), "split_resources of something that is not a list is empty");
    json many = json::array();
    for (int i = 0; i < 2100; ++i) many.push_back({{"id", "l" + std::to_string(i)}, {"type", "light"}});
    split_resources(many, s);
    check(s.lights.size() == 2000, "split_resources keeps at most 2000 of a type");
}

static void test_things() {
    json src = json::parse(kSnapshot);
    // A name with a right-to-left override in it (U+202E), as a bridge could hold one.
    src["lights"][0]["metadata"]["name"] = " Sofa\xe2\x80\xaelamp ";
    Snapshot s;
    s.devices = src["devices"];
    s.lights = src["lights"];
    s.rooms = src["rooms"];
    s.zones = src["zones"];
    s.homes = src["homes"];
    s.scenes = src["scenes"];
    s.temperatures = src["temperatures"];
    s.motions = src["motions"];
    std::vector<home::Thing> things;
    BridgeInfo info;
    build_things(s, things, info);

    check(info.name == "My Bridge" && info.model == "BSB002", "the bridge's own name and model, cleaned");
    const home::Thing sofa = find(things, "light-sofa");
    check(sofa.name == "Sofalamp" && sofa.type == "light", "a light is named from its metadata, cleaned");
    check(sofa.caps == (home::kPower | home::kBrightness | home::kColorTemp | home::kColor), "a colour light gets all four light caps");
    check(sofa.room == "Living room", "a light's room comes from its device's room");
    check(sofa.state["on"] == true && sofa.state["brightness"] == 1, "the lowest dim level reads as 1 percent, still on");
    check(sofa.state.contains("hue") && sofa.state["hue"] == 0 && sofa.state["saturation"] == 100 && !sofa.state.contains("kelvin"),
          "a light in colour mode reports hue and saturation");
    const json &groups = sofa.ref["groups"];
    check(groups.is_array() && groups.size() == 3, "the light knows its room, zone and home groups");
    check(groups.size() == 3 && groups[0]["rtype"] == "room" && groups[0]["gl"] == "gl-living" && groups[0]["n"] == 3,
          "its room's grouped_light reaches 3 lights (the plug counts)");
    check(groups.size() == 3 && groups[1]["rtype"] == "zone" && groups[1]["gl"] == "gl-down" && groups[1]["n"] == 2,
          "its zone's grouped_light reaches 2 lights");
    check(groups.size() == 3 && groups[2]["rtype"] == "bridge_home" && groups[2]["n"] == 8,
          "the home group reaches every light, the camera's and the malformed one included");
    check(sofa.ref["kind"] == "light" && sofa.ref["rid"] == "light-sofa" && sofa.ref["device"] == "dev-sofa", "light ref");

    const home::Thing ceiling = find(things, "light-ceiling");
    check(ceiling.caps == (home::kPower | home::kBrightness | home::kColorTemp), "a white-ambiance light has no colour cap");
    check(ceiling.state["on"] == false && ceiling.state["brightness"] == 62 && ceiling.state["kelvin"] == 2703,
          "a light in white mode reports kelvin");
    check(ceiling.ref["mirek"] == json::array({153, 454}), "its mirek range is kept for clamping");

    const home::Thing plug = find(things, "light-plug");
    check(plug.type == "plug" && plug.caps == home::kPower && plug.room == "Living room", "a smart plug is a plug with only power");

    const home::Thing white = find(things, "light-white");
    check(white.caps == (home::kPower | home::kBrightness) && white.room == "Hall", "a dimmable white light");

    const home::Thing lantern = find(things, "light-lantern");
    check(lantern.room == "Outside", "a light in no room takes its zone's name");
    check(lantern.ref["mirek"] == json::array({153, 500}), "a 0..65535 mirek schema is corrected to 153..500");
    check(lantern.state["hue"] == 240 && lantern.state["saturation"] == 100, "blue primary reads back as hue 240");

    check(!has_thing(things, "light-cam"), "a camera's floodlight is never exposed");
    check(!has_thing(things, "light-bad"), "a light whose 'on' is not an object is ignored");
    const home::Thing nameless = find(things, "light-nameless");
    check(nameless.name == "Hall light", "a light without a usable name takes its device's name");

    const home::Thing relax = find(things, "scene-relax");
    check(relax.type == "scene" && relax.caps == home::kScene && relax.room == "Living room" && relax.ref["rid"] == "scene-relax",
          "a scene is a thing in its room");
    check(find(things, "scene-out").room == "Outside", "a zone's scene is in the zone");
    check(!has_thing(things, "scene-cam"), "a scene that includes a camera's light is never exposed");
    check(!has_thing(things, "scene-noname"), "a scene without a name is ignored");

    const home::Thing sensor = find(things, "dev-sensor");
    check(sensor.type == "sensor" && sensor.caps == home::kSensor && sensor.room == "Hall" && sensor.name == "Hall sensor",
          "one sensor thing per sensor device");
    check(sensor.state["current_c"] == 21.4 && !sensor.state.contains("motion"),
          "temperature from the newer report, rounded; a disabled motion service is not reported");
    check(sensor.ref["temperature"] == "temp-1" && sensor.ref["motion"] == "motion-1", "sensor ref");
    check(!has_thing(things, "dev-cam") && !has_thing(things, "dev-chime"), "cameras and chimes are not sensors either");
    check(!has_thing(things, "dev-bridge") && !has_thing(things, "../bad"), "the bridge and a bad id are not things");
    size_t lights = 0, scenes = 0, sensors = 0;
    for (const auto &t : things) {
        lights += t.type == "light" || t.type == "plug";
        scenes += t.type == "scene";
        sensors += t.type == "sensor";
    }
    check(lights == 6 && scenes == 2 && sensors == 1, "6 lights and plugs, 2 scenes, 1 sensor");
    static const std::set<std::string> kAllowed = {"light", "plug", "scene", "sensor"};
    bool allowed = true;
    for (const auto &t : things) allowed = allowed && kAllowed.count(t.type);
    check(allowed, "every thing is of a type on the allow-list");

    // Hostile shapes: wrong types everywhere must not throw.
    Snapshot bad;
    bad.devices = json::parse(R"([{"id":5},{"id":"d","services":"x"},{"id":"e","services":[{"rid":1,"rtype":null}]}])");
    bad.lights = json::parse(R"([{"id":"l","on":{"on":true},"dimming":{"brightness":"50"},"color":{"xy":{"x":"a"}},
                               "color_temperature":{"mirek_schema":{"mirek_minimum":"x","mirek_maximum":1e300}}},null,7,"s"])");
    bad.rooms = json::parse(R"([{"id":"r","children":{"a":1},"services":7,"metadata":[]}])");
    bad.zones = json::parse(R"([{"id":"z","children":[null,{"rid":"l"}]}])");
    bad.scenes = json::parse(R"([{"id":"s","metadata":{"name":"x"},"actions":"all"}])");
    bad.homes = json::parse(R"(["home"])");
    std::vector<home::Thing> few;
    bool threw = false;
    try {
        build_things(bad, few, info);
    } catch (...) {
        threw = true;
    }
    check(!threw, "wrong JSON types everywhere do not throw");
    const home::Thing l = find(few, "l");
    check(l.id == "l" && l.caps == (home::kPower | home::kColorTemp) && l.ref["mirek"] == json::array({153, 500}),
          "a light with wrong-typed fields keeps only what is well formed");
    check(!has_thing(few, "s"), "a scene whose members cannot be read is not offered");
}

// ---------------------------------------------------------------- changes
static home::Thing light(const std::string &id, uint32_t caps, const json &groups = json::array(), const char *type = "light",
                         json mirek = json()) {
    home::Thing t;
    t.id = id;
    t.type = type;
    t.caps = caps;
    t.ref = {{"kind", "light"}, {"rid", id}, {"device", "d-" + id}, {"groups", groups}};
    if (!mirek.is_null()) t.ref["mirek"] = mirek;
    return t;
}

static const char *const kUnreachableWords[] = {"connect", "reach", "answer", "respond", "timed out", "timeout", "no route", "unreachable"};

static bool reads_as_no_answer(const std::string &e) {
    for (const char *w : kUnreachableWords)
        if (e.find(w) != std::string::npos) return true;
    return false;
}

static void test_changes() {
    const uint32_t all = home::kPower | home::kBrightness | home::kColorTemp | home::kColor;
    const home::Thing colour = light("l1", all, json::array(), "light", {153, 454});
    const home::Thing white = light("l2", home::kPower | home::kBrightness);
    const home::Thing plug = light("p1", home::kPower, json::array(), "plug");
    json body;
    std::string why;
    home::Change c;

    c = home::Change();
    c.on = true;
    check(light_body(colour, c, body, why) && body == json::parse(R"({"on":{"on":true}})"), "on");
    c.brightness = 40;
    check(light_body(colour, c, body, why) && body == json::parse(R"({"on":{"on":true},"dimming":{"brightness":40}})"),
          "on at 40 percent, one request");
    c = home::Change();
    c.brightness = 0;
    check(light_body(colour, c, body, why) && body == json::parse(R"({"on":{"on":false}})"), "brightness 0 alone switches off");
    c.on = true;
    check(light_body(colour, c, body, why) && body == json::parse(R"({"on":{"on":true},"dimming":{"brightness":0}})"),
          "brightness 0 with on is the lowest level");
    c = home::Change();
    c.brightness = 250;
    check(light_body(colour, c, body, why) && body["dimming"]["brightness"] == 100, "brightness is kept within 100");
    c = home::Change();
    c.on = true;
    c.kelvin = 2700;
    check(light_body(colour, c, body, why) && body["color_temperature"]["mirek"] == 370, "2700 K is sent as 370 mirek");
    c.kelvin = 1500;
    check(light_body(colour, c, body, why) && body["color_temperature"]["mirek"] == 454, "1500 K is kept within the light's 454");
    c = home::Change();
    c.on = true;
    c.hs = std::make_pair(0, 100);
    check(light_body(colour, c, body, why) && body["color"]["xy"]["x"] == 0.7006 && body["color"]["xy"]["y"] == 0.2993,
          "red is sent as xy");
    check(!light_body(white, c, body, why) && why == "its colour cannot be changed", "a white light refuses a colour");
    c = home::Change();
    c.kelvin = 3000;
    check(!light_body(white, c, body, why) && why == "it cannot be set to a shade of white", "a dimmable light refuses a white");
    c = home::Change();
    c.brightness = 50;
    check(!light_body(plug, c, body, why) && why == "this plug can only be switched on and off", "a plug refuses brightness");
    c = home::Change();
    c.on = false;
    check(light_body(plug, c, body, why) && body == json::parse(R"({"on":{"on":false}})"), "a plug switches off");
    c.hs = std::make_pair(10, 10);
    check(light_body(white, c, body, why) && body == json::parse(R"({"on":{"on":false}})"),
          "switching off sends nothing else (values sent to an off light do not stick)");
    c = home::Change();
    c.kelvin = 3000;
    c.hs = std::make_pair(10, 10);
    check(!light_body(colour, c, body, why), "colour and white at once are refused");
    c = home::Change();
    c.volume = 3;
    check(!light_body(colour, c, body, why) && why == "a Hue light can only be switched, dimmed or coloured", "volume is refused");
    c = home::Change();
    c.key = "ok";
    check(!light_body(colour, c, body, why), "a remote-control key is refused");
    c = home::Change();
    check(!light_body(colour, c, body, why), "an empty change is refused");
    home::Thing broken = colour;
    broken.ref["rid"] = "../../api";
    c.on = true;
    check(!light_body(broken, c, body, why), "a thing whose id is not safe in a URL is refused");

    home::Thing scene;
    scene.id = "s1";
    scene.type = "scene";
    scene.caps = home::kScene;
    scene.ref = {{"kind", "scene"}, {"rid", "s1"}};
    c = home::Change();
    c.on = true;
    check(scene_body(scene, c, body, why) && body == json::parse(R"({"recall":{"action":"active"}})"), "a scene is recalled");
    c.on = false;
    check(!scene_body(scene, c, body, why) && why == "a scene can only be started", "a scene cannot be switched off");
    c.on = true;
    c.brightness = 20;
    check(!scene_body(scene, c, body, why), "a scene takes nothing but on");

    // Groups.
    const json room2 = json::array({{{"rtype", "room"}, {"rid", "r1"}, {"gl", "g1"}, {"n", 2}, {"fp", "0123456789abcdef"}}});
    const json room3 = json::array({{{"rtype", "room"}, {"rid", "r2"}, {"gl", "g2"}, {"n", 3}, {"fp", "0123456789abcdee"}}});
    const home::Thing a = light("a", all, room2, "light", {153, 454});
    const home::Thing b = light("b", home::kPower | home::kBrightness | home::kColorTemp, room2, "light", {200, 500});
    Group g;
    c = home::Change();
    c.on = false;
    check(pick_group({&a, &b}, c, g) && g.rid == "r1" && g.gl == "g1" && g.rtype == "room", "two lights that make up a room");
    check(!pick_group({&a}, c, g), "one light is not a group call");
    check(!pick_group({&a, &a}, c, g), "the same light twice is not a group");
    const home::Thing x = light("x", all, room3), y = light("y", all, room3);
    check(!pick_group({&x, &y}, c, g), "two of a room's three lights (a plug left out) are not the room");
    c.hs = std::make_pair(100, 100);
    c.on = true;
    check(!pick_group({&a, &b}, c, g), "a colour one member cannot show is not sent to the group");
    c = home::Change();
    c.on = true;
    c.kelvin = 6500;
    check(group_body({&a, &b}, c, body, why) && body["color_temperature"]["mirek"] == 200,
          "a group's white is kept within every member's range");
    c.kelvin = 2000;
    check(group_body({&a, &b}, c, body, why) && body["color_temperature"]["mirek"] == 454, "and at the warm end");
    const json bad_fp = json::array({{{"rtype", "room"}, {"rid", "r1"}, {"gl", "g1"}, {"n", 2}, {"fp", "short"}}});
    const home::Thing c1 = light("c1", all, bad_fp), c2 = light("c2", all, bad_fp);
    c = home::Change();
    c.on = true;
    check(!pick_group({&c1, &c2}, c, g), "a malformed group entry is not used");
    const json home2 = json::array({{{"rtype", "bridge_home"}, {"rid", "h"}, {"gl", "gh"}, {"n", 2}, {"fp", "fedcba9876543210"}}});
    const home::Thing h1 = light("h1", all, home2), h2 = light("h2", all, home2);
    check(pick_group({&h1, &h2}, c, g) && g.rtype == "bridge_home", "every light of the bridge is the home group");

    check(describe_bridge_error("device (light) has communication issues, command (.on.on) may not have effect", false) ==
              "the Hue Bridge has lost contact with this light; it may be switched off at the wall",
          "a light the bridge cannot reach");
    check(describe_bridge_error("anything else", false) == "the Hue Bridge did not accept the change", "other bridge errors");
    for (const char *e : {"communication issues", "soft off", "x"})
        for (const bool group : {false, true}) {
            const std::string d = describe_bridge_error(e, group);
            check(!reads_as_no_answer(d), std::string("'") + d + "' does not read as 'the bridge did not answer'");
        }
    // Which fixed errors send the shared layer looking for the bridge at a new address: exactly the
    // three about the bridge not answering.
    std::vector<std::string> no_answer;
    for (const std::string &e : error_texts())
        if (reads_as_no_answer(e)) no_answer.push_back(e);
    check(no_answer == std::vector<std::string>{"the Hue Bridge did not answer", "the Hue Bridge did not answer in time",
                                                "the Hue Bridge did not answer at its saved address"},
          "only the three 'did not answer' errors read as 'it did not answer' to the shared layer");

    check(fingerprint({"b", "a"}) == fingerprint({"a", "b"}) && fingerprint({"a"}) != fingerprint({"b"}) &&
              fingerprint({}).size() == 16,
          "fingerprints ignore order and differ by content");
    const json room = json::parse(R"({"children":[{"rid":"d2","rtype":"device"},{"rid":"d1","rtype":"device"}]})");
    const json room_more = json::parse(R"({"children":[{"rid":"d2","rtype":"device"},{"rid":"d1","rtype":"device"},
                                        {"rid":"d3","rtype":"device"}]})");
    check(group_fingerprint(room) != group_fingerprint(room_more), "a device added to a room changes its fingerprint");
}

// ---------------------------------------------------------------- the driver without a network
static void test_driver() {
    auto d = make_hue_driver();
    MemorySecrets secrets;
    d->attach(&secrets);
    check(std::string(d->kind()) == "hue_bridge", "kind is hue_bridge");
    check(d->claims("hue_bridge", {{"bridgeid", "00005efffe005301"}, {"modelid", "BSB002"}}), "claims a square bridge");
    check(d->claims("hue_bridge", {{"modelid", "BSB003"}}), "claims a Hue Bridge Pro");
    check(d->claims("hue_bridge", {}), "claims a bridge found without TXT keys");
    check(!d->claims("hue_bridge", {{"modelid", " bsb001 "}}), "does not claim the round first bridge (no v2)");
    check(!d->claims("tapo", {}) && !d->claims("homekit", {{"md", "BSB002"}}), "does not claim another kind, HomeKit included");

    home::Device dev;
    dev.id = "hue:00005efffe005301";
    dev.kind = "hue_bridge";
    dev.ip = "127.0.0.1";
    dev.port = 9;   // nothing is sent anywhere in these checks; were it, it would stay on this machine
    home::PairStep s = d->next_step(dev);
    check(s.kind == home::Pairing::PressButton && s.wait_s == 30 && s.prompt.find("round button") != std::string::npos,
          "an unpaired bridge asks for its button, for 30 s");
    const std::string key = "1028d66426293e821ecfd9ef1a0731df", pin = "sha256//S4p40dB26XlmuqWFim48xJGQRLm4jjAVv4JeY32tn6U=";
    dev.auth = {{"key", key}, {"pin", pin}, {"bridgeid", "00005efffe005301"}};
    check(d->next_step(dev).kind == home::Pairing::None, "a paired bridge needs nothing");

    // A refusal recorded by an earlier run, in the family's secrets: SHA-256 of the key, by device.
    const std::string key_sha256 = "455431dc2d569907d630af1d33ecbf924594836ff6143bcf5e37f4702564f81a";
    secrets.all["hue_bridge"] = {{"refused", {{"hue:00005efffe005301", key_sha256}}}};
    s = d->next_step(dev);
    check(s.kind == home::Pairing::PressButton && s.prompt.find("no longer accepts") != std::string::npos,
          "a key refused in an earlier run (in the secrets) asks for the button again");
    std::vector<home::Thing> out;
    std::string err;
    check(!d->enumerate(dev, out, err) && err == "the Hue Bridge no longer accepts Zero Bot's key; pair it again in Settings, Smart home",
          "and nothing is sent with that key");
    auto fresh = make_hue_driver();
    fresh->attach(&secrets);
    check(fresh->next_step(dev).kind == home::Pairing::PressButton, "a new driver object (a new run) knows it too");
    secrets.all["hue_bridge"] = {{"refused", {{"hue:00005efffe005301", std::string(64, '0')}}}};
    check(d->next_step(dev).kind == home::Pairing::None, "a refusal of another key does not count against this one");
    secrets.all["hue_bridge"] = {{"refused", {{"hue:somewhere-else", key_sha256}}}};
    check(d->next_step(dev).kind == home::Pairing::None, "nor does a refusal recorded for another bridge");
    secrets.all["hue_bridge"] = {{"cert_changed", {{"hue:00005efffe005301", pin}}}};
    s = d->next_step(dev);
    check(s.kind == home::Pairing::PressButton && s.prompt.find("certificate has changed") != std::string::npos,
          "a changed certificate recorded earlier asks for the button, to confirm");
    err.clear();
    check(!d->enumerate(dev, out, err) && err.find("certificate has changed, so nothing was sent") != std::string::npos,
          "and nothing is sent while it stands");
    secrets.all["hue_bridge"] = "not an object";
    check(d->next_step(dev).kind == home::Pairing::None, "secrets of the wrong shape are ignored");
    d->unpair(dev);
    check(dev.auth.is_object() && dev.auth.empty(), "unpair forgets the key and the pin");
    dev.auth = {{"key", key}, {"pin", pin}, {"bridgeid", "00005efffe005301"}};
    secrets.all["hue_bridge"] = {{"refused", {{"hue:00005efffe005301", key_sha256}}}, {"cert_changed", {{"other", pin}}}};
    d->unpair(dev);
    check(secrets.all.contains("hue_bridge") && !secrets.all["hue_bridge"].contains("refused") &&
              secrets.all["hue_bridge"]["cert_changed"] == json{{"other", pin}},
          "unpair also forgets what was recorded about this bridge, and only this bridge");
    secrets.all = json::object();

    dev.auth = {{"key", "bad key\r\n"}, {"pin", pin}};
    check(d->next_step(dev).kind == home::Pairing::PressButton, "a stored key that is not well formed counts as unpaired");
    dev.auth = {{"key", key}};
    check(d->next_step(dev).kind == home::Pairing::PressButton, "a key without a pin counts as unpaired: it is never sent unpinned");

    err.clear();
    dev.auth = json::object();
    check(!d->enumerate(dev, out, err) && err.find("not paired") != std::string::npos, "enumerate refuses without a pairing");
    dev.auth = {{"key", key}, {"pin", pin}, {"bridgeid", "00005efffe005301"}};
    dev.ip = "bridge.local";
    err.clear();
    check(!d->enumerate(dev, out, err) && err == "the Hue Bridge's saved address is not valid", "a host name is refused (no DNS)");
    dev.ip = "127.0.0.1";
    home::Thing sensor;
    sensor.id = "dev-sensor";
    sensor.type = "sensor";
    sensor.caps = home::kSensor;
    sensor.ref = {{"kind", "sensor"}, {"device", "dev-sensor"}};
    home::Change on;
    on.on = true;
    err.clear();
    check(!d->apply(dev, sensor, on, 2500, err) && err == "this is a sensor; it can only be read", "a sensor is never commanded");
    home::Thing scene;
    scene.id = "s1";
    scene.type = "scene";
    scene.ref = {{"kind", "scene"}, {"rid", "s1"}};
    home::Change off;
    off.on = false;
    err.clear();
    check(!d->apply(dev, scene, off, 2500, err) && err == "a scene can only be started", "a scene is refused before anything is sent");
    home::Thing mystery;
    mystery.ref = {{"kind", "lock"}};
    err.clear();
    check(!d->apply(dev, mystery, on, 2500, err) && err.find("does not know") != std::string::npos, "an unknown kind is refused");
    err.clear();
    const home::Thing one = light("l1", home::kPower);
    check(!d->apply_many(dev, {&one}, on, 2500, err) && err.empty(), "apply_many with one thing leaves it to apply");
}

int main(int argc, char **argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/mocks/hue";
    test_text();
    test_certs(dir);
    test_colour();
    test_replies();
    test_split();
    test_things();
    test_changes();
    test_driver();
    printf("hue: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
