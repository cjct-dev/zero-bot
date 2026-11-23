// Shelly relays, plugs, dimmers, colour lights, blinds and sensors, over their local HTTP API.
// One driver for both generations:
//   Gen2 and later (Plus, Pro, Mini, Gen3, Gen4): RPC calls as GET /rpc/<Method>?<params>, HTTP
//     digest authentication (SHA-256, user "admin", realm = the device id) when the owner set a
//     password.
//   Gen1: GET /settings, /status, /relay/N, /light/N, /color/N, /white/N, /roller/N, HTTP basic
//     authentication when the owner restricted the login.
// GET /shelly answers on every generation without a password: it gives the generation, the
// device's id (Gen2+) or MAC (Gen1), and whether a password is needed.
//
// Everything follows Shelly's public API documentation (shelly-api-docs.shelly.cloud, Gen1 and
// Gen2+). Two rules come from reading aioshelly (Apache-2.0; no code copied): a relay whose
// consumption or appliance type starts with "light" is a light, and a Gen2 relay the device uses
// as its own thermostat's actuator is not offered.
//
// What a caller needs to know:
//  - Things, one per channel. Gen2+: "switch:N" (type plug, switch or light; power, with power_w
//    when it measures), "light:N" (dimmer), "rgb:N" and "rgbw:N" (colour), "cct:N" (white with a
//    colour temperature), "cover:N" (blind: position), "temperature:N" (sensor: current_c, with
//    humidity from "humidity:N" of the same number), "pm1:N" (sensor: power_w). Gen1: "relay:N",
//    "light:N", "roller:N", and "sensor" for an H&T. A 2PM is two switches or one blind,
//    depending on its profile; a Gen1 2.5 likewise by its mode. At most 32 things a device.
//  - Never offered: inputs, energy meters, thermostats, smoke, gas, flood and door sensors, and
//    any relay or blind whose name (its own, or the device's when its own does not say it is a
//    light) or appliance type reads as a garage door, gate, door, lock, opener, valve, alarm or siren
//    (in English, or the words for garage, door, gate and lock in German, French, Spanish, Italian,
//    Portuguese, Dutch or Polish; a Shelly 1 is a common garage-door retrofit), unless the name also says it is a light ("Front
//    door light"); nor a relay set to switch itself off within seconds (a pulse, as a door or gate
//    opener is wired), whatever it is called.
//  - Changes: on/off; brightness, colour (Change::hs) and white (Change::kelvin) where the thing
//    has them; for a blind position 100 opens it, 0 closes it, a value between moves it there
//    (only when the blind has been calibrated in the Shelly app: Cover pos_control, Gen1
//    positioning), and Change::transport "stop" stops it. A brightness, colour or white without
//    Change::on also switches the light on; brightness 0 switches it off. Calibration, setup,
//    firmware and every other call that changes how a device is set up are never sent.
//  - Before every change, every read and pairing, the device is asked who it is (GET /shelly): when
//    another device answers at the saved address, nothing is sent (kOtherDevice, whose wording
//    makes the app look for the device at a new address: home::who_answered), so a password never
//    reaches another device and its answers (a refusal, a 401) are never kept against this one.
//    "Who it is" is the id or MAC its things were read with, and the one that took the password
//    when it was typed (Device::auth "dev"), so a paired device that has never been read is
//    recognised too. An answer that is garbled (kNotUnderstood) or too long is the device's: it
//    is not looked for elsewhere.
//  - Before a change to a relay or blind, its settings are read again (Shelly.GetConfig, Gen1
//    /settings): one renamed "Garage door", given a switch-off timer of seconds, or gone (a 2PM or
//    2.5 changed between relay and blind) since the device was last read is not switched.
//  - Pairing: none, unless the device asks for a password. The driver learns that from GET
//    /shelly or a 401, and keeps it in the family's secrets (secrets.shelly.locked[device] =
//    generation), because read() and apply() cannot change Device::auth. next_step() then asks:
//    Gen2+ for the password (the user is always "admin"); Gen1 first for the login's user name,
//    then its password. Device::auth holds {"user", "password", "dev"}.
//  - A password the device rejected is never sent again, by this run or any later one: its
//    fingerprint (SHA-256 of user and password, not the password) is kept as
//    secrets.shelly.refused[device] until a password is typed again (pair) or the device is
//    unpaired; when the file cannot take it, it is kept in memory for the rest of the run. A 429 (the
//    device's protection against guessing) is "try later", not a refusal.
//  - A typed password is on trial (driver.h home::trial) until the device takes it, kept as
//    secrets.shelly.trial[device] = {"print": its fingerprint, "trial": ...}: written before the first
//    request that carries it (nothing is sent when that write fails), and one tried by an earlier run
//    that never learnt the outcome counts as refused until it is typed again.
//  - Device::port is honoured (80 when 0). Only IPv4 addresses are contacted.
//  - Deadlines: 4 s for enumerate(), read() and pair(); apply() keeps to deadline_ms.
//  - Device text (names) is cleaned with home::clean_name; numbers outside their documented
//    ranges are dropped; answers over 256 KiB or nested deeper than 32 are refused.
//
// Depends on libcurl (through net/http.h), OpenSSL libcrypto (SHA-256) and nlohmann/json.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "smarthome/driver.h"

std::unique_ptr<home::Driver> make_shelly_driver();

namespace shelly {
using home::json;

// The opening sentences of the driver's errors (an error may add one more sentence).
extern const char *const kNoAnswer;       // "The Shelly did not answer."
extern const char *const kNeedsPassword;  // "The Shelly asks for a password."
extern const char *const kRefused;        // "The Shelly did not accept the saved password,"
extern const char *const kThrottled;      // "The Shelly is turning logins away for a while"
extern const char *const kOtherDevice;    // "Another device now answers at this address."
extern const char *const kNotUnderstood;  // "The Shelly's answer could not be understood."

// Building blocks, exposed for shelly_test.cpp. Not part of the interface.
namespace detail {

// What GET /shelly says.
struct Identity {
    int gen = 0;              // 1 for Gen1 (no "gen" key), else the "gen" value; 0 when not understood
    std::string id;           // Gen2+: "shellyplus1pm-a8032ab12345" (lower case); Gen1: ""
    std::string mac;          // lower-case hex without separators
    std::string model;        // Gen2+ "model" ("SNSW-001P16EU"), Gen1 "type" ("SHSW-PM")
    std::string app;          // Gen2+ "app" ("Plus1PM")
    std::string profile;      // Gen2+ multi-profile devices: "switch", "cover", ...
    bool auth = false;        // a password is needed (Gen2+ auth_en, Gen1 auth)
    int num_meters = -1;      // Gen1, when given
    // What a thing's ref keeps to recognise the device: its id, or for Gen1 its MAC.
    std::string who() const { return gen >= 2 ? id : mac; }
};
bool parse_identity(const std::string &body, Identity &out);

// The things of a Gen2+ device, from the results of Shelly.GetConfig and Shelly.GetStatus;
// `device_name` is set to the name its owner gave it ("" when none).
std::vector<home::Thing> things_gen2(const Identity &who, const json &config, const json &status, std::string &device_name);
// The things of a Gen1 device, from the bodies of /settings and /status.
std::vector<home::Thing> things_gen1(const Identity &who, const json &settings, const json &status, std::string &device_name);
// A thing's state from a status (Gen2+: the result of Shelly.GetStatus; Gen1: /status), with
// only the keys home.h documents; {} when the status does not mention the thing.
json state_of(const home::Thing &thing, const json &status);

// The one request a change becomes.
struct Call {
    std::string method;   // Gen2+: "Switch.Set", "Cover.GoToPosition", ...; "" for Gen1
    json params;          // Gen2+
    std::string path;     // Gen1: "/relay/0?turn=on"
};
// False with a sentence when the thing cannot take the change; then nothing is to be sent.
bool plan(const home::Thing &thing, const home::Change &change, Call &out, std::string &err);
// Whether a call is one this driver may ever send (the allow-list the e2e test also checks).
bool allowed(const Call &call);

// The path of a Gen2+ call by GET: "/rpc/Switch.Set?id=0&on=true", each value as JSON, URL-encoded
// ("rgb=%5B0%2C255%2C0%5D"), id first.
std::string rpc_path(const std::string &method, const json &params);
// Reads the answer to one: the result (2xx), or an error sentence from {"code", "message"} (any
// other status), with the code in `code` (0 when none). Never the device's own message.
bool read_rpc_answer(long status, const std::string &body, json &result, std::string &err, int *code = nullptr);

std::string basic_auth_header(const std::string &user, const std::string &password);   // "Authorization: Basic ..."
std::string fingerprint(const std::string &user, const std::string &password);         // 32 hex digits
bool too_deep(const std::string &text, int max = 32);
bool valid_ipv4(const std::string &ip);
// Whether a relay with this name or appliance type must not be offered (garage doors, locks, ...).
// A word that contains "garage" counts ("GarageDoor", "Garagentor").
bool banned(const std::string &name, const std::string &appliance_type);
// Whether a relay or blind is left out: banned() on its own name or appliance type; the device's
// name when the channel's own name does not say it is a light (a Shelly 1 named "Garage door" in
// the Shelly app with its relay unnamed, a Gen1 2.5's blind, which has no name of its own); or a
// relay that switches itself off within kPulseSeconds of being switched on (auto-off: how a
// garage door, gate or door opener, or a bell, is wired, whatever it is called).
constexpr double kPulseSeconds = 10;
bool left_out(const std::string &name, const std::string &device_name, const std::string &appliance_type, double auto_off_s = 0);
// Before a change: whether a relay or blind (Gen2+ switch/cover, Gen1 relay/roller) is still what
// it was read as, by the device's current settings (Gen2+: the result of Shelly.GetConfig; Gen1:
// the body of /settings): still there, and not left out now. False with a sentence when it is
// gone or left out; true for any other thing.
bool recheck(const home::Thing &thing, const json &config, std::string &err);

}  // namespace detail
}  // namespace shelly
