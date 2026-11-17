// Smart-home devices the user has let the app find, remembered per network, and the commands the
// assistant can give them. Nothing here ever looks at a network the user has not scanned on
// purpose from the settings page.
#pragma once
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "core/util.h"
#include "smarthome/discovery.h"

namespace home {

// Something that can be switched: a plug, or one socket of a power strip.
struct Switch {
    std::string name;          // what the user calls it ("Bed heater"), as set in the maker's app
    std::string child;         // socket id on a strip, "" for a plain plug
    bool on = false;           // as last read
};

struct Saved {
    std::string id, kind, name, model, mac, ip;
    int port = 0;
    std::map<std::string, std::string> info;
    std::vector<std::string> found_by;
    bool enabled = true;       // the assistant may use it
    time_t last_seen = 0;
    std::vector<Switch> switches;   // filled once the device has been read (needs the account for Tapo)
    std::string problem;       // why it could not be read last time, shown in settings
    bool controllable() const;      // a kind this app knows how to drive
    std::string title() const;      // best name for a list
};

struct NetworkEntry {
    std::string id, label, gateway_mac;
    time_t allowed_at = 0, last_scan = 0;
    std::vector<Saved> devices;
};

struct TapoAccount {
    std::string email;
    std::string password_sha1;   // the password itself is never kept
    bool set() const { return !email.empty() && !password_sha1.empty(); }
};

// The network the device is on, looked up at most every few seconds.
discovery::Network network_now(bool fresh = false);

bool scanned(const std::string &network_id);
NetworkEntry entry(const std::string &network_id);
// Folds one scan into what is saved for the network: new devices are added, known ones keep
// their settings and get their current address. Returns how many were new.
int remember_scan(const discovery::Network &net, const std::vector<discovery::Device> &found);
void set_enabled(const std::string &network_id, const std::string &device_id, bool on);
void remove_device(const std::string &network_id, const std::string &device_id);
void forget_network(const std::string &network_id);

TapoAccount tapo_account();
void set_tapo_account(const std::string &email, const std::string &password);   // "" password keeps the saved one

// Reads names and states from every controllable device on the network (logging in where the
// maker requires it) and saves them. Returns a short summary for the settings page.
std::string refresh(const std::string &network_id);

// ---- what the assistant's tool uses; all of it applies to the current network only
std::vector<std::string> switch_names();                 // names of enabled switches, "" when none
bool available();                                        // there is something to control here
// `name` is matched loosely against the saved names. Returns text for the model.
std::string switch_power(const std::string &name, bool on);
std::string status(const std::string &name);             // "" lists everything
std::string other_devices();                             // found but not controllable, for the model's information

}  // namespace home
