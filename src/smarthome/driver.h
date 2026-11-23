// The contract between the smart-home layer and the code that talks to one family of devices
// (Tapo plugs, a Hue bridge, HomeKit accessories, a TV...). A driver turns a discovered device
// into "things" a person names and commands, and applies changes to them.
//
// Rules every driver follows:
//  - Stateless between calls apart from a small session cache keyed by device id. No background
//    threads, no long-lived connections, no subscriptions: state is read on demand.
//  - Deadlines: about 2.5 s to apply a change, 4 s to list a bridge's things.
//  - Errors are short sentences a person can be told; never raw protocol text, never a secret.
//    One that says nothing answered at the device's address, or another device did, uses the
//    words who_answered() below knows; no other error does.
//  - Names that come from a device are untrusted: trimmed, control characters removed, capped
//    at 40 characters.
//  - Only device types on the driver's allow-list become things. Locks, garage doors, alarms,
//    cameras, valves and cooking appliances are never exposed.
//  - A driver opens a socket only when the shared layer calls it, and the shared layer never
//    calls it unless the device is on a network the user has scanned (Wi-Fi or Ethernet, never
//    a phone's Bluetooth tether).
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

namespace home {

using json = nlohmann::json;

// What a thing can do. Units are fixed here so no caller deals with a maker's scale.
enum Cap : uint32_t {
    kPower = 1 << 0,        // on / off
    kBrightness = 1 << 1,   // 0..100 percent
    kColorTemp = 1 << 2,    // kelvin
    kColor = 1 << 3,        // hue 0..360, saturation 0..100
    kScene = 1 << 4,        // a thing of type "scene" can only be activated
    kTargetTemp = 1 << 5,   // degrees Celsius, with a heating mode (off / heat / auto)
    kPosition = 1 << 6,     // covers: 0 closed .. 100 open
    kFanSpeed = 1 << 7,     // 0..100 percent
    kTransport = 1 << 8,    // play, pause, stop, next, previous
    kVolume = 1 << 9,       // 0..100 and mute
    kVolumeStep = 1 << 10,  // only up / down exist
    kSource = 1 << 11,      // input or app by name
    kKeys = 1 << 12,        // remote-control key presses (TVs)
    kSensor = 1 << 13,      // read-only values in state
};

// The unit a person names and commands: a plug, one socket of a strip, a light, a thermostat,
// a TV. One device yields one or many.
struct Thing {
    std::string id;         // stable within its device: child id, resource id, "aid.iid", ...
    std::string name;       // as the maker's app has it
    std::string alias;      // set by the user in settings; overrides name when speaking
    std::string room;       // from the bridge, or set by the user
    std::string type;       // light, plug, switch, thermostat, cover, fan, speaker, tv, scene, sensor
    uint32_t caps = 0;
    bool enabled = true;    // the assistant may use it
    bool ask_first = false; // ask aloud before switching it on (a heater, say)
    json ref;               // driver-private addressing
    json state;             // last read: {"on":true,"brightness":40,"temperature":19.5,...}
};

// What to do to a thing. Fields left empty are left alone.
struct Change {
    std::optional<bool> on;
    std::optional<int> brightness;           // 0..100
    std::optional<int> kelvin;
    std::optional<std::pair<int, int>> hs;   // hue 0..360, saturation 0..100
    std::optional<double> target_c;
    std::optional<std::string> mode;         // off, heat, auto
    std::optional<int> position;             // 0..100
    std::optional<int> fan;                  // 0..100
    std::optional<int> volume;               // 0..100
    std::optional<int> volume_step;          // +n / -n
    std::optional<bool> mute;
    std::optional<std::string> transport;    // play, pause, stop, next, previous
    std::optional<std::string> source;       // input or app name
    std::optional<std::string> key;          // TV keys: power, volume_up, volume_down, mute, channel_up,
                                             // channel_down, input, up, down, left, right, ok, menu, back, home, 0..9
};

// A saved device as a driver sees it.
struct Device {
    std::string id, kind, name, model, mac, ip;
    int port = 0;
    std::map<std::string, std::string> info;   // what discovery reported (TXT keys, scheme, ...)
    json auth = json::object();                // this device's secrets and pairing data; never logged or shown
    std::vector<Thing> things;
};

// One-time pairing, as a sequence of steps the settings page walks through.
enum class Pairing { None, PressButton, EnterCode, AcceptPrompt, Password, Token };

struct PairStep {
    Pairing kind = Pairing::None;
    std::string prompt;     // "Press the round button on the Hue Bridge"
    int wait_s = 0;         // for button and prompt steps: keep trying for this long
    std::string field;      // label of the text field for a code, password or token
    // What is typed is no secret (an e-mail, a user name): shown as it is typed. A code, password or token is
    // hidden while it is typed (Menu::edit_text's secret).
    bool plain = false;
};

// Secrets shared by every device of a family (an account, this controller's own key pair).
// Kept in the same 0600 file as the devices.
class Secrets {
public:
    virtual ~Secrets() = default;
    virtual json get(const std::string &family) const = 0;            // {} when nothing is stored
    virtual void set(const std::string &family, const json &value) = 0;
    // As set(), and whether it was written: false when the file could not be (a full or read-only
    // store). A driver about to send a credential checks this first (trial below).
    virtual bool put(const std::string &family, const json &value) {
        set(family, value);
        return true;
    }
};

// A password, code or key the user has typed is on trial until a device first accepts it. A refused
// one is never sent again by itself, not even by a later run (a Tapo plug locks itself after a
// few), so the trial is written where the credential is kept (`holder`: the family's secrets, or
// Device::auth) before it is first sent, and nothing is sent when that write fails:
//   typed(h)   just typed: nothing sent yet
//   begin(h)   about to be sent for the first time in this run: true when `h` changed and must be
//              written, successfully, before anything is sent
//   stale(h)   sent by an earlier run that never learnt the outcome (it stopped, or could not record
//              a refusal): it counts as refused until it is typed again
//   end(h)     accepted (or dropped): true when `h` changed and should be written
// A holder without a trial (saved before trials existed, or accepted) is not on trial.
//
// A refusal is also kept in memory for the rest of the run (refuse), so that one the file could not
// take still stops this run: checked as refused(), forgiven when the user types a credential for the
// slot again. `slot` names where the credential is kept (a family's account or key, one device's
// password); `print` tells that credential apart from others typed (a hash of it). The memory is the
// protection within a run, and cannot fail; the file is the one across runs, and a write can. A driver
// says when the file could not take a refusal: one still on trial counts as refused in a later run
// anyway (stale), but one a device had accepted before is then held by the memory alone, and could be
// tried once more after a restart.
namespace trial {
void typed(json &holder);
bool begin(json &holder);
bool stale(const json &holder);
bool end(json &holder);
void refuse(const std::string &slot, const std::string &print);
bool refused(const std::string &slot, const std::string &print);
void forgive(const std::string &slot);
}  // namespace trial

// Who answered at a device's saved address, as the shared layer reads a driver's error (defined in
// control.cpp). The router may have given the device another address: the layer looks for it on the
// network (a short scan, 2.5 s) when nothing answered at the saved one, or another device did. A
// device that answered, only wrongly (a reply that could not be read, an error code, a refusal), is
// where it was: looked for, it was searched for in vain and then called "not found at another address
// ... switched off at the wall" (ZB-108). The words, in any capitals:
//   Nobody    "did not answer", "could not be reached", "cannot connect", "refused the connection",
//             "closed the connection"
//   Another   "answers at this" or "answered at this" (address): another device, one that would not
//             say which device it is, or an answer none of the family would give, before the device
//             has said who it is (Tapo's login comes first: a web server's 404 to it)
//   Device    any other error: the device answered, or nothing was sent
enum class Answered { Device, Nobody, Another };
Answered who_answered(const std::string &err);

class Driver {
public:
    virtual ~Driver() = default;
    virtual const char *kind() const = 0;     // "tapo", "hue_bridge", "shelly", "homekit", ...
    // Whether a discovered device of this kind/model is one this driver handles.
    virtual bool claims(const std::string &device_kind, const std::map<std::string, std::string> &) const {
        return device_kind == kind();
    }
    virtual void attach(Secrets *secrets) { secrets_ = secrets; }

    // Pairing. next_step is asked until it returns None; pair is called with what the user
    // typed ("" for button and prompt steps) and stores what it learns in dev.auth.
    // A code or password the device rejected must never be retried automatically.
    virtual PairStep next_step(const Device &) const { return {}; }
    virtual bool pair(Device &, const std::string & /*input*/, std::string & /*err*/) { return true; }
    // Forgets the pairing by clearing dev.auth. A driver that must also tell the device (HomeKit)
    // leaves dev.auth as it was when it could not, so that Unpair can be tried again: the app
    // keeps whatever unpair leaves.
    virtual void unpair(Device &dev) { dev.auth = json::object(); }

    // Lists the device's things with names, rooms, types, capabilities and current state.
    virtual bool enumerate(Device &dev, std::vector<Thing> &out, std::string &err) = 0;
    // Refreshes the state of the given things (all of the device's when empty).
    virtual bool read(const Device &dev, std::vector<Thing> &things, std::string &err) = 0;
    // Applies one change to one thing within deadline_ms.
    virtual bool apply(const Device &dev, const Thing &thing, const Change &change, int deadline_ms, std::string &err) = 0;
    // Optional: one request for several things on the same device. Returns false when not supported.
    virtual bool apply_many(const Device &, const std::vector<const Thing *> &, const Change &, int /*deadline_ms*/,
                            std::string & /*err*/) {
        return false;
    }

protected:
    Secrets *secrets_ = nullptr;
};

}  // namespace home
