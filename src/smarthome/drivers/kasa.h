// TP-Link Kasa plugs, power strips, wall switches, dimmers and bulbs that speak the older local
// protocol: JSON on TCP port 9999, scrambled with an "autokey" XOR (the first key is 171, then
// each scrambled byte is the key for the next) and preceded by its length as four big-endian
// bytes. Discovery sends the same scrambled get_sysinfo request without the length to UDP 9999.
// The protocol has no login: anyone on the home network can switch these devices, with or
// without this app. TP-Link does not document it; what is used here is described in STATUS.md.
//
// What a caller needs to know:
//  - Device kind "tplink". Claimed when Device::info "device_type" is one of the two families
//    switched here, "IOT.SMARTPLUGSWITCH" (plugs, strips, wall switches, dimmers) or
//    "IOT.SMARTBULB" (bulbs, light strips), and the kind is "tplink", or "tapo" for a device an
//    earlier version saved from its 20002 reply. The Tapo driver claims no "IOT." type.
//  - Things: a plug or wall switch is one thing "main" (type plug or switch, kPower); a dimmer is
//    one "light" with kBrightness; a power strip is one "plug" per socket, its id the socket's
//    child id; a bulb or light strip is one "light" with kBrightness, kColorTemp and kColor as
//    the bulb reports them. Models that measure power add kSensor (state power_w and
//    energy_today_wh, read only when particular things are asked for).
//  - A device whose firmware moved it to TP-Link's newer login (KLAP, the one Tapo plugs use) is
//    recognised from Device::info "scheme" (set by discovery from the 20002 reply), or when its
//    port refuses the connection and a 20002 query to its address says so. Nothing is then sent
//    and the error begins with kNewLogin. This driver does not speak that login.
//  - Port: Device::port when not 0, else 9999. One TCP connection per request, closed after the
//    reply. No pairing and no secrets. Deadlines: apply's deadline_ms; 4 s for enumerate and read.
//
// Depends on the C++ standard library, POSIX sockets and nlohmann/json.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "smarthome/driver.h"

std::unique_ptr<home::Driver> make_kasa_driver();

namespace kasa {
using home::json;

// The opening words of the errors a caller may want to tell apart.
extern const char *const kNewLogin;   // "this Kasa device moved to TP-Link's newer login"
extern const char *const kNoAnswer;   // "the Kasa device did not answer"
extern const char *const kRefused;    // "the Kasa device refused the connection"

// Building blocks, exposed so that kasa_test.cpp can check each one. Not part of the interface.
namespace detail {

std::string encrypt(const std::string &plain);    // autokey XOR from 171, no length
std::string decrypt(const std::string &cipher);
std::string frame(const std::string &plain);      // four-byte big-endian length, then encrypt(plain)

constexpr size_t kMaxReply = 64 * 1024;           // a longer reply is refused unread
// Whether a JSON text nests arrays and objects deeper than `max` (strings are skipped).
bool too_deep(const std::string &text, int max = 32);
// The JSON object in a decrypted reply; false when it is not one, or nests too deeply.
bool parse_reply(const std::string &plain, json &out);

// "IOT.SMARTPLUGSWITCH" or "IOT.SMARTBULB" (any case, surrounding spaces ignored).
bool switched_type(const std::string &device_type);
// Whether discovery said this device needs the newer login ("scheme" other than "" or "XOR").
bool needs_new_login(const std::map<std::string, std::string> &info);
// Whether a 20002 discovery reply (the bytes after its 16-byte header) is from a Kasa device that
// needs the newer login.
bool reply_says_new_login(const std::string &json_text);

// The things of a device from its get_sysinfo object. False with an error sentence when the
// device is not of a kind switched here. Names are cleaned; at most 16 sockets are kept.
bool things_from(const json &sysinfo, std::vector<home::Thing> &out, std::string &err);
// Refreshes thing.state from get_sysinfo (what is on, brightness, colour).
void state_from(const json &sysinfo, home::Thing &thing);
// power_w and energy_today_wh into `state` from emeter get_realtime and get_daystat answers, for
// the day `day` of the month the request named. Values that are not plausible are left out.
void energy_into(const json &realtime, const json &daystat, int day, json &state);

// The request for a change to one thing, or to several sockets of one strip (child ids). False
// with an error sentence when the thing cannot take the change; then nothing is sent.
bool request_for(const home::Thing &thing, const home::Change &change, json &request, std::string &err);
bool request_for_sockets(const std::vector<const home::Thing *> &things, const home::Change &change, json &request,
                         std::string &err);
// Whether the device carried out every method of `request`, from its reply.
bool reply_ok(const json &request, const json &reply, std::string &err);

// The UDP port of the 20002 query sent when port 9999 refuses (tests point it at a mock).
void set_probe_port(int port);
int probe_port();

}  // namespace detail
}  // namespace kasa
