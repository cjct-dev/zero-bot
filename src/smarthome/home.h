// Smart-home devices the user has let the app find, remembered per network, and the commands the
// assistant can give them. Nothing here ever looks at a network the user has not scanned on
// purpose from the settings page, and every call that reaches a device goes through
// network_now(): on a phone's Bluetooth tether, a VPN or with no network there is no home.
//
// The parts:
//   driver.h              what one family of devices implements (things, changes, pairing)
//   drivers/registry.cpp  the one list of families built into the app
//   store.cpp             the 0600 file: networks, devices, things, secrets; migration; the network
//   targets.cpp           what a spoken name means: a thing, a room, a group
//   control.cpp           carrying things out: safety rules, locking, fan-out, status, refresh, pairing
//   tool.cpp              the assistant's smart_home tool: schema and arguments
#pragma once
#include <ctime>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/util.h"
#include "smarthome/discovery.h"
#include "smarthome/driver.h"

namespace home {

// State keys this layer understands; drivers fill in what applies. Only these ever reach the
// model, so a driver can keep anything else it likes in Thing::state without it leaking.
//   on (bool), brightness (0..100), kelvin, hue (0..360), saturation (0..100),
//   mode ("off", "heat", "auto"), target_c, current_c, humidity (percent),
//   position (0..100), fan (0..100), volume (0..100), muted (bool),
//   playing ("playing", "paused", "stopped", "idle"), source, title (what is playing),
//   power_w, energy_today_wh, motion (bool), contact ("open", "closed")

// What the app keeps about a thing beside what its driver reports.
struct ThingMeta {
    bool room_by_user = false;   // the room was typed in settings, so a bridge's room does not replace it
    time_t used = 0;             // when the assistant last changed it
    time_t gone = 0;             // when its device stopped listing it; kept a while for its settings
};

struct Saved : Device {
    std::vector<std::string> found_by;
    bool enabled = true;         // the assistant may use it
    bool needs_setup = false;    // its driver wants pairing before anything else
    time_t last_seen = 0;
    std::string problem;         // why it could not be read last time, shown in settings
    std::map<std::string, ThingMeta> meta;   // by Thing::id
    bool controllable() const;   // a driver built into the app claims it
    std::string title() const;   // best name for a list
    const Thing *thing(const std::string &thing_id) const;
    bool listed(const Thing &thing) const;   // still reported by the device (not gone)
    bool any_listed() const;                 // it reported at least one thing last time it was read
};

struct NetworkEntry {
    std::string id, label, gateway_mac;
    time_t allowed_at = 0, last_scan = 0;
    std::vector<Saved> devices;
};

// ---- drivers
// `name` is what the settings pages call the family's devices ("Tapo", "Hue Bridge").
void register_driver(std::unique_ptr<Driver> driver, const std::string &name = "");
Driver *driver_for(const std::string &kind);      // the driver whose kind() this is
Driver *driver_for(const Device &device);         // the driver that claims this device
// The name of a device's family for a list: its driver's, else a known kind's ("Sonos" for a
// device no driver claims yet), else "Device".
std::string kind_label(const Device &device);
// Defined in drivers/registry.cpp: registers every family built into the app. Called once, the
// first time a driver is looked up.
void register_builtin_drivers();
// Secrets shared by a family (an account, this controller's own key pair), kept in the same file.
Secrets &secrets();

// ---- the network the device is on, looked up at most every few seconds
discovery::Network network_now(bool fresh = false);
// ZB_TEST_NETWORK is set: network_now() is a stand-in ("Test network") instead of the real one.
// On it only devices at a 127.x address are ever contacted, and nothing is scanned.
bool test_network();
// Runs a discovery scan of the current network, unless the stand-in network is in use.
bool scan(std::vector<discovery::Device> &found, std::string &err, bool *heard_self = nullptr);

// ---- what is saved
bool scanned(const std::string &network_id);
NetworkEntry entry(const std::string &network_id);
// Folds one scan into what is saved for the network: new devices are added, known ones keep
// their settings and things and get their current address. Returns how many were new.
int remember_scan(const discovery::Network &net, const std::vector<discovery::Device> &found);
void set_enabled(const std::string &network_id, const std::string &device_id, bool on);
void remove_device(const std::string &network_id, const std::string &device_id);
void forget_network(const std::string &network_id);
// What the user decides about one thing. These survive every later read of the device.
void set_thing_alias(const std::string &network_id, const std::string &device_id, const std::string &thing_id, const std::string &alias);
void set_thing_room(const std::string &network_id, const std::string &device_id, const std::string &thing_id, const std::string &room);
void set_thing_enabled(const std::string &network_id, const std::string &device_id, const std::string &thing_id, bool on);
void set_thing_ask_first(const std::string &network_id, const std::string &device_id, const std::string &thing_id, bool on);
std::vector<std::string> rooms(const std::string &network_id);   // every room in use, for the settings page

// ---- pairing, driven from the settings page. All of it applies to the current network only.
// What the device's driver wants next; kind None when it is ready (or cannot be asked).
PairStep pairing_step(const std::string &network_id, const std::string &device_id);
// Hands the driver what the user typed ("" for a button or prompt step). A refusal is reported
// and never tried again by itself.
bool pairing_submit(const std::string &network_id, const std::string &device_id, const std::string &input, std::string &err);
// "" once the pairing is gone; otherwise why it is kept (the device could not be told).
std::string unpair(const std::string &network_id, const std::string &device_id);

// Asks every controllable device on the network (or just one) for its things, names and states,
// and saves them: aliases, rooms and the two switches a user sets survive. Returns a short
// summary for the settings page.
std::string refresh(const std::string &network_id, const std::string &only_device = "");
// Once a device's pairing has finished: reads it, and every other device of its family that the
// pairing made ready and that has not been read yet (one Tapo account serves every Tapo plug, so
// typing it for one readies them all).
std::string refresh_after_pairing(const std::string &network_id, const std::string &device_id);

// ---- names
std::string clean_name(const std::string &raw);             // untrusted text made safe to show and to pass as data
std::string spoken(const Thing &thing);                     // alias when set, else the maker's name
std::string infer_room(const std::string &name);            // "Bedroom lamp" gives "Bedroom"; "" when unknown
bool suggests_heat(const std::string &name);                // a plug that should ask before it is switched on

// ---- targets: what a spoken name refers to (targets.cpp)
struct Item {                    // one enabled thing of an enabled, ready device
    std::string device_id;
    Thing thing;
    std::string label;           // how it is listed for the model; unique within the network
    time_t used = 0;
};
enum class Scope { None, Thing, Room, AllLights, AllSpeakers, Everything };
struct Resolved {
    Scope scope = Scope::None;
    std::string label;                    // the thing's label, the room's name, "all lights", ...
    std::vector<const Item *> members;    // every thing the name covers, before an action narrows it
};
constexpr size_t kMaxTargets = 48;
std::vector<Item> catalog(const NetworkEntry &entry);
bool is_light(const Thing &thing);
// The values offered to the model: things, then rooms that have lights, then groups. `overflow`
// is set when not everything fitted, in which case other names are still understood by resolve().
std::vector<std::string> target_names(const std::vector<Item> &items, bool *overflow = nullptr);
// Exact name first, then a partial one; several candidates are reported, never guessed between.
bool resolve(const std::vector<Item> &items, const std::string &said, Resolved &out, std::string &err);

// ---- control (control.cpp)
struct Command {
    std::string action;          // on, off, set, status, scene, play, pause, stop, next, previous, mute, unmute, press
    std::string target;          // "" only for status: everything
    std::optional<int> brightness, position, volume;
    std::optional<int> volume_step;   // volume key presses: +n louder, -n quieter (a TV with only volume keys)
    std::string key;                  // press: a remote-control key (kTvKeys)
    std::optional<double> temperature;
    std::string color, source;
};
// The remote-control keys the press action offers (driver.h Change::key lists every name a driver
// may take; power, volume and mute have actions or values of their own).
inline const char *const kTvKeys[] = {"up",   "down",   "left",          "right",      "ok",           "back", "home",
                                      "menu", "rewind", "fast_forward", "channel_up", "channel_down", "input"};
// Carries a command out on the current network and returns text for the model: compact JSON,
// "ERROR: ..." or "CONFIRM: ...". With dry_run nothing is sent to any device.
std::string execute(const Command &command, bool dry_run);
// Switching from the settings page, by hand: no question is asked and no name is matched.
std::string switch_thing(const std::string &network_id, const std::string &device_id, const std::string &thing_id, bool on);
// The request loop reports what the user says and every reply that goes to them, so that a
// change that needs a yes goes ahead only after its question was put (a reply that asks, naming
// the thing) and the user's very next message answered it with a short, plain yes.
void user_spoke(const std::string &text);
void assistant_replied(const std::string &reply);
// A tool has just handed the model text from outside (a web page, a photo). From now on every
// change needs the user's yes, for as long as the model can still see that text: until the
// conversation is cleared, or has grown so long that the message it came with is dropped.
void saw_untrusted_text();
// The conversation was cleared: nothing read in it is seen by the model any more, and no
// question asked in it is still open.
void conversation_reset();
// How many of the user's messages (the newest) the model is still shown, reported each time the
// loop shortens the conversation; text from outside read before the oldest of them is gone.
void context_kept(size_t user_messages);

// ---- the assistant's tool (tool.cpp); all of it applies to the current network only
bool available();                                // there is something to control here
size_t thing_count();
// {"description": ..., "parameters": {...}} sized to what this home has, or null when the tool
// should not be offered at all.
json tool_spec();
std::string tool_run(const json &args, bool dry_run);
// Whether the sentence the model supplied in "say" may be spoken straight after this result.
bool tool_say_ok(const json &args, const std::string &result);

}  // namespace home
