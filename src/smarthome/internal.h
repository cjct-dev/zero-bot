// Shared between the smart-home source files; not for the rest of the app.
#pragma once
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>

#include "smarthome/home.h"

namespace home {

// Loads the network's entry, lets `change` edit it and saves it, all under the file lock.
// Returns false when the network is not saved. `change` must not call back into this layer.
bool modify_entry(const std::string &network_id, const std::function<void(NetworkEntry &)> &change);

// One conversation with a device at a time; different devices do not wait for each other.
std::mutex &device_lock(const std::string &device_id);

// Whether `network_id` is the network the device is on right now and may be used. Every path
// that ends in a driver call checks this first.
bool on_network(const std::string &network_id, std::string &why_not);
// Whether this device's address may be contacted at all: on the stand-in test network only a
// 127.x address may.
bool may_contact(const Device &device, std::string &why_not);

// Only these types of thing are ever kept or offered (survey 6.5: an allow-list).
bool allowed_type(const std::string &type);

// Folds a fresh list of things from a driver into what is saved for the device.
void merge_things(Saved &device, std::vector<Thing> fresh, time_t now);

unsigned long now_ms();

// Runs job(0..count-1) on at most `workers` threads and returns when all have finished.
void run_parallel(size_t count, size_t workers, const std::function<void(size_t)> &job);

// At most 12 changes a minute in all, and 3 in ten seconds for one target: contains a model
// that has started to loop.
class RateLimiter {
public:
    bool allow(const std::string &target, unsigned long at_ms, std::string &why_not);
    void reset();

private:
    std::deque<unsigned long> all_;
    std::map<std::string, std::deque<unsigned long>> per_target_;
};

// Letters and digits only, lower case: "Bed-heater" and "bed heater" are the same thing said aloud.
std::string plain(const std::string &text);

// For the device-free tests only.
namespace testing {
void set_network(const discovery::Network *network);      // stands in for the real network; nullptr restores it
void set_clock(unsigned long (*clock_ms)());               // nullptr restores the real clock
void reset_control();                                      // forgets pending questions, rate counts, turn counters
void clear_drivers();                                      // empties the driver table (and stops the built-in list loading)
}  // namespace testing

}  // namespace home
