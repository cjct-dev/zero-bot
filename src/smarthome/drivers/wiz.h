// WiZ lights, plugs and ceiling fans (Signify's Wi-Fi range, "WiZ Connected"): local control with
// one JSON datagram each way on UDP port 38899.
//
//   {"method":"getSystemConfig","params":{}}   module name (what kind of device), MAC, firmware
//   {"method":"getModelConfig","params":{}}    white range, lowest dimming, fan speeds (firmware 1.22+)
//   {"method":"getUserConfig","params":{}}     white range on older firmware
//   {"method":"getPilot","params":{}}          current state
//   {"method":"setPilot","params":{...}}       change it
//   {"method":"getPower","params":{}}          milliwatts, on metering plugs
//
// Signify publishes no document for this protocol. Everything here follows sbidy/pywizlight (MIT
// licence), which Home Assistant uses; see wiz.cpp for what was ported from where.
//
// What a caller needs to know:
//  - Each request has a socket of its own, connected to the device and closed before the call
//    returns. Nothing acknowledges a datagram except the reply itself, so a request is sent again
//    (after 250 ms, then at growing intervals) until the reply with the same method arrives or
//    the deadline passes. setPilot only states what the device should be, so a repeat is harmless.
//  - Things: a light ("light": power, brightness, and white and colour where the module has them),
//    a plug ("plug": power, and power_w when it meters), a ceiling fan ("fan": power and speed,
//    beside its light). The module name from getSystemConfig decides; a module of any other kind
//    yields no things. The built-in scenes are not offered (over thirty per light, with the same
//    names on every light). WiZ devices keep no name of their own: things are called "WiZ light
//    1a2b" (the end of the MAC) until the user names them.
//  - Brightness is the WiZ app's own percentage ("dimming"), raised to the module's lowest
//    (10 unless getModelConfig says otherwise). The device takes a white or a colour per command,
//    never both.
//  - Signing. Newer firmware puts an "hmac" in its replies, answers reads from anyone, and turns
//    down an unsigned setPilot (-32602). It takes a setPilot signed with the home's key: the
//    "udp_signing_key" (32 hex characters, one per WiZ home) from the WiZ app's export for local
//    integration. The key is kept once for the family, in Secrets under "wiz"
//    ({"signing_key": hex, "refused": true when a device turned it down}); a device's things carry
//    "signed": true in their ref once its replies were seen signed, and then next_step asks for
//    the key (Pairing::Token) while there is none or the saved one was refused. pair() stores what
//    was typed (the hex, or the whole exported file pasted) and sends nothing: only a change shows
//    whether a key is right, and none is made up. A key a device refused (-32605) is never sent
//    again, by this run or a later one, until a key is typed again; a key not yet taken by any
//    device goes to one device at a time, in one datagram (no resend while its answer is on the
//    way; once a device has taken it, a lost datagram is resent as usual). A signed request carries the
//    time, and a stale one is refused like a wrong key, so nothing is signed while the clock reads
//    earlier than 2025 (never set), nor in the first ten minutes after start while the kernel
//    reports the clock as not synchronised (a board without a battery-backed clock starts from a
//    time saved at the last shutdown until a time service sets it).
//  - The device at the saved address must be this one: its MAC (from the id discovery gave it,
//    "wiz:<MAC>", or Device::mac) must be named by getSystemConfig when listing, by getPilot when
//    reading, and by a getPilot sent before every setPilot; a getPilot that names no MAC (some
//    firmware) is followed by a getSystemConfig in the same time. Routers hand an address to another
//    device, and a WiZ device holds no key that would fail elsewhere. Another MAC gives kMoved, none
//    kNoMac; a device saved without a MAC may be listed and read, never changed (kNoIdentity).
//  - Errors are short sentences. Only the "did not answer" ones, and the two for another device
//    answering at the saved address (kMoved, kNoMac), contain a word that control.cpp's unreachable()
//    looks for, so only they lead to a search for the device at a new address.
//
// Depends on the C++ standard library, POSIX sockets, OpenSSL libcrypto and nlohmann/json.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "smarthome/driver.h"

std::unique_ptr<home::Driver> make_wiz_driver();

namespace wiz {

using home::json;

// The fixed sentences errors begin with.
extern const char *const kNoAnswer;      // "The WiZ device did not answer."
extern const char *const kGarbled;       // "The WiZ device replied with something that could not be understood."
extern const char *const kNeedsKey;      // "This WiZ device only accepts signed commands. ..."
extern const char *const kKeyRefused;    // "A WiZ device did not accept the signing key, ..."
extern const char *const kKeyRefusedNotNoted;   // the same, when that refusal could not be noted on this device
extern const char *const kClockNotSet;   // "The clock here is not set yet, ..."
extern const char *const kKeyNotSaved;      // "The WiZ signing key's first use could not be noted on this device ..."
extern const char *const kKeyUnconfirmed;   // "No WiZ device has confirmed the signing key typed last ..."
extern const char *const kRefused;       // "The WiZ device refused the command."
extern const char *const kMoved;         // "Another WiZ device answered at this one's saved address, ..."
extern const char *const kNoMac;         // "The device that answered at this WiZ device's saved address did not say ..."
extern const char *const kNoIdentity;    // "No hardware address is saved for this WiZ device, ..."

// Building blocks, exposed for wiz_test.cpp. Nothing here touches the network.
namespace detail {

// The wall clock signed requests are stamped with (seconds since 1970). Tests set their own; the
// app never calls this. nullptr restores the system clock.
void set_unix_clock(long long (*clock)());
// Whether a signed request may be stamped with this time: not before 2025, and not while the clock
// is not synchronised (`synced`) during the first ten minutes after start (`uptime_s`).
bool signing_clock_ok(long long unix_time, bool synced, long long uptime_s);

// What a module is, read from its name ("ESP01_SHRGB_03": the middle part says what it can do).
struct Model {
    std::string cls;            // RGB, TW, DW, SOCKET, FANDIM, FANTW; "" when not one of ours
    int kelvin_min = 2200, kelvin_max = 6500;
    int min_dimming = 10;       // the lowest "dimming" the device accepts
    int fan_speeds = 6;         // fans: speeds 1..n
    bool has_light() const { return cls == "RGB" || cls == "TW" || cls == "DW" || cls == "FANDIM" || cls == "FANTW"; }
    bool has_fan() const { return cls == "FANDIM" || cls == "FANTW"; }
    bool is_plug() const { return cls == "SOCKET"; }
    uint32_t light_caps() const;
};
// Fills m.cls from getSystemConfig's moduleName, or from typeId on firmware too old to name its
// module. False when the module is of no kind this driver exposes.
bool classify(const std::string &module_name, int type_id, Model &m);
// Takes the white range, lowest dimming and fan speeds from a getModelConfig or getUserConfig
// result. Values out of range are ignored.
void apply_config(const json &result, Model &m);

// Percent as people say it (0..100) to and from the device's "dimming" (min_dimming..100).
int dimming_from_percent(int percent, int min_dimming);
int percent_from_dimming(int dimming);
// Fan speed steps 1..speeds to and from percent.
int fan_speed_from_percent(int percent, int speeds);
int percent_from_fan_speed(int speed, int speeds);

// Hue 0..360 and saturation 0..100 to the bulb's red, green, blue (0..255) and warm white (0..128)
// channels, and back. Above 50 % saturation the colour stays full and white is faded out; below,
// white is full and the colour is faded out.
void hs_to_rgbw(double hue, double saturation, int rgb[3], int &white);
void rgbw_to_hs(const int rgb[3], int white, int &hue, int &saturation);

// The datagram for a request. `params` is the text of a JSON object. With a key (16 bytes, already
// decoded) the request is signed: "sigTs" is added at the end of params and an HMAC-SHA256 of
// that text sent along. `unix_time` is passed in so that tests can fix it.
std::string datagram(const std::string &method, const std::string &params, const std::string &key, long long unix_time);
// 32 hex characters to 16 bytes; false for anything else. White space around and inside is
// ignored. A whole exported file (JSON holding "udp_signing_key" somewhere) is accepted too, when
// it holds one key; an export of several homes with different keys is refused (`several` set).
bool parse_key(const std::string &text, std::string &key, bool *several = nullptr);

// The things one device yields, with capabilities and addressing but no state yet.
std::vector<home::Thing> things_for(const Model &m, const std::string &mac, bool metering, bool signed_firmware);
// Fills thing.state from a getPilot result (and a getPower result's milliwatts, when given).
void fill_state(home::Thing &thing, const json &pilot, const json *power_mw);
// The setPilot params (JSON text, keys in the order sent) for a change. False with a sentence in
// err when the thing cannot do what is asked.
bool pilot_for(const home::Thing &thing, const home::Change &change, std::string &params, std::string &err);

}  // namespace detail

}  // namespace wiz
