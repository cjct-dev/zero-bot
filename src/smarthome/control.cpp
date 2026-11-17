// Carrying things out: the commands the assistant gives, with the safety rules of the design
// (questions before risky changes, rate limits, nothing on a network the user has not scanned)
// enforced here in code rather than left to the model; reading state; refreshing what devices
// offer; pairing. Each device is talked to by one thread at a time, and a command for several
// devices fans out over at most four threads, all joined before the command returns.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <set>
#include <thread>

#include "smarthome/color.h"
#include "smarthome/internal.h"

namespace home {

namespace {

constexpr int kApplyDeadlineMs = 2500;      // per device, for a change
constexpr int kReadDeadlineMs = 4000;       // per device, for reading its things
constexpr size_t kWorkers = 4;              // threads a command for several devices may use
constexpr size_t kMaxAtOnce = 8;            // more things than this in one command need a yes
constexpr size_t kStatusMax = 30;           // things listed by a status of everything
constexpr unsigned long kAnswerWithinMs = 3 * 60 * 1000;   // a yes must come within this
constexpr double kMinC = 5, kMaxC = 30;     // heating targets are clamped to this range

const char *const kActions[] = {"on", "off", "set", "status", "scene", "play", "pause", "stop", "next", "previous", "mute", "unmute"};

std::string fail(const std::string &why) { return "ERROR: " + why; }

std::string dump(const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

std::string number_text(double v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f", v);
    std::string s = buf;
    if (s.size() > 2 && s.compare(s.size() - 2, 2, ".0") == 0) s.resize(s.size() - 2);
    return s;
}

// ---------------------------------------------------------------- per-device locks
std::mutex g_locks_mtx;
std::map<std::string, std::unique_ptr<std::mutex>> g_locks;

// ---------------------------------------------------------------- the conversation, as far as this layer needs it
// A change that needed the user's yes. It goes ahead only when the identical change is asked for
// again in the very next user message, that message reads as a yes, and a reply (carrying the
// question) went to the user in between. Nothing the model passes can stand in for this.
struct Pending {
    std::string signature;
    unsigned long at_ms = 0;
    unsigned long turn = 0;      // the user message during which it was asked for
    bool asked = false;          // a reply went to the user after it
};

std::mutex g_ctl_mtx;            // guards everything in this group
unsigned long g_turn = 0;        // user messages so far
bool g_last_yes = false;         // the newest user message reads as a yes
bool g_untrusted = false;        // text from outside has reached the model ...
unsigned long g_untrusted_turn = 0;   // ... since this user message
std::vector<Pending> g_pending;
RateLimiter g_rate;

// A short answer that agrees and does not also refuse ("yes, no wait" is not a yes). Spoken
// answers come in many forms; one that is not recognised only means the question is asked again.
bool sounds_like_yes(const std::string &text) {
    const std::string p = " " + plain(text) + " ";
    for (const char *no : {" no ", " nope ", " not ", " don t ", " dont ", " do not ", " never ", " cancel ", " stop ", " wait ",
                           " nein ", " nicht ", " non ", " pas ", " não ", " nao ", " nee ", " niet ", " nie "})
        if (p.find(no) != std::string::npos) return false;
    for (const char *yes : {" yes ", " yeah ", " yea ", " yep ", " yup ", " aye ", " sure ", " ok ", " okay ", " please ",
                            " go ahead ", " go on ", " go for it ", " do it ", " please do ", " confirm ", " confirmed ",
                            " correct ", " right ", " absolutely ", " definitely ", " of course ", " affirmative ",
                            " that s right ", " turn it on ", " switch it on ", " si ", " sí ", " claro ", " vale ", " oui ",
                            " d accord ", " ja ", " jawohl ", " genau ", " klar ", " sim ", " certo ", " tak ", " da "})
        if (p.find(yes) != std::string::npos) return true;
    return false;
}

// ---------------------------------------------------------------- changes
json change_json(const Change &c) {
    json j = json::object();
    if (c.on) j["on"] = *c.on;
    if (c.brightness) j["brightness"] = *c.brightness;
    if (c.kelvin) j["kelvin"] = *c.kelvin;
    if (c.hs) { j["hue"] = c.hs->first; j["saturation"] = c.hs->second; }
    if (c.target_c) j["target_c"] = *c.target_c;
    if (c.mode) j["mode"] = *c.mode;
    if (c.position) j["position"] = *c.position;
    if (c.fan) j["fan"] = *c.fan;
    if (c.volume) j["volume"] = *c.volume;
    if (c.volume_step) j["volume_step"] = *c.volume_step;
    if (c.mute) j["muted"] = *c.mute;
    if (c.transport) j["transport"] = *c.transport;
    if (c.source) j["source"] = *c.source;
    if (c.key) j["key"] = *c.key;
    return j;
}

// What a thing's saved state becomes once a change to it has worked.
void apply_to_state(const Change &c, Thing &t) {
    if (!t.state.is_object()) t.state = json::object();
    if (t.type == "scene") return;   // a scene is an event, not a state
    json j = change_json(c);
    if (j.contains("transport")) {
        const std::string tr = j["transport"];
        if (tr == "play") t.state["playing"] = "playing";
        else if (tr == "pause") t.state["playing"] = "paused";
        else if (tr == "stop") t.state["playing"] = "stopped";
        j.erase("transport");
    }
    if (j.contains("hue")) t.state.erase("kelvin");
    if (j.contains("kelvin")) { t.state.erase("hue"); t.state.erase("saturation"); }
    j.erase("volume_step");
    j.erase("key");
    for (auto it = j.begin(); it != j.end(); ++it) t.state[it.key()] = it.value();
}

// The change as this thing can take it, or false with the reason it cannot.
bool fit(const Change &c, const Thing &t, Change &out, std::string &why) {
    out = c;
    const uint32_t caps = t.caps;
    if (t.type == "scene") {
        const bool only_on = c.on && *c.on && !c.brightness && !c.kelvin && !c.hs && !c.target_c && !c.position &&
                             !c.volume && !c.mute && !c.transport && !c.source;
        if (!only_on) { why = "it is a scene, which can only be started"; return false; }
        return true;
    }
    if (c.on && !(caps & kPower)) {
        if (caps & kTargetTemp) {            // heating: on and off are its mode
            out.on.reset();
            if (!out.mode) out.mode = *c.on ? "heat" : "off";
        } else if (caps & kPosition) {       // blinds: on and off are open and closed
            out.on.reset();
            if (!out.position) out.position = *c.on ? 100 : 0;
        } else {
            why = "it cannot be switched on or off";
            return false;
        }
    }
    if (c.brightness && !(caps & kBrightness)) { why = "it cannot be dimmed"; return false; }
    if (c.kelvin && !(caps & kColorTemp)) {
        if (!(caps & kColor)) { why = "its colour cannot be changed"; return false; }
        int h = 0, s = 0;
        color::kelvin_to_hs(*c.kelvin, h, s);
        out.kelvin.reset();
        out.hs = std::make_pair(h, s);
    }
    if (c.hs && !(caps & kColor)) { why = "it can only be set to shades of white"; return false; }
    if (c.target_c && !(caps & kTargetTemp)) { why = "it has no temperature setting"; return false; }
    if (out.position && !(caps & kPosition)) { why = "it cannot be opened or closed"; return false; }
    if (c.volume && !(caps & kVolume)) { why = "its volume cannot be set to a level"; return false; }
    if (c.mute && !(caps & kVolume)) { why = "it cannot be muted"; return false; }
    if (c.transport && !(caps & kTransport)) { why = "it has no playback controls"; return false; }
    if (c.source && !(caps & kSource)) { why = "its input cannot be changed"; return false; }
    return true;
}

struct Step {
    const Item *item;
    Change change;
    bool ok = false;
    std::string err;
};

std::string describe(const std::string &action, const std::string &label, const Change &c) {
    if (action == "on") return "turn on " + label;
    if (action == "off") return "turn off " + label;
    if (action == "scene") return "start the scene " + label;
    if (action == "mute" || action == "unmute" || action == "play" || action == "pause" || action == "stop") return action + " " + label;
    if (action == "next" || action == "previous") return "skip to the " + action + " track on " + label;
    std::vector<std::string> parts;
    if (c.brightness) parts.push_back("brightness " + std::to_string(*c.brightness) + "%");
    if (c.kelvin || c.hs) parts.push_back("the colour asked for");
    if (c.target_c) parts.push_back(number_text(*c.target_c) + " degrees");
    if (c.position) parts.push_back("position " + std::to_string(*c.position) + "%");
    if (c.volume) parts.push_back("volume " + std::to_string(*c.volume));
    if (c.source) parts.push_back(*c.source);
    std::string what;
    for (const auto &p : parts) what += (what.empty() ? "" : ", ") + p;
    return "set " + label + " to " + what;
}

std::string capitalised(std::string s) {
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

// Why this plan needs the user's yes first, or "" when it does not.
std::string needs_yes(const Resolved &r, const Change &base, const std::vector<Step> &steps) {
    for (const auto &s : steps)
        if (s.change.on && *s.change.on && s.item->thing.ask_first)
            return s.item->label + " is set to be switched on only after the user says yes";
    for (const auto &s : steps) {
        if (!s.change.target_c) continue;
        const double t = *s.change.target_c;
        if (t > 25 || t < 10) return number_text(t) + " degrees is an unusual heating temperature";
        const json &st = s.item->thing.state;
        if (st.contains("target_c") && st["target_c"].is_number() && std::fabs(t - st["target_c"].get<double>()) > 5)
            return "that changes the heating by more than 5 degrees in one go";
    }
    const bool all_lights_off = r.scope == Scope::AllLights && base.on && !*base.on;
    if (steps.size() > kMaxAtOnce && !all_lights_off) return "it would change " + std::to_string(steps.size()) + " devices at once";
    size_t blinds = 0;
    for (const auto &s : steps) blinds += s.change.position ? 1 : 0;
    if (blinds > 1) return "it would move " + std::to_string(blinds) + " blinds at once";
    for (const auto &s : steps)
        if (s.change.volume && *s.change.volume > 70) return "a volume of " + std::to_string(*s.change.volume) + " is loud";
    std::lock_guard<std::mutex> lk(g_ctl_mtx);
    if (g_untrusted && g_untrusted_turn == g_turn)
        return "this request involved text from outside (a web page or a photo), and devices change only at the user's own word";
    return "";
}

std::string signature(const std::string &action, const std::vector<Step> &steps) {
    std::vector<std::string> parts;
    for (const auto &s : steps) parts.push_back(s.item->device_id + "\n" + s.item->thing.id + "\n" + change_json(s.change).dump());
    std::sort(parts.begin(), parts.end());
    std::string sig = action;
    for (const auto &p : parts) sig += "\n\n" + p;
    return sig;
}

// Whether this exact change was put to the user as a question and they have just said yes.
// A matching question that has not been answered that way is replaced.
bool answered_yes(const std::string &sig) {
    std::lock_guard<std::mutex> lk(g_ctl_mtx);
    const unsigned long now = now_ms();
    for (auto it = g_pending.begin(); it != g_pending.end(); ++it) {
        if (it->signature != sig) continue;
        const bool ok = it->asked && g_turn == it->turn + 1 && g_last_yes && now - it->at_ms <= kAnswerWithinMs;
        g_pending.erase(it);
        if (ok) return true;
        break;
    }
    Pending p;
    p.signature = sig;
    p.at_ms = now;
    p.turn = g_turn;
    g_pending.push_back(p);
    if (g_pending.size() > 4) g_pending.erase(g_pending.begin());
    return false;
}

// ---------------------------------------------------------------- talking to devices
// A device whose saved address no longer answers is looked for again by asking the network, and
// recognised by its identifier, never by going through addresses. One scan serves every device
// that needs it for a short while. Never on the stand-in test network.
bool relocate(const std::string &network_id, Saved &device) {
    if (test_network()) return false;
    static std::mutex mtx;
    static std::vector<discovery::Device> found;
    static std::string found_on;
    static unsigned long found_at = 0;
    std::lock_guard<std::mutex> lk(mtx);
    const discovery::Network net = network_now(true);
    if (!net.usable || net.id() != network_id) return false;
    if (found_on != network_id || found_at == 0 || now_ms() - found_at > 20000) {
        std::vector<discovery::Device> fresh;
        std::string err;
        discovery::Options opt;
        opt.listen_ms = 2500;
        if (!discovery::scan(opt, fresh, err)) return false;
        found = fresh;
        found_on = network_id;
        found_at = now_ms() ? now_ms() : 1;
        remember_scan(net, found);
    }
    for (const auto &f : found)
        if (f.id == device.id && f.ip != device.ip) {
            device.ip = f.ip;
            device.port = f.port;
            device.info = f.info;
            return true;
        }
    return false;
}

// Whether a driver's error reads as "it did not answer" rather than "it said no".
bool unreachable(const std::string &err) {
    const std::string e = util::lower(err);
    for (const char *w : {"connect", "reach", "answer", "respond", "timed out", "timeout", "no route", "unreachable"})
        if (e.find(w) != std::string::npos) return true;
    return false;
}

bool call_apply(Driver *drv, const Device &dev, const Thing &t, const Change &c, std::string &err) {
    try {
        err.clear();
        if (drv->apply(dev, t, c, kApplyDeadlineMs, err)) return true;
    } catch (const std::exception &) {
        err = "the device's answer could not be understood";
    }
    if (err.empty()) err = "it did not do it";
    return false;
}

// Carries out the steps for one device; the caller holds no lock.
void apply_device(const std::string &network_id, const Saved &saved, std::vector<Step *> &steps) {
    Driver *drv = driver_for(saved);
    std::string why;
    if (!drv) why = "this kind of device cannot be controlled";
    else if (!may_contact(saved, why) && why.empty()) why = "it may not be contacted";
    if (!why.empty()) {
        for (auto *s : steps) s->err = why;
        return;
    }
    Saved dev = saved;
    std::lock_guard<std::mutex> lk(device_lock(dev.id));
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::vector<Step *> todo;
        for (auto *s : steps)
            if (!s->ok) todo.push_back(s);
        if (todo.empty()) return;
        bool same = todo.size() > 1;
        for (auto *s : todo) same = same && change_json(s->change) == change_json(todo[0]->change);
        bool handled = false;
        if (same) {
            // Several things on one device with one change: a single request where the driver has one.
            std::vector<const Thing *> things;
            for (auto *s : todo) things.push_back(&s->item->thing);
            std::string err;
            bool ok = false;
            try {
                ok = drv->apply_many(dev, things, todo[0]->change, kApplyDeadlineMs, err);
            } catch (const std::exception &) {
                err = "the device's answer could not be understood";
            }
            if (ok || !err.empty()) {
                handled = true;
                for (auto *s : todo) { s->ok = ok; s->err = ok ? "" : err; }
            }
        }
        if (!handled)
            for (auto *s : todo) s->ok = call_apply(drv, dev, s->item->thing, s->change, s->err);
        // No answer at the saved address: the router may have given the device a new one. Tried
        // once, and only when the device turns up somewhere else.
        const bool none_ok = std::none_of(todo.begin(), todo.end(), [](const Step *s) { return s->ok; });
        if (attempt > 0 || !none_ok || !unreachable(todo[0]->err) || !relocate(network_id, dev)) return;
        modify_entry(network_id, [&](NetworkEntry &e) {
            for (auto &d : e.devices)
                if (d.id == dev.id) { d.ip = dev.ip; d.port = dev.port; d.info = dev.info; }
        });
    }
}

// Fans steps out by device: at most kWorkers devices at once, each device's steps in turn.
void run_steps(const std::string &network_id, const NetworkEntry &e, std::vector<Step> &steps) {
    std::vector<std::pair<const Saved *, std::vector<Step *>>> jobs;
    for (auto &s : steps) {
        auto it = std::find_if(jobs.begin(), jobs.end(), [&](const auto &j) { return j.first->id == s.item->device_id; });
        if (it == jobs.end()) {
            const Saved *dev = nullptr;
            for (const auto &d : e.devices)
                if (d.id == s.item->device_id) dev = &d;
            if (!dev) { s.err = "it is no longer saved"; continue; }
            jobs.push_back({dev, {}});
            it = jobs.end() - 1;
        }
        it->second.push_back(&s);
    }
    run_parallel(jobs.size(), kWorkers, [&](size_t k) { apply_device(network_id, *jobs[k].first, jobs[k].second); });
}

std::string key_of(const std::string &device_id, const std::string &thing_id) { return device_id + "\n" + thing_id; }

// Reads the state of these things. `detail`: they were asked about by name, so the driver is
// handed exactly them (and may read sensors that cost another request); otherwise it is handed
// none, meaning "all of yours, briefly". Fills states and errors by key_of().
void read_now(const std::string &network_id, const NetworkEntry &e, const std::vector<const Item *> &members, bool detail,
              std::map<std::string, json> &states, std::map<std::string, std::string> &errors) {
    std::vector<std::pair<const Saved *, std::vector<const Item *>>> jobs;
    for (const auto *m : members) {
        auto it = std::find_if(jobs.begin(), jobs.end(), [&](const auto &j) { return j.first->id == m->device_id; });
        if (it == jobs.end()) {
            const Saved *dev = nullptr;
            for (const auto &d : e.devices)
                if (d.id == m->device_id) dev = &d;
            if (!dev) continue;
            jobs.push_back({dev, {}});
            it = jobs.end() - 1;
        }
        it->second.push_back(m);
    }
    std::mutex out_mtx;
    run_parallel(jobs.size(), kWorkers, [&](size_t k) {
        Saved dev = *jobs[k].first;
        const auto &mine = jobs[k].second;
        Driver *drv = driver_for(dev);
        std::string err;
        std::vector<Thing> things;
        bool ok = false;
        if (!drv) err = "this kind of device cannot be read";
        else if (may_contact(dev, err)) {
            std::lock_guard<std::mutex> lk(device_lock(dev.id));
            for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
                things.clear();
                if (detail)
                    for (const auto *m : mine) things.push_back(m->thing);
                err.clear();
                try {
                    ok = drv->read(dev, things, err);
                } catch (const std::exception &) {
                    err = "the device's answer could not be understood";
                }
                if (!ok && err.empty()) err = "it did not answer";
                // Re-discovery is worth its wait only for things asked about by name.
                if (ok || attempt > 0 || !detail || !unreachable(err) || !relocate(network_id, dev)) break;
                modify_entry(network_id, [&](NetworkEntry &en) {
                    for (auto &d : en.devices)
                        if (d.id == dev.id) { d.ip = dev.ip; d.port = dev.port; d.info = dev.info; }
                });
            }
        }
        std::lock_guard<std::mutex> lk(out_mtx);
        for (const auto *m : mine) {
            const std::string key = key_of(m->device_id, m->thing.id);
            if (!ok) { errors[key] = err; continue; }
            const auto t = std::find_if(things.begin(), things.end(), [&](const Thing &x) { return x.id == m->thing.id; });
            states[key] = t != things.end() && t->state.is_object() ? t->state : m->thing.state;
        }
    });
}

void save_states(const std::string &network_id, const std::map<std::string, json> &states, bool used) {
    if (states.empty()) return;
    const time_t now = time(nullptr);
    modify_entry(network_id, [&](NetworkEntry &e) {
        for (auto &d : e.devices)
            for (auto &t : d.things) {
                const auto it = states.find(key_of(d.id, t.id));
                if (it == states.end()) continue;
                t.state = it->second;
                if (used) d.meta[t.id].used = now;
            }
    });
}

// The state values the model may see: known keys only, short and plain.
json shown_state(const Thing &t, const json &state) {
    static const char *const kKeys[] = {"on", "brightness", "kelvin", "hue", "saturation", "mode", "target_c", "current_c",
                                        "humidity", "position", "fan", "volume", "muted", "playing", "source", "title",
                                        "power_w", "energy_today_wh", "motion", "contact"};
    json out = json::object();
    if (!state.is_object()) return out;
    for (const char *k : kKeys) {
        const auto it = state.find(k);
        if (it == state.end()) continue;
        if (it->is_boolean() || it->is_number()) out[k] = *it;
        else if (it->is_string()) out[k] = clean_name(it->get<std::string>());
    }
    if (t.type == "scene") out.erase("on");
    return out;
}

// One word or a few values for a list of everything.
json brief_state(const Thing &t, const json &state) {
    const json s = shown_state(t, state);
    if (t.caps & kTargetTemp) {
        json h = json::object();
        for (const char *k : {"mode", "target_c", "current_c"})
            if (s.contains(k)) h[k] = s[k];
        return h;
    }
    if (s.contains("on") && s["on"].is_boolean()) return s["on"].get<bool>() ? "on" : "off";
    if (s.contains("position")) return json{{"position", s["position"]}};
    if (s.contains("playing")) return s["playing"];
    if (t.type == "scene") return "scene";
    return "unknown";
}

std::string status(const std::string &network_id, const NetworkEntry &e, const std::vector<Item> &items, const std::string &target,
                   bool dry_run) {
    Resolved r;
    std::string err;
    std::vector<const Item *> members;
    bool everything = util::trim(target).empty();
    if (!everything) {
        if (!resolve(items, target, r, err)) return fail(err);
        everything = r.scope == Scope::Everything;
        members = r.members;
    }
    if (everything) {
        r = Resolved();
        r.scope = Scope::Everything;
        for (const auto &i : items) members.push_back(&i);
        if (members.size() > kStatusMax) members.resize(kStatusMax);
    }
    const bool detail = !everything && members.size() <= kMaxAtOnce;
    std::map<std::string, json> states;
    std::map<std::string, std::string> errors;
    if (dry_run)
        for (const auto *m : members) states[key_of(m->device_id, m->thing.id)] = m->thing.state;
    else {
        read_now(network_id, e, members, detail, states, errors);
        save_states(network_id, states, false);
    }
    auto value = [&](const Item *m) -> json {
        const std::string key = key_of(m->device_id, m->thing.id);
        if (errors.count(key)) return "not reachable";
        return detail ? shown_state(m->thing, states[key]) : brief_state(m->thing, states[key]);
    };
    if (r.scope == Scope::Thing) {
        const Item *m = members[0];
        const std::string key = key_of(m->device_id, m->thing.id);
        if (errors.count(key)) return fail("could not read " + m->label + ": " + errors[key] + ". Tell the user");
        return dump({{"target", m->label}, {"state", value(m)}});
    }
    if (everything) {
        json rooms = json::object();
        for (const auto *m : members) rooms[m->thing.room.empty() ? "no room" : m->thing.room][m->label] = value(m);
        json out = {{"devices", rooms}};
        if (items.size() > members.size()) out["not_listed"] = items.size() - members.size();
        return dump(out);
    }
    json things = json::object();
    for (const auto *m : members) things[m->label] = value(m);
    return dump({{"target", r.label}, {"things", things}});
}

}  // namespace

// ---------------------------------------------------------------- helpers shared with the other files
std::mutex &device_lock(const std::string &device_id) {
    std::lock_guard<std::mutex> lk(g_locks_mtx);
    auto &slot = g_locks[device_id];
    if (!slot) slot = std::make_unique<std::mutex>();
    return *slot;
}

void run_parallel(size_t count, size_t workers, const std::function<void(size_t)> &job) {
    if (count == 0) return;
    std::atomic<size_t> next{0};
    auto work = [&] {
        for (size_t k; (k = next++) < count;) job(k);
    };
    std::vector<std::thread> threads;
    const size_t extra = std::min(count, std::max<size_t>(workers, 1)) - 1;
    for (size_t i = 0; i < extra; ++i) threads.emplace_back(work);
    work();
    for (auto &t : threads) t.join();
}

bool RateLimiter::allow(const std::string &target, unsigned long at_ms, std::string &why_not) {
    while (!all_.empty() && at_ms - all_.front() >= 60000) all_.pop_front();
    auto &mine = per_target_[plain(target)];
    while (!mine.empty() && at_ms - mine.front() >= 10000) mine.pop_front();
    if (all_.size() >= 12) {
        why_not = "too many device commands in the last minute, so this one was not carried out. Tell the user to wait a "
                  "moment and ask again";
        return false;
    }
    if (mine.size() >= 3) {
        why_not = "'" + target + "' was changed several times in the last few seconds, so this was not carried out. Tell "
                  "the user to wait a moment";
        return false;
    }
    all_.push_back(at_ms);
    mine.push_back(at_ms);
    return true;
}

void RateLimiter::reset() {
    all_.clear();
    per_target_.clear();
}

// ---------------------------------------------------------------- the conversation
void user_spoke(const std::string &text) {
    std::lock_guard<std::mutex> lk(g_ctl_mtx);
    ++g_turn;
    g_last_yes = sounds_like_yes(text);
    // A question is answered by the next message or not at all.
    g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(), [](const Pending &p) { return p.turn + 1 < g_turn; }),
                    g_pending.end());
}

void assistant_replied() {
    std::lock_guard<std::mutex> lk(g_ctl_mtx);
    for (auto &p : g_pending)
        if (p.turn == g_turn) p.asked = true;
}

void saw_untrusted_text() {
    std::lock_guard<std::mutex> lk(g_ctl_mtx);
    g_untrusted = true;
    g_untrusted_turn = g_turn;
}

// ---------------------------------------------------------------- commands
std::string execute(const Command &cmd, bool dry_run) {
    const discovery::Network net = network_now();
    if (!net.usable) return fail(net.why_not.empty() ? "there is no home network right now" : net.why_not);
    const NetworkEntry e = entry(net.id());
    const std::vector<Item> items = catalog(e);
    if (items.empty())
        return fail("no smart-home device on this network may be used by the assistant. Tell the user they can set "
                    "devices up in Settings, Smart home");

    std::string action = util::lower(util::trim(cmd.action));
    if (action == "list" || action == "check") action = "status";
    const bool has_value = cmd.brightness || !util::trim(cmd.color).empty() || cmd.temperature || cmd.position || cmd.volume ||
                           !util::trim(cmd.source).empty();
    if (action == "on" && has_value) action = "set";   // "on at 30 percent"
    if (std::find_if(std::begin(kActions), std::end(kActions), [&](const char *a) { return action == a; }) == std::end(kActions))
        return fail("action must be one of on, off, set, status, scene, play, pause, stop, next, previous, mute, unmute");
    if (action == "status") return status(net.id(), e, items, cmd.target, dry_run);
    if (util::trim(cmd.target).empty()) return fail("say which device, room or group to " + action + " (target)");

    Resolved r;
    std::string err;
    if (!resolve(items, cmd.target, r, err)) return fail(err);

    // What is to change, in the fixed units of driver.h.
    Change base;
    if (action == "on" || action == "scene") base.on = true;
    else if (action == "off") base.on = false;
    else if (action == "play" || action == "pause" || action == "stop" || action == "next" || action == "previous") base.transport = action;
    else if (action == "mute" || action == "unmute") base.mute = action == "mute";
    else if (action == "set") {
        if (!has_value) return fail("set needs a value to set, such as brightness, color, temperature, position, volume or source");
        if (cmd.brightness) {
            const int v = std::max(0, std::min(100, *cmd.brightness));
            if (v == 0) base.on = false;
            else { base.brightness = v; base.on = true; }
        }
        if (!util::trim(cmd.color).empty()) {
            color::Named named;
            if (!color::lookup(cmd.color, named))
                return fail("'" + clean_name(cmd.color) + "' is not a colour this knows; use one of the listed colours");
            if (named.kelvin) base.kelvin = named.kelvin;
            else base.hs = std::make_pair(named.hue, named.saturation);
            if (!base.on || *base.on) base.on = true;
        }
        if (cmd.temperature) {
            if (!std::isfinite(*cmd.temperature)) return fail("temperature must be a number of degrees Celsius");
            base.target_c = std::round(std::max(kMinC, std::min(kMaxC, *cmd.temperature)) * 2) / 2;
        }
        if (cmd.position) base.position = std::max(0, std::min(100, *cmd.position));
        if (cmd.volume) base.volume = std::max(0, std::min(100, *cmd.volume));
        if (!util::trim(cmd.source).empty()) {
            const std::string source = clean_name(cmd.source);
            if (source.empty()) return fail("source must be the name of an input or an app");
            base.source = source;
        }
    }

    // Which of the things the name covers this action is for.
    std::vector<const Item *> members;
    auto has = [](const Item *i, uint32_t cap) { return (i->thing.caps & cap) != 0; };
    switch (r.scope) {
        case Scope::Everything:
            // Off is always safe to do in one go; on is not (one of them may feed a heater).
            if (action != "off")
                return fail("everything can be switched off together, but nothing else can be done to everything at once; "
                            "ask which device or room");
            for (const auto *m : r.members) {
                const std::string &type = m->thing.type;
                if (has(m, kPower) && (type == "light" || type == "plug" || type == "switch" || type == "fan" || type == "tv" ||
                                       type == "speaker"))
                    members.push_back(m);
            }
            break;
        case Scope::AllSpeakers:
            if (action != "pause" && action != "stop" && action != "mute")
                return fail("all speakers can only be paused, stopped or muted together; ask which speaker");
            members = r.members;
            break;
        case Scope::Room:
            for (const auto *m : r.members) {
                bool wanted;
                if (base.transport) wanted = has(m, kTransport);
                else if (base.mute || base.volume) wanted = has(m, kVolume);
                else if (base.source) wanted = has(m, kSource);
                else if (base.target_c) wanted = has(m, kTargetTemp);
                else if (base.position) wanted = has(m, kPosition);
                else if (action == "scene") wanted = m->thing.type == "scene";
                else wanted = is_light(m->thing);   // on, off, brightness, colour: the room's lights
                if (wanted) members.push_back(m);
            }
            if (members.empty()) {
                std::vector<std::string> names;
                for (const auto *m : r.members) names.push_back(m->label);
                std::string list;
                for (const auto &n : names) list += (list.empty() ? "" : ", ") + n;
                return fail(std::string(action == "on" || action == "off" ? "a room means its lights, and there are no lights in "
                                                                          : "nothing in ") +
                            r.label + (action == "on" || action == "off" ? "" : " can do that") + ". Ask which of these is meant: " + list);
            }
            if (action == "scene" && members.size() > 1) {
                std::string list;
                for (const auto *m : members) list += (list.empty() ? "" : ", ") + m->label;
                return fail(r.label + " has several scenes: " + list + ". Ask which one");
            }
            break;
        default:
            members = r.members;
            break;
    }
    if (action == "scene")
        for (const auto *m : members)
            if (m->thing.type != "scene") return fail(m->label + " is not a scene. Use on or off for it");

    std::vector<Step> steps;
    std::string why_not;
    for (const auto *m : members) {
        Step s{m, Change()};
        std::string why;
        if (fit(base, m->thing, s.change, why)) steps.push_back(s);
        else if (why_not.empty()) why_not = m->label + " cannot do that: " + why;
    }
    if (steps.empty()) return fail((why_not.empty() ? "nothing there can do that" : why_not) + ". Tell the user");

    // The questions of the design's safety rules, asked in code: the model cannot answer them itself.
    const std::string reason = needs_yes(r, base, steps);
    if (!reason.empty() && !answered_yes(signature(action, steps)))
        return "CONFIRM: nothing has been changed yet, because " + reason + ". Ask the user: \"" +
               capitalised(describe(action, r.label, base)) + "?\" Only if they answer yes, call smart_home again with "
               "the same request.";

    {
        std::lock_guard<std::mutex> lk(g_ctl_mtx);
        if (!g_rate.allow(r.label, now_ms(), err)) return fail(err);
    }

    if (dry_run) {
        for (auto &s : steps) s.ok = true;   // tests check the choice of things without switching anything
    } else {
        run_steps(net.id(), e, steps);
    }
    std::map<std::string, json> states;
    for (const auto &s : steps)
        if (s.ok) {
            Thing t = s.item->thing;
            apply_to_state(s.change, t);
            states[key_of(s.item->device_id, t.id)] = t.state;
        }
    save_states(net.id(), states, true);

    size_t changed = 0;
    json failed = json::array();
    for (const auto &s : steps) {
        if (s.ok) ++changed;
        else failed.push_back(s.item->label + ": " + s.err);
    }
    json now = change_json(base);
    if (base.on && now.size() == 1) now = *base.on ? "on" : "off";
    if (base.transport) { now.erase("transport"); now["playback"] = *base.transport; }
    if (r.scope == Scope::Thing) {
        if (!changed) return fail("could not change " + r.label + ": " + steps[0].err + ". Tell the user");
        return dump({{"target", r.label}, {"now", now}});
    }
    if (!changed) return fail("none of " + r.label + " could be changed: " + failed[0].get<std::string>() + ". Tell the user");
    json out = {{"target", r.label}, {"changed", changed}, {"now", now}};
    if (!failed.empty()) out["failed"] = failed;
    return dump(out);
}

std::string switch_thing(const std::string &network_id, const std::string &device_id, const std::string &thing_id, bool on) {
    std::string why;
    if (!on_network(network_id, why)) return fail(why);
    const NetworkEntry e = entry(network_id);
    const Saved *dev = nullptr;
    for (const auto &d : e.devices)
        if (d.id == device_id) dev = &d;
    const Thing *t = dev ? dev->thing(thing_id) : nullptr;
    if (!t) return fail("That device is no longer saved.");
    Item item;
    item.device_id = device_id;
    item.thing = *t;
    item.label = spoken(*t);
    Change base;
    base.on = on;
    std::vector<Step> steps(1, Step{&item, Change()});
    if (!fit(base, *t, steps[0].change, why)) return fail(item.label + " " + why + ".");
    if (getenv("ZB_DRY_RUN")) steps[0].ok = true;
    else run_steps(network_id, e, steps);
    if (!steps[0].ok) return fail(steps[0].err);
    Thing changed = *t;
    apply_to_state(steps[0].change, changed);
    std::map<std::string, json> states;
    states[key_of(device_id, thing_id)] = changed.state;
    save_states(network_id, states, false);
    return dump({{"target", item.label}, {"now", on ? "on" : "off"}});
}

// ---------------------------------------------------------------- reading what devices offer
std::string refresh(const std::string &network_id, const std::string &only_device) {
    std::string why;
    if (!on_network(network_id, why)) return why;
    const NetworkEntry e = entry(network_id);
    struct Out {
        const Saved *saved = nullptr;
        bool ok = false, setup = false;
        std::string problem;
        Device dev;
        std::vector<Thing> fresh;
    };
    std::vector<Out> outs;
    for (const auto &d : e.devices)
        if ((only_device.empty() || d.id == only_device) && d.controllable()) {
            Out o;
            o.saved = &d;
            outs.push_back(o);
        }
    run_parallel(outs.size(), kWorkers, [&](size_t k) {
        Out &o = outs[k];
        Driver *drv = driver_for(*o.saved);
        if (!drv) return;
        o.dev = *o.saved;
        const PairStep step = drv->next_step(o.dev);
        if (step.kind != Pairing::None) {
            o.setup = true;
            o.problem = step.prompt.empty() ? "It needs setting up first." : step.prompt;
            return;
        }
        std::string err;
        if (!may_contact(o.dev, err)) { o.problem = err; return; }
        std::lock_guard<std::mutex> lk(device_lock(o.dev.id));
        try {
            o.ok = drv->enumerate(o.dev, o.fresh, err);
        } catch (const std::exception &) {
            o.ok = false;
            err = "its answer could not be understood";
        }
        if (!o.ok) o.problem = err.empty() ? "It did not answer." : err;
    });
    const time_t now = time(nullptr);
    modify_entry(network_id, [&](NetworkEntry &en) {
        for (const auto &o : outs)
            for (auto &d : en.devices) {
                if (d.id != o.saved->id) continue;
                d.needs_setup = o.setup;
                d.problem = o.problem;
                if (!o.ok) continue;
                // Only what a read changes is written back, so that settings changed meanwhile stay.
                merge_things(d, o.fresh, now);
                const std::string name = clean_name(o.dev.name);
                if (!name.empty()) d.name = name;
                if (o.dev.auth.is_object()) d.auth = o.dev.auth;
                d.last_seen = now;
            }
    });
    int ready = 0, failed = 0;
    std::string first_problem;
    for (const auto &o : outs) {
        if (o.ok) ++ready;
        else { ++failed; if (first_problem.empty()) first_problem = o.problem; }
    }
    if (outs.empty()) return "Nothing here can be controlled yet.";
    if (failed == 0) return std::to_string(ready) + (ready == 1 ? " device ready." : " devices ready.");
    return std::to_string(ready) + " ready, " + std::to_string(failed) + " not: " + first_problem;
}

// ---------------------------------------------------------------- pairing
namespace {
bool find_saved(const std::string &network_id, const std::string &device_id, Saved &out) {
    for (const auto &d : entry(network_id).devices)
        if (d.id == device_id) { out = d; return true; }
    return false;
}
}  // namespace

PairStep pairing_step(const std::string &network_id, const std::string &device_id) {
    std::string why;
    Saved dev;
    if (!on_network(network_id, why) || !find_saved(network_id, device_id, dev)) return {};
    Driver *drv = driver_for(dev);
    if (!drv) return {};
    try {
        return drv->next_step(dev);
    } catch (const std::exception &) {
        return {};
    }
}

bool pairing_submit(const std::string &network_id, const std::string &device_id, const std::string &input, std::string &err) {
    Saved saved;
    if (!on_network(network_id, err)) return false;
    if (!find_saved(network_id, device_id, saved)) { err = "That device is no longer saved."; return false; }
    Driver *drv = driver_for(saved);
    if (!drv) { err = "This kind of device cannot be set up here."; return false; }
    if (!may_contact(saved, err)) return false;
    Device dev = saved;
    bool ok = false;
    PairStep next;
    {
        std::lock_guard<std::mutex> lk(device_lock(dev.id));
        try {
            ok = drv->pair(dev, input, err);
            next = drv->next_step(dev);
        } catch (const std::exception &) {
            ok = false;
            err = "The device's answer could not be understood.";
        }
    }
    if (!ok && err.empty()) err = "That did not work.";
    modify_entry(network_id, [&](NetworkEntry &e) {
        for (auto &d : e.devices)
            if (d.id == device_id) {
                d.auth = dev.auth.is_object() ? dev.auth : json::object();
                d.needs_setup = next.kind != Pairing::None;
                if (!d.needs_setup && ok) d.problem.clear();
            }
    });
    return ok;
}

void unpair(const std::string &network_id, const std::string &device_id) {
    std::string why;
    Saved saved;
    if (!on_network(network_id, why) || !find_saved(network_id, device_id, saved)) return;
    Driver *drv = driver_for(saved);
    if (!drv) return;
    Device dev = saved;
    PairStep next;
    {
        std::lock_guard<std::mutex> lk(device_lock(dev.id));
        try {
            if (may_contact(dev, why)) drv->unpair(dev);
            dev.auth = json::object();   // whatever the device said, nothing of the pairing is kept
            next = drv->next_step(dev);
        } catch (const std::exception &) {
            dev.auth = json::object();
        }
    }
    modify_entry(network_id, [&](NetworkEntry &e) {
        for (auto &d : e.devices)
            if (d.id == device_id) {
                d.auth = json::object();
                d.needs_setup = next.kind != Pairing::None;
            }
    });
}

// ---------------------------------------------------------------- for the device-free tests
namespace testing {
void reset_control() {
    std::lock_guard<std::mutex> lk(g_ctl_mtx);
    g_turn = 0;
    g_last_yes = false;
    g_untrusted = false;
    g_untrusted_turn = 0;
    g_pending.clear();
    g_rate.reset();
}
}  // namespace testing

}  // namespace home
