// Philips Hue bridges, over the bridge's own local interface (CLIP version 2, HTTPS). No cloud.
//
// What a caller needs to know:
//  - kind() is "hue_bridge", the kind discovery records for `_hue._tcp` and for SSDP answers with
//    a `hue-bridgeid` header (a bridge also announces `_hap._tcp`; discovery ranks hue_bridge above
//    homekit, so the HomeKit driver never sees it). claims() refuses model BSB001 (the first, round
//    bridge has no version 2).
//  - Pairing is one PressButton step (wait_s 30): pair() checks that the address is a Hue Bridge
//    whose certificate names the bridge id, pins the certificate's public key, then asks the bridge
//    for an application key once a second for up to 30 s while the user presses the link button.
//    After pairing, every request is pinned to that key (http::Request::pinned_pubkey); the key is
//    never sent to a peer that presents another public key.
//  - Device::auth holds {"key", "pin", "bridgeid"} once paired. What a later call learns that must
//    survive a restart is kept in the family's secrets, `secrets()["hue_bridge"]`:
//      "refused":      {<device id>: SHA-256 (hex) of a key the bridge rejected (401 or 403), or a
//                       list of them: one bridge saved on two networks has a key for each}
//      "cert_changed": {<device id>: the pin the bridge's certificate no longer matched}
//    Either makes next_step() ask for the button again and every other call fail at once, sending
//    nothing, until pair() succeeds or unpair() is used (each forgets what was recorded about the
//    key and pin it replaces or drops, and nothing else). A rejected key is therefore never sent
//    again, not even by a later run.
//  - Things: one per light (type "light", or "plug" for a light whose archetype is "plug"), with
//    kPower and, as the light offers them, kBrightness, kColorTemp, kColor; one per scene (type
//    "scene", kScene, room = its room or zone); one per sensor device with a temperature or motion
//    service (type "sensor", kSensor). Thing::room is the light's room, else its first zone (by
//    name). Devices with a camera, siren, chime, doorbell button or contact/tamper service are
//    never exposed, nor is any scene that includes one of their lights, nor a scene that switches
//    on a smart plug (or does not say it stays off): a plug may feed a heater, which the app asks
//    about before switching it on, and a scene would switch it on without that question. Since a
//    scene can be edited in the Hue app at any time, apply() reads it again before every recall
//    and refuses it (scene_allowed) unless every light it holds is a thing the assistant may use,
//    and every light it switches on is a light (not a plug) not set to "Ask before switching on".
//  - Thing::state: lights {"on", "brightness" 0..100, "kelvin" (white mode) or "hue" 0..360 and
//    "saturation" 0..100 (colour mode)}; sensors {"current_c", "motion"}; scenes {}.
//  - Thing::ref (driver-private): lights {"kind":"light","rid","device","mirek":[min,max],
//    "groups":[{"rtype","rid","gl","n","fp"}]}; scenes {"kind":"scene","rid"}; sensors
//    {"kind":"sensor","device","temperature","motion"}.
//  - enumerate() reads everything with one GET /clip/v2/resource. read() asks only for the
//    resource types its things need (light, temperature, motion).
//  - apply_many() makes one grouped_light request when the things are exactly the lights of one
//    room, zone or the whole home, and the bridge still reports that membership; otherwise it
//    returns false with no error so the caller applies the change thing by thing.
//  - Deadlines: apply/apply_many use the deadline given; enumerate and read take at most 4 s.
//    Successive commands to one bridge are spaced 100 ms apart (1 s after a group command or a
//    scene), as Signify asks, within the deadline. A 429 or 503 answer is retried once.
//  - Device::port, when not 0, is the HTTPS port (default 443). Device::ip must be a numeric
//    address; host names are refused, so no DNS lookup is ever made.
//  - Errors: pairing errors are sentences; enumerate/read/apply errors are lower-case clauses the
//    caller puts after a thing's name. Only an error about the bridge not answering contains
//    "answer" (the shared layer's cue to look for the bridge at a new address).
//
// Sources (see the sprint's STATUS.md for what was checked against each): Signify's public "Get
// started" page (POST /api, devicetype, error 101), the openhue-api description of CLIP v2
// (Apache-2.0: paths, GET /clip/v2/resource, brightness 0 = lowest, mirek 153..500, 401/403/429),
// aiohue (Apache-2.0) and its fixture recorded from a real bridge (resource shapes, bridge id
// forms, mirek schemas of 50..1000 and 0..65535). openHAB's binding (EPL-2.0) was read only.
//
// Depends on the app's http layer (libcurl), OpenSSL libcrypto, smarthome/color.h and
// nlohmann/json.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "smarthome/driver.h"

std::unique_ptr<home::Driver> make_hue_driver();

namespace hue {
using home::json;

// The bridge firmware that first offered version 2 of the interface (openHAB's threshold).
constexpr long long kMinFirmware = 1948086000LL;

// Building blocks, exposed so that hue_test.cpp can check each one. Not part of the interface.
namespace detail {

// ---- text from the bridge
// Replaces bytes that are not UTF-8 (RFC 3629: no overlong forms, no surrogates) with '?'.
std::string sanitize_utf8(const std::string &raw);
// Trims, removes control characters (C0, DEL, C1), zero-width and bidirectional controls and the
// byte-order mark, squeezes spaces, and caps at `max` bytes without cutting a UTF-8 sequence.
std::string clean_name(const std::string &raw, size_t max = 40);
// Whether the text nests arrays and objects deeper than `max` (strings are skipped).
bool too_deep(const std::string &text, int max = 48);
// Whether `id` may be put in a URL path: 1 to 64 of [A-Za-z0-9_-].
bool safe_id(const std::string &id);
// Whether an application key may be put in a header: 1 to 128 of [A-Za-z0-9_-].
bool safe_key(const std::string &key);
// Whether `pin` is "sha256//" followed by 44 base64 characters.
bool safe_pin(const std::string &pin);
// A bridge id in one comparable form: lower case, ':' and '-' removed, and the 16-digit form
// with "fffe" in the middle reduced to the 12-digit MAC form. "" when it is not 12 or 16 hex digits.
std::string normalize_bridge_id(const std::string &id);

// ---- the bridge's certificate
// "sha256//<base64>" of the certificate's SubjectPublicKeyInfo (what libcurl's pinned public key
// option takes), "" when `pem` holds no certificate.
std::string pin_from_pem(const std::string &pem);
// The certificate's subject common names and DNS subjectAltNames, as written.
std::vector<std::string> names_from_pem(const std::string &pem);
// Whether one of those names is this bridge id (compared with normalize_bridge_id).
bool cert_names_bridge(const std::string &pem, const std::string &bridge_id);

// ---- colour (through smarthome/color.h)
// Mirek (micro reciprocal degrees) for a white, rounded and kept within [lo, hi].
int mirek_from_kelvin(int kelvin, int lo = 153, int hi = 500);
int kelvin_from_mirek(double mirek);   // 0 for nonsense
// Hue 0..360 and saturation 0..100 to CIE 1931 xy, rounded to 4 decimals as the bridge takes it.
void hs_to_xy(double hue, double sat, double &x, double &y);

// ---- replies
// Parses a reply body: UTF-8 repaired, nesting checked, nothing thrown. False when unusable.
bool parse_json(const std::string &body, json &out);

enum class LinkReply { Key, NotPressed, Refused, Unreadable };
// Reads the answer to POST /api: [{"success":{"username":...}}] gives Key (with `key` filled,
// only if safe_key), [{"error":{"type":101,...}}] NotPressed, any other error Refused, anything
// else Unreadable.
LinkReply read_link_reply(const std::string &body, std::string &key);

// The bridge's public configuration (GET /api/0/config).
struct Config {
    std::string bridgeid, modelid, name;
    long long swversion = -1;   // -1 when absent or not a number
};
bool read_config(const std::string &body, Config &out);

// ---- things
// The resources enumerate() works from, by type.
struct Snapshot {
    json lights = json::array(), rooms = json::array(), zones = json::array(), scenes = json::array(),
         devices = json::array(), temperatures = json::array(), motions = json::array(), homes = json::array();
};
// Sorts the "data" array of GET /clip/v2/resource into a snapshot (other types are dropped; at
// most 2000 of each type are kept).
void split_resources(const json &data, Snapshot &out);
struct BridgeInfo {
    std::string name, model;   // from the bridge's own device resource, cleaned; may be empty
};
// Turns a snapshot into things (allow-list mapping, names cleaned, at most 400 things).
void build_things(const Snapshot &s, std::vector<home::Thing> &out, BridgeInfo &info);
// The state of one light, sensor or scene thing from its resource(s).
json light_state(const json &light);
json sensor_state(const json *temperature, const json *motion);

// A membership fingerprint: FNV-1a 64 of the sorted strings, as 16 hex digits.
std::string fingerprint(std::vector<std::string> items);
// The fingerprint of a room or zone resource's children (room: device ids; zone: "rtype/rid").
std::string group_fingerprint(const json &group);

// ---- changes
// The PUT body for one light or plug thing, or false with a lower-case reason (nothing sent).
bool light_body(const home::Thing &t, const home::Change &c, json &body, std::string &why);
// The scene recall body, or false with a reason.
bool scene_body(const home::Thing &t, const home::Change &c, json &body, std::string &why);
// Whether a scene, as the bridge holds it now (the resource from GET /clip/v2/resource/scene/<id>),
// may be recalled, judged against the device's saved things (Device::things, with the user's own
// settings): every light it changes must be a thing of this bridge the assistant may use (a light
// of a device that is never exposed is no thing), and every light it switches on (an action that
// does not say "off") must be a light, not a plug, that is not set to ask before switching on.
// False with a lower-case reason otherwise. A scene can be changed in the Hue app at any time, so
// this is asked right before every recall, not only when the list is read.
bool scene_allowed(const json &scene, const std::vector<home::Thing> &things, std::string &why);

struct Group {
    std::string rtype, rid, gl, fp;   // the room/zone/bridge_home, its grouped_light, its fingerprint
    int n = 0;                        // lights in it at enumeration
};
// The group whose lights are exactly `things`, when there is one. Every thing must be a light or
// plug thing of this driver that can take `c` (else false).
bool pick_group(const std::vector<const home::Thing *> &things, const home::Change &c, Group &out);
// The grouped_light PUT body for those things (mirek kept within every member's range).
bool group_body(const std::vector<const home::Thing *> &things, const home::Change &c, json &body, std::string &why);

// Plain words for a v2 "errors" description from a reply that otherwise succeeded.
std::string describe_bridge_error(const std::string &description, bool group);

// Every fixed error sentence enumerate, read and apply can give, for tests: which of them read as
// "the bridge did not answer" to the shared layer is part of the contract above.
std::vector<std::string> error_texts();

// Test hooks: how long pair() keeps asking for a key (default 30000 ms), and how long it waits
// between attempts (default 1000 ms). The app never calls these.
void set_pair_window_ms(int ms);
void set_pair_interval_ms(int ms);

}  // namespace detail
}  // namespace hue
