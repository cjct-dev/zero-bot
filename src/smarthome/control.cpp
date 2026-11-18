// Carrying things out: the commands the assistant gives, with the safety rules of the design
// (questions before risky changes, rate limits, nothing on a network the user has not scanned)
// enforced here in code rather than left to the model; reading state; refreshing what devices
// offer; pairing. Each device is talked to by one thread at a time, and a command for several
// devices fans out over at most four threads, all joined before the command returns.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <system_error>
#include <thread>

#include "smarthome/color.h"
#include "smarthome/internal.h"

namespace home {

namespace {

constexpr int kApplyDeadlineMs = 2500;      // per device, for a change
constexpr int kStalledMs = kApplyDeadlineMs / 2;   // a change that took this long and had no answer: the device has stalled
constexpr int kReadDeadlineMs = 4000;       // per device, for reading its things
constexpr size_t kWorkers = 4;              // threads a command for several devices may use
constexpr size_t kMaxAtOnce = 8;            // more things than this in one command need a yes
constexpr size_t kStatusMax = 30;           // things listed by a status of everything
constexpr unsigned long kAnswerWithinMs = 3 * 60 * 1000;   // a yes must come within this
constexpr double kMinC = 5, kMaxC = 30;     // heating targets are clamped to this range

const char *const kActions[] = {"on",   "off",      "set",  "status", "scene",  "play",
                                "pause", "stop", "next", "previous", "mute", "unmute", "press"};

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
// How a reply or an answer may refer to what a question is about. A reply names it when, for one
// of `ways`, it has a word from every set: {"heater"} for a "Fan heater" when no other thing's
// name has that word, {"heating"} for the only thermostat, {all, every, ...} and {light, lights,
// ...} for all the lights, or every word of the name as listed. An answer may use any of `words`
// besides agreeing ("yes, the heating", "yes, turn everything off").
struct Naming {
    std::vector<std::vector<std::set<std::string>>> ways;
    std::set<std::string> words;
};

// A change that needed the user's yes. It goes ahead only when the identical change is asked for
// again in the very next user message, that message reads as a yes, and a reply that put the
// question (a question naming the thing) went to the user in between. Nothing the model passes
// can stand in for this.
struct Pending {
    std::string signature;
    std::string reason;          // why it needs a yes; kept for as long as the question is open
    std::string question;        // what the user is to be asked, e.g. "Turn on Bed heater?"
    std::string action;          // on, off, set, ...
    Naming naming;               // how a reply that asks, and an answer, refer to the target
    unsigned long at_ms = 0;
    unsigned long turn = 0;      // the user message during which it was asked for
    bool asked = false;          // a reply that put the question went to the user after it
};

std::mutex g_ctl_mtx;            // guards everything in this group
unsigned long g_turn = 0;        // user messages so far
std::string g_last_said;         // the newest user message
// Text from outside (a web page, what a photo shows) the model has read. It stays in what the
// model sees until the conversation is cleared or grows past it, and could be steering the model
// for all that time, so every change needs the user's yes until then: not only in the turn it was
// read in, and not only a change already put as a question.
bool g_untrusted = false;             // read since the conversation was last cleared ...
unsigned long g_untrusted_turn = 0;   // ... most recently during this user message
unsigned long g_context_from = 0;     // the oldest user message the model still sees
std::vector<Pending> g_pending;
constexpr size_t kMaxPending = 8;
RateLimiter g_rate;

std::vector<std::string> words_of(const std::string &text) {
    std::vector<std::string> out;
    for (const auto &w : util::split(plain(text), ' '))
        if (!w.empty()) out.push_back(w);
    return out;
}

// Whether text from outside is still in what the model sees. The caller holds g_ctl_mtx.
bool steered_by_outside() { return g_untrusted && g_untrusted_turn >= g_context_from; }

// Words a question or an answer uses for a group, or for a kind of thing ("the TV", "the
// heating"), in English and the languages the yes words below cover.
const std::set<std::string> kAllWords = {"all", "every", "everything", "alle", "alles", "tout", "tous", "toutes", "todo",
                                         "todos", "todas", "tutto", "tutti", "tutte", "allemaal"};
const std::set<std::string> kLightWords = {"light", "lights", "lamp", "lamps", "licht", "lichter", "lampe", "lampen",
                                           "lumière", "lumières", "luz", "luces", "luzes", "luce", "luci", "lampade"};
const std::set<std::string> kSpeakerWords = {"speaker", "speakers", "lautsprecher", "enceinte", "enceintes", "altavoz",
                                             "altavoces", "altoparlante", "altoparlanti", "luidspreker", "luidsprekers"};
const std::set<std::string> kDeviceWords = {"device", "devices", "thing", "things", "geräte", "appareils", "dispositivos",
                                            "dispositivi", "apparaten"};
// Words of a name that say nothing about which thing it is.
const std::set<std::string> kNotNaming = {"the", "a", "an", "of", "and", "in", "on", "off", "at", "to", "my", "our", "your",
                                          "for", "with", "by", "room", "s"};

std::set<std::string> kind_words(const std::string &type) {
    if (type == "light") return kLightWords;
    if (type == "speaker") return kSpeakerWords;
    if (type == "plug") return {"plug", "socket"};
    if (type == "thermostat")
        return {"heating", "thermostat", "temperature", "heizung", "chauffage", "calefacción", "calefaccion", "riscaldamento",
                "aquecimento", "verwarming"};
    if (type == "cover") return {"blind", "blinds", "curtain", "curtains", "shade", "shades", "shutter", "shutters"};
    if (type == "fan") return {"fan"};
    if (type == "tv") return {"tv", "television", "telly", "fernseher", "télé", "tele", "televisor", "televisión", "televisione"};
    if (type == "scene") return {"scene"};
    return {};
}

bool is_number(const std::string &w) {
    return !w.empty() && std::all_of(w.begin(), w.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// The words of a name that say which thing or room it is ("snug" of "The Snug"); all of them when
// none does.
std::vector<std::string> naming_words(const std::vector<std::string> &name) {
    std::vector<std::string> out;
    for (const auto &w : name)
        if (!kNotNaming.count(w)) out.push_back(w);
    return out.empty() ? name : out;
}

// How the question about a change to `r` may name it. A thing is named by every word of its
// name, or by one word that names nothing else listed: a word of its name no other thing's name
// has ("the heater" for "Fan heater"), or the word for its kind when it is the only one of that
// kind ("the TV", "the heating"). A room by every word of its name ("the snug lights" for The
// Snug); a group by a word for all and, for the lights or speakers, a word for those.
Naming naming_of(const std::vector<Item> &items, const Resolved &r, const std::vector<const Thing *> &changed) {
    Naming n;
    auto every_word = [](const std::vector<std::string> &words) {
        std::vector<std::set<std::string>> way;
        for (const auto &w : words) way.push_back({w});
        return way;
    };
    for (const Thing *t : changed)
        for (const auto &w : kind_words(t->type)) n.words.insert(w);
    switch (r.scope) {
        case Scope::Thing: {
            const Item *me = r.members[0];
            const std::vector<std::string> name = words_of(me->label);
            const std::vector<std::string> naming = naming_words(name);
            n.ways.push_back(every_word(naming));
            std::set<std::string> elsewhere;   // every word of the other names listed
            bool alone = true;                 // the only one of its kind
            for (const auto &i : items) {
                if (&i == me) continue;
                for (const auto &w : words_of(i.label)) elsewhere.insert(w);
                alone = alone && i.thing.type != me->thing.type;
            }
            std::set<std::string> own;   // never a number ("Socket 2") or a word like "the" or "on"
            for (const auto &w : naming)
                if (!elsewhere.count(w) && !is_number(w) && !kNotNaming.count(w)) own.insert(w);
            if (alone)
                for (const auto &w : kind_words(me->thing.type))
                    if (!elsewhere.count(w)) own.insert(w);
            if (!own.empty()) n.ways.push_back({own});
            for (const auto &w : name) n.words.insert(w);
            break;
        }
        case Scope::Room:
            n.ways.push_back(every_word(naming_words(words_of(r.label))));
            for (const auto &w : words_of(r.label)) n.words.insert(w);
            break;
        case Scope::AllLights:
            n.ways.push_back({kAllWords, kLightWords});
            n.words.insert(kAllWords.begin(), kAllWords.end());
            n.words.insert(kLightWords.begin(), kLightWords.end());
            break;
        case Scope::AllSpeakers:
            n.ways.push_back({kAllWords, kSpeakerWords});
            n.words.insert(kAllWords.begin(), kAllWords.end());
            n.words.insert(kSpeakerWords.begin(), kSpeakerWords.end());
            break;
        case Scope::Everything:
            n.ways.push_back({kAllWords});
            n.words.insert(kAllWords.begin(), kAllWords.end());
            n.words.insert(kDeviceWords.begin(), kDeviceWords.end());
            break;
        default:
            break;
    }
    return n;
}

// A short answer that agrees and says nothing else: "yes", "yes please", "sure, go ahead", "yes,
// the bed heater". Every word has to belong to an agreement, to the politeness around one, or to
// what the question was about (`names`: the thing's name, the word for its kind, a group's words);
// a message that also says anything else is a new request ("please set a timer", "ok, what time
// is it"), and one with a word of refusal in it ("leave it off please", "yes, no wait"), or of the
// opposite of what was asked ("off" to a question about switching on), is not a yes. An answer
// that is not recognised only means the question is asked again.
bool sounds_like_yes(const std::string &text, const std::set<std::string> &names, const std::string &action) {
    const std::vector<std::string> words = words_of(text);
    if (words.empty() || words.size() > 10) return false;
    static const std::set<std::string> kRefusal = {
        "no", "nope", "nah", "not", "don", "dont", "never", "cancel", "stop", "wait", "later", "leave", "keep", "hold", "maybe",
        "nein", "nicht", "non", "pas", "não", "nao", "nee", "niet", "nie", "ne", "nem", "nunca", "jamais", "nada"};
    const bool switching_off = action == "off";
    for (const auto &w : words)
        if (kRefusal.count(w) || w == (switching_off ? "on" : "off")) return false;
    // Words that agree, alone or together; "please" answers a question as a yes only when it is
    // all that was said.
    static const char *const kAgreeing[] = {"go ahead", "go on", "go for it", "do it", "do that", "please do", "of course",
                                            "that s right", "that s fine", "that s ok", "that s okay", "all right", "sounds good",
                                            "sure thing", "you bet", "uh huh", "mm hmm", "d accord", "bien sûr", "bien sur",
                                            "vas y", "por supuesto", "mach das", "mach es", "na klar", "va bene", "pode ser",
                                            "isso mesmo"};
    static const char *const kPlease[] = {"por favor", "s il vous plaît", "s il vous plait", "s il te plaît", "s il te plait"};
    static const std::set<std::string> kYes = {"yes", "yeah", "yea", "yep", "yup", "aye", "sure", "ok", "okay", "okey", "alright",
                                               "fine", "affirmative", "absolutely", "definitely", "certainly", "correct", "confirm",
                                               "confirmed", "mhm", "si", "sí", "oui", "ja", "jawohl", "genau", "klar", "sim",
                                               "tak", "da", "vale", "claro", "certo", "dale"};
    // Allowed around an agreement, never one by themselves ("right" also gives directions).
    static const std::set<std::string> kAround = {"thanks", "thank", "you", "then", "just", "oh", "um", "uh", "er", "erm", "ah",
                                                  "well", "now", "right", "mate", "bitte", "danke", "merci", "gracias", "obrigado",
                                                  "obrigada", "grazie", "the", "it", "them", "that", "one", "all", "of", "turn",
                                                  "switch", "set", "on", "off"};
    bool agreed = false, please = false, other = false;
    std::string joined = " ";
    for (const auto &w : words) joined += w + " ";
    auto take = [&](const char *phrase) {
        bool found = false;
        for (size_t at; (at = joined.find(" " + std::string(phrase) + " ")) != std::string::npos; found = true)
            joined.replace(at, strlen(phrase) + 2, " ");
        return found;
    };
    for (const char *phrase : kAgreeing) agreed = take(phrase) || agreed;
    for (const char *phrase : {"turn it", "switch it", "turn them", "switch them"})
        agreed = take((std::string(phrase) + (switching_off ? " off" : " on")).c_str()) || agreed;
    for (const char *phrase : kPlease) please = take(phrase) || please;
    for (const auto &w : util::split(joined, ' ')) {
        if (w.empty()) continue;
        if (kYes.count(w)) agreed = true;
        else if (w == "please") please = true;
        else if (kAround.count(w) || names.count(w)) other = true;
        else return false;   // it says something else as well
    }
    return agreed || (please && !other);
}

// Whether a reply put the question: it asks something, and it names what the question is about
// (see Naming).
bool puts_question(const std::string &reply, const Naming &naming) {
    if (reply.find_first_of("?") == std::string::npos && reply.find("？") == std::string::npos &&
        reply.find("؟") == std::string::npos)
        return false;
    const std::vector<std::string> words = words_of(reply);
    const std::set<std::string> said(words.begin(), words.end());
    return std::any_of(naming.ways.begin(), naming.ways.end(), [&](const std::vector<std::set<std::string>> &way) {
        return !way.empty() && std::all_of(way.begin(), way.end(), [&](const std::set<std::string> &choice) {
            return std::any_of(choice.begin(), choice.end(), [&](const std::string &w) { return said.count(w) > 0; });
        });
    });
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
        // Recorded only for a thing whose driver reports what is playing: on a TV whose play and
        // pause are one toggle key (a Fire TV), "play" says nothing about what happens.
        const std::string tr = t.state.contains("playing") ? j["transport"].get<std::string>() : "";
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
                             !c.volume && !c.volume_step && !c.mute && !c.transport && !c.source && !c.key;
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
    if (c.volume && !(caps & kVolume)) {
        why = caps & kVolumeStep ? "its volume can only be turned up or down (use volume_step)" : "its volume cannot be set to a level";
        return false;
    }
    if (c.volume_step && !(caps & kVolumeStep)) {
        why = caps & kVolume ? "its volume is set as a level, 0 to 100 (use volume)" : "its volume cannot be changed";
        return false;
    }
    if (c.key && !(caps & kKeys)) { why = "it has no remote-control keys"; return false; }
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
    if (action == "press" && c.key) return "press " + *c.key + " on " + label;
    std::vector<std::string> parts;
    if (c.brightness) parts.push_back("brightness " + std::to_string(*c.brightness) + "%");
    if (c.kelvin || c.hs) parts.push_back("the colour asked for");
    if (c.target_c) parts.push_back(number_text(*c.target_c) + " degrees");
    if (c.position) parts.push_back("position " + std::to_string(*c.position) + "%");
    if (c.volume) parts.push_back("volume " + std::to_string(*c.volume));
    if (c.source) parts.push_back(*c.source);
    if (c.volume_step && parts.empty())
        return "turn " + label + (*c.volume_step > 0 ? " up " : " down ") + std::to_string(std::abs(*c.volume_step)) +
               (std::abs(*c.volume_step) == 1 ? " step" : " steps");
    if (parts.empty()) return std::string(c.on && !*c.on ? "turn off " : "change ") + label;
    std::string what;
    for (const auto &p : parts) what += (what.empty() ? "" : ", ") + p;
    return "set " + label + " to " + what;
}

std::string capitalised(std::string s) {
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

// What a question calls the target, as a person would: "Bed heater", "the lights in Hall", "all
// 10 lights", "all 12 devices" (never "all", which reads as nothing when spoken).
std::string target_phrase(const Resolved &r, const std::vector<Step> &steps) {
    const std::string n = std::to_string(steps.size());
    auto all_are = [&](const char *type) {
        return std::all_of(steps.begin(), steps.end(), [&](const Step &s) { return s.item->thing.type == type; });
    };
    switch (r.scope) {
        case Scope::Everything: return steps.size() > 1 ? "all " + n + " devices" : "everything";
        case Scope::AllLights: return steps.size() > 1 ? "all " + n + " lights" : "all the lights";
        case Scope::AllSpeakers: return steps.size() > 1 ? "all " + n + " speakers" : "all the speakers";
        case Scope::Room:
            if (all_are("light")) return "the lights in " + r.label;
            if (all_are("cover")) return "the blinds in " + r.label;
            if (all_are("speaker")) return "the speakers in " + r.label;
            return r.label;
        default: return r.label;
    }
}

// Why this plan needs the user's yes first, or "" when it does not.
std::string needs_yes(const Resolved &r, const Change &base, const std::vector<Step> &steps) {
    for (const auto &s : steps)
        if (s.change.on && *s.change.on && s.item->thing.ask_first)
            return in_quotes(s.item->label) + " is set to be switched on only after the user says yes";
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
    if (steered_by_outside())
        return g_untrusted_turn == g_turn
                   ? "this request involved text from outside (a web page or a photo), and devices change only at the user's own word"
                   : "this conversation includes text from outside (a web page or a photo), and devices change only at the user's "
                     "own word";
    return "";
}

// What identifies a change for its question: the action, the target as resolved (so "all" and
// "all lights" are different requests even where they cover the same lights) and every change to
// every thing.
std::string signature(const std::string &action, const std::string &target, const std::vector<Step> &steps) {
    std::vector<std::string> parts;
    for (const auto &s : steps) parts.push_back(s.item->device_id + "\n" + s.item->thing.id + "\n" + change_json(s.change).dump());
    std::sort(parts.begin(), parts.end());
    std::string sig = action + "\n" + target;
    for (const auto &p : parts) sig += "\n\n" + p;
    return sig;
}

// The questions of the design's safety rules, asked in code. Returns "" when the change may go
// ahead: it needs no yes (`reason` is empty and it was not put to the user as a question), or it
// was put to the user as a question in the reply to their previous message and they have just
// said yes. Otherwise the change is remembered and the CONFIRM text for the model is returned.
// A change once put as a question keeps needing the yes in the next turn even when what made it
// risky has passed (ask_first was switched off in settings meanwhile), so an answer of "no"
// stops it. (Text from outside keeps every change waiting for a yes by itself: needs_yes().)
std::string question_first(const std::string &sig, std::string reason, const std::string &action, const Naming &naming,
                           const std::string &question) {
    std::lock_guard<std::mutex> lk(g_ctl_mtx);
    const unsigned long now = now_ms();
    bool not_agreed = false;
    for (auto it = g_pending.begin(); it != g_pending.end(); ++it) {
        if (it->signature != sig) continue;
        // Within one turn the rules give the current answer (the user may have just switched the
        // question off in settings); a question from the turn before stays open whatever they say.
        if (reason.empty() && it->turn < g_turn) reason = it->reason;
        const bool answered = it->asked && g_turn == it->turn + 1;
        const bool ok = answered && sounds_like_yes(g_last_said, it->naming.words, it->action) && now - it->at_ms <= kAnswerWithinMs;
        not_agreed = answered && !ok;
        g_pending.erase(it);
        if (ok) return "";
        break;
    }
    if (reason.empty()) return "";
    Pending p;
    p.signature = sig;
    p.reason = reason;
    p.question = question;
    p.action = action;
    p.naming = naming;
    p.at_ms = now;
    p.turn = g_turn;
    g_pending.push_back(p);
    if (g_pending.size() > kMaxPending) g_pending.erase(g_pending.begin());
    if (not_agreed)
        return "CONFIRM: nothing has been changed, because the user's answer was not a clear yes. If they still want it, ask: \"" +
               question + "\" Only if they answer yes, call smart_home again with the same request.";
    return "CONFIRM: nothing has been changed yet, because " + reason + ". Ask the user: \"" + question +
           "\" Only if they answer yes, call smart_home again with the same request.";
}

// ---------------------------------------------------------------- talking to devices
// A device whose saved address no longer answers is looked for again by asking the network the
// way the user's own scan does (on a network they opted in, never on the stand-in test network),
// and recognised by its identifier, never by going through addresses. One scan serves every device
// that needs it for 20 seconds. Only addresses change: the saved devices that answered from a new
// address (or port) are moved there; nothing is added and nothing else of theirs is replaced, since
// a short scan in the middle of a command is not the user's scan from Settings, which alone adds
// devices.
enum class Lookup { NotTried, NotFound, StillThere, Moved };
std::mutex g_lookup_mtx;
std::vector<discovery::Device> g_found;
std::string g_found_on;
unsigned long g_found_at = 0;
bool (*g_test_scan)(std::vector<discovery::Device> &found) = nullptr;

// A port counts only when both are known: a saved port of 0 means the family's usual one.
bool moved(const discovery::Device &f, const Device &d) {
    return f.id == d.id && !f.ip.empty() && (f.ip != d.ip || (f.port > 0 && d.port > 0 && f.port != d.port));
}

Lookup relocate(const std::string &network_id, Saved &device) {
    std::lock_guard<std::mutex> lk(g_lookup_mtx);
    if (!g_test_scan && test_network()) return Lookup::NotTried;
    const discovery::Network net = network_now(true);
    if (!net.usable || net.id() != network_id) return Lookup::NotTried;
    if (g_found_on != network_id || g_found_at == 0 || now_ms() - g_found_at > 20000) {
        std::vector<discovery::Device> fresh;
        std::string err;
        discovery::Options opt;
        opt.listen_ms = 2500;
        if (!(g_test_scan ? g_test_scan(fresh) : discovery::scan(opt, fresh, err))) return Lookup::NotTried;
        g_found = fresh;
        g_found_on = network_id;
        g_found_at = now_ms() ? now_ms() : 1;
        modify_entry(network_id, [&](NetworkEntry &e) {
            for (auto &d : e.devices)
                for (const auto &f : g_found)
                    if (moved(f, d)) {
                        d.ip = f.ip;
                        if (f.port > 0) d.port = f.port;
                    }
        });
    }
    for (const auto &f : g_found)
        if (f.id == device.id) {
            if (!moved(f, device)) return Lookup::StillThere;   // it answers the scan where it was, but not the command
            device.ip = f.ip;
            if (f.port > 0) device.port = f.port;
            return Lookup::Moved;
        }
    return Lookup::NotFound;
}

// Whether a driver's error says the device did answer, only not as hoped ("answered with error
// code -1501", "the plug's answer could not be read"). unreachable() lets these through too, since
// another device may have taken the address, but such a device is not "busy or starting up".
bool answered(const std::string &err) {
    const std::string e = util::lower(err);
    return e.find("answered") != std::string::npos || e.find("'s answer") != std::string::npos ||
           e.find("its answer") != std::string::npos;
}

// Added to "it did not answer" (`err`) once the device has been looked for on the network.
const char *after_lookup(Lookup where, const std::string &err) {
    if (where == Lookup::NotFound)
        return "; it was not found at another address on the network either (it may be switched off at the wall, or out of "
               "Wi-Fi range)";
    if (where == Lookup::StillThere && !answered(err))
        return "; it still announces itself on the network at the same address, so it may be busy or starting up";
    return "";
}

// A change that comes out the same however often it is made (on, 40 %, an input) may be sent a
// second time; one that moves on from where things are (volume steps, a key press, the next track)
// never is by itself: had the first reached the device after all, it would happen twice. Nor is
// any playback control: some TVs have a single play/pause toggle, so a pause sent twice plays again.
bool repeatable(const Change &c) {
    return !c.volume_step && !c.key && !c.transport;
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

// Whether a driver may be called for a device of `network_id` at this moment, asked again before
// every call: a command that changes or reads several devices takes seconds, and the connection
// can move meanwhile (Wi-Fi drops and the phone's tether takes over). `why` says why not.
bool still_on(const std::string &network_id, std::string &why) {
    why.clear();
    return on_network(network_id, why);
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
        if (!still_on(network_id, why)) {
            for (auto *s : todo) s->err = why;
            return;
        }
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
        if (!handled) {
            // A device that spent most of its time on a change and then did not answer is not asked
            // again for each of its other things in this command: a stalled four-relay device cost
            // 2.5 s a relay, and only then was it looked for. A quick "did not answer" (a refused
            // connection, a bridge reporting one lamp out of reach) is asked thing by thing as before.
            std::string stalled;
            for (size_t k = 0; k < todo.size(); ++k) {
                if (k > 0 && !still_on(network_id, why)) {
                    for (size_t j = k; j < todo.size(); ++j) todo[j]->err = why;
                    return;
                }
                if (!stalled.empty()) { todo[k]->err = stalled; continue; }
                const auto started = std::chrono::steady_clock::now();
                todo[k]->ok = call_apply(drv, dev, todo[k]->item->thing, todo[k]->change, todo[k]->err);
                const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
                if (!todo[k]->ok && took.count() >= kStalledMs && unreachable(todo[k]->err)) stalled = todo[k]->err;
            }
        }
        // No answer at the saved address: the router may have given the device a new one. Sent
        // there once more, and only when the device has turned up at another address, so the first
        // cannot have reached it; and only a change that comes out the same if it did after all.
        const bool none_ok = std::none_of(todo.begin(), todo.end(), [](const Step *s) { return s->ok; });
        if (attempt > 0 || !none_ok || !unreachable(todo[0]->err)) return;
        const Lookup where = relocate(network_id, dev);   // saves the new address itself
        for (auto *s : todo) s->err += after_lookup(where, s->err);
        if (where != Lookup::Moved) return;
        if (!std::all_of(todo.begin(), todo.end(), [](const Step *s) { return repeatable(s->change); })) {
            for (auto *s : todo)
                s->err = "it had moved to a new address on the network, which is now saved, and the change was not sent "
                         "there: a volume step, key press or playback control is never sent a second time by itself. Nothing was "
                         "changed; ask the user whether to do it now";
            return;
        }
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
                if (!still_on(network_id, err)) break;
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
                // Re-discovery is worth its wait only for things asked about by name. Reading
                // changes nothing, so it is always asked again at a new address.
                if (ok || attempt > 0 || !detail || !unreachable(err)) break;
                const Lookup where = relocate(network_id, dev);
                err += after_lookup(where, err);
                if (where != Lookup::Moved) break;
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
        if (errors.count(key)) return "could not be read: " + errors[key];
        return detail ? shown_state(m->thing, states[key]) : brief_state(m->thing, states[key]);
    };
    if (r.scope == Scope::Thing) {
        const Item *m = members[0];
        const std::string key = key_of(m->device_id, m->thing.id);
        if (errors.count(key)) return fail("could not read " + in_quotes(m->label) + ": " + errors[key] + ". Tell the user");
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
        // A job that throws (a driver that broke its promise not to) must not take the app with
        // it; what it did not finish stays reported as failed.
        for (size_t k; (k = next++) < count;) {
            try {
                job(k);
            } catch (...) {
            }
        }
    };
    // Every thread started is joined, whatever happens. One that cannot be started (the system is
    // short of threads or memory) only means fewer helpers: this thread does the rest itself.
    std::vector<std::thread> threads;
    struct Joiner {
        std::vector<std::thread> &threads;
        ~Joiner() {
            for (auto &t : threads)
                if (t.joinable()) t.join();
        }
    } joiner{threads};
    const size_t extra = std::min(count, std::max<size_t>(workers, 1)) - 1;
    try {
        threads.reserve(extra);
        for (size_t i = 0; i < extra; ++i) threads.emplace_back(work);
    } catch (...) {
    }
    work();
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
        why_not = in_quotes(target) + " was changed several times in the last few seconds, so this was not carried out. Tell "
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
    g_last_said = text;
    // A question is answered by the next message or not at all.
    g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(), [](const Pending &p) { return p.turn + 1 < g_turn; }),
                    g_pending.end());
}

void assistant_replied(const std::string &reply) {
    std::lock_guard<std::mutex> lk(g_ctl_mtx);
    // Only a reply that puts a change's question counts as asking it: "Done. Anything else?" or
    // an empty answer leaves it unasked, and asking about one change does not ask about another.
    for (auto &p : g_pending)
        if (p.turn == g_turn && puts_question(reply, p.naming)) p.asked = true;
}

void saw_untrusted_text() {
    std::lock_guard<std::mutex> lk(g_ctl_mtx);
    g_untrusted = true;
    g_untrusted_turn = g_turn;
}

void conversation_reset() {
    std::lock_guard<std::mutex> lk(g_ctl_mtx);
    g_untrusted = false;
    g_pending.clear();   // a yes in the new conversation answers nothing asked in the old one
}

void context_kept(size_t user_messages) {
    std::lock_guard<std::mutex> lk(g_ctl_mtx);
    // The messages kept are the newest ones: the oldest of them is this user message.
    if (user_messages <= g_turn) g_context_from = std::max(g_context_from, g_turn + 1 - user_messages);
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
                           cmd.volume_step || !util::trim(cmd.source).empty();
    if (action == "on" && has_value) action = "set";   // "on at 30 percent"
    if (std::find_if(std::begin(kActions), std::end(kActions), [&](const char *a) { return action == a; }) == std::end(kActions))
        return fail("action must be one of on, off, set, status, scene, play, pause, stop, next, previous, mute, unmute, press");
    if (action == "status") return status(net.id(), e, items, cmd.target, dry_run);
    if (util::trim(cmd.target).empty()) {
        const std::string verb = action == "on" || action == "off" ? "switch " + action : action == "set" ? "change" : action;
        return fail("say which device, room or group to " + verb + " (target)");
    }

    Resolved r;
    std::string err;
    if (!resolve(items, cmd.target, r, err)) return fail(err);

    // What is to change, in the fixed units of driver.h.
    Change base;
    if (action == "on" || action == "scene") base.on = true;
    else if (action == "off") base.on = false;
    else if (action == "play" || action == "pause" || action == "stop" || action == "next" || action == "previous") base.transport = action;
    else if (action == "mute" || action == "unmute") base.mute = action == "mute";
    else if (action == "press") {
        std::string k = util::lower(util::trim(cmd.key));
        std::replace(k.begin(), k.end(), ' ', '_');
        if (k == "select" || k == "enter") k = "ok";
        if (std::find_if(std::begin(kTvKeys), std::end(kTvKeys), [&](const char *n) { return k == n; }) == std::end(kTvKeys))
            return fail(k.empty() ? "press needs the key to press (key)" : "there is no remote key called " + in_quotes(clean_name(cmd.key)) +
                                                                                   "; use one of the listed keys");
        base.key = k;
    } else if (action == "set") {
        if (!has_value) return fail("set needs a value to set, such as brightness, color, temperature, position, volume or source");
        if (cmd.brightness) {
            const int v = std::max(0, std::min(100, *cmd.brightness));
            if (v == 0) base.on = false;
            else { base.brightness = v; base.on = true; }
        }
        if (!util::trim(cmd.color).empty()) {
            color::Named named;
            if (!color::lookup(cmd.color, named))
                return fail(in_quotes(clean_name(cmd.color)) + " is not a colour this knows; use one of the listed colours");
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
        if (cmd.volume_step) {
            if (*cmd.volume_step == 0) return fail("volume_step must be a number of presses, such as 2 or -2");
            base.volume_step = std::max(-10, std::min(10, *cmd.volume_step));
        }
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
                else if (base.volume_step) wanted = has(m, kVolumeStep);
                else if (base.key) wanted = has(m, kKeys);
                else if (base.source) wanted = has(m, kSource);
                else if (base.target_c) wanted = has(m, kTargetTemp);
                else if (base.position) wanted = has(m, kPosition);
                else if (action == "scene") wanted = m->thing.type == "scene";
                else wanted = is_light(m->thing);   // on, off, brightness, colour: the room's lights
                if (wanted) members.push_back(m);
            }
            if (members.empty()) {
                std::string list;
                for (const auto *m : r.members) list += (list.empty() ? "" : ", ") + in_quotes(m->label);
                return fail(std::string(action == "on" || action == "off" ? "a room means its lights, and there are no lights in "
                                                                          : "nothing in ") +
                            in_quotes(r.label) + (action == "on" || action == "off" ? "" : " can do that") +
                            ". Ask which of these is meant: " + list);
            }
            if (action == "scene" && members.size() > 1) {
                std::string list;
                for (const auto *m : members) list += (list.empty() ? "" : ", ") + in_quotes(m->label);
                return fail(in_quotes(r.label) + " has several scenes: " + list + ". Ask which one");
            }
            break;
        default:
            members = r.members;
            break;
    }
    if (action == "scene")
        for (const auto *m : members)
            if (m->thing.type != "scene") return fail(in_quotes(m->label) + " is not a scene. Use on or off for it");

    std::vector<Step> steps;
    std::string why_not;
    for (const auto *m : members) {
        Step s{m, Change()};
        std::string why;
        if (fit(base, m->thing, s.change, why)) steps.push_back(s);
        else if (why_not.empty()) why_not = in_quotes(m->label) + " cannot do that: " + why;
    }
    if (steps.empty()) return fail((why_not.empty() ? "nothing there can do that" : why_not) + ". Tell the user");

    // The questions of the design's safety rules, asked in code: the model cannot answer them itself.
    std::vector<const Thing *> to_change;
    for (const auto &s : steps) to_change.push_back(&s.item->thing);
    const std::string confirm = question_first(signature(action, r.label, steps), needs_yes(r, base, steps), action,
                                               naming_of(items, r, to_change),
                                               capitalised(describe(action, target_phrase(r, steps), base)) + "?");
    if (!confirm.empty()) return confirm;

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
    const Step *first_failure = nullptr;
    for (const auto &s : steps) {
        if (s.ok) { ++changed; continue; }
        failed.push_back(s.item->label + ": " + s.err);
        if (!first_failure) first_failure = &s;
    }
    json now = change_json(base);
    if (base.on && now.size() == 1) now = *base.on ? "on" : "off";
    if (base.transport) { now.erase("transport"); now["playback"] = *base.transport; }
    if (r.scope == Scope::Thing) {
        if (!changed) return fail("could not change " + in_quotes(r.label) + ": " + steps[0].err + ". Tell the user");
        return dump({{"target", r.label}, {"now", now}});
    }
    if (!changed)
        return fail("none of " + in_quotes(r.label) + " could be changed: " + in_quotes(first_failure->item->label) + ": " +
                    first_failure->err + ". Tell the user");
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
    if (!fit(base, *t, steps[0].change, why)) return fail(capitalised(why) + ".");
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
namespace {

bool find_saved(const std::string &network_id, const std::string &device_id, Saved &out) {
    for (const auto &d : entry(network_id).devices)
        if (d.id == device_id) { out = d; return true; }
    return false;
}

// Asks the controllable devices `pick` chooses for their things, names and states, and saves them.
std::string refresh_where(const std::string &network_id, const std::function<bool(const Saved &)> &pick) {
    std::string why;
    if (!on_network(network_id, why)) return why;
    const NetworkEntry e = entry(network_id);
    struct Out {
        const Saved *saved = nullptr;
        bool ok = false, setup = false;
        bool paired_meanwhile = false;   // set up while this ran: what pairing saved stands
        bool not_asked = false;          // the connection moved before its turn: nothing to save
        std::string problem;
        Device dev;
        std::vector<Thing> fresh;
    };
    std::vector<Out> outs;
    for (const auto &d : e.devices)
        if (pick(d) && d.controllable()) {
            Out o;
            o.saved = &d;
            outs.push_back(o);
        }
    run_parallel(outs.size(), kWorkers, [&](size_t k) {
        Out &o = outs[k];
        Driver *drv = driver_for(*o.saved);
        if (!drv) return;
        o.dev = *o.saved;
        if (!still_on(network_id, o.problem)) { o.not_asked = true; return; }
        PairStep step;
        try {
            step = drv->next_step(o.dev);
        } catch (const std::exception &) {
            o.problem = "Its driver could not tell what it needs.";
            return;
        }
        if (step.kind != Pairing::None) {
            o.setup = true;
            o.problem = step.prompt.empty() ? "It needs setting up first." : step.prompt;
            return;
        }
        std::string err;
        if (!may_contact(o.dev, err)) { o.problem = err; return; }
        std::lock_guard<std::mutex> lk(device_lock(o.dev.id));
        if (!still_on(network_id, o.problem)) { o.not_asked = true; return; }
        try {
            o.ok = drv->enumerate(o.dev, o.fresh, err);
        } catch (const std::exception &) {
            o.ok = false;
            err = "its answer could not be understood";
        }
        if (!o.ok) o.problem = err.empty() ? "It did not answer." : err;
    });
    // A family's account typed on another page while this ran: the step asked at the start is
    // out of date.
    for (auto &o : outs) {
        if (!o.setup) continue;
        Driver *drv = driver_for(*o.saved);
        try {
            o.paired_meanwhile = drv && drv->next_step(o.dev).kind == Pairing::None;
        } catch (...) {
        }
    }
    const time_t now = time(nullptr);
    modify_entry(network_id, [&](NetworkEntry &en) {
        for (const auto &o : outs)
            for (auto &d : en.devices) {
                if (d.id != o.saved->id || o.not_asked) continue;
                // The pairing is written back only if nobody paired or unpaired the device while
                // this ran: an Unpair must not be undone by a read that started before it.
                const bool pairing_as_read = d.auth == o.saved->auth && !o.paired_meanwhile;
                if (pairing_as_read) {
                    d.needs_setup = o.setup;
                    d.problem = o.problem;
                }
                if (!o.ok) continue;
                // Only what a read changes is written back, so that settings changed meanwhile stay.
                merge_things(d, o.fresh, now);
                const std::string name = clean_name(o.dev.name);
                if (!name.empty()) d.name = name;
                if (pairing_as_read && o.dev.auth.is_object()) d.auth = o.dev.auth;
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

}  // namespace

std::string refresh(const std::string &network_id, const std::string &only_device) {
    return refresh_where(network_id, [&](const Saved &d) { return only_device.empty() || d.id == only_device; });
}

std::string refresh_after_pairing(const std::string &network_id, const std::string &device_id) {
    Saved paired;
    if (!find_saved(network_id, device_id, paired)) return "That device is no longer saved.";
    Driver *drv = driver_for(paired);
    return refresh_where(network_id, [&](const Saved &d) {
        if (d.id == device_id) return true;
        if (!drv || driver_for(d) != drv) return false;
        // Another device of the family that the pairing made ready (pairing_submit asked it again)
        // and that has not been read yet: an account typed for one serves them all. One that
        // still needs its own setup (a second bridge with a key of its own) is left alone.
        return !d.needs_setup && !d.any_listed();
    });
}

// ---------------------------------------------------------------- pairing
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
        if (!still_on(network_id, err)) return false;
        try {
            ok = drv->pair(dev, input, err);
            next = drv->next_step(dev);
        } catch (const std::exception &) {
            ok = false;
            err = "The device's answer could not be understood.";
        }
    }
    if (!ok && err.empty()) err = "That did not work.";
    // What was typed may be the whole family's (an account every Tapo plug obeys): the family's
    // other devices are asked again what they need, so that one account readies them all, and a
    // refused one shows on each of them at once.
    std::map<std::string, PairStep> others;
    for (const auto &d : entry(network_id).devices) {
        if (d.id == device_id || driver_for(d) != drv) continue;
        try {
            others[d.id] = drv->next_step(d);
        } catch (...) {
        }
    }
    modify_entry(network_id, [&](NetworkEntry &e) {
        for (auto &d : e.devices) {
            if (d.id == device_id) {
                d.auth = dev.auth.is_object() ? dev.auth : json::object();
                d.needs_setup = next.kind != Pairing::None;
                if (!d.needs_setup && ok) d.problem.clear();
                continue;
            }
            const auto it = others.find(d.id);
            if (it == others.end()) continue;
            if (it->second.kind != Pairing::None) {
                d.needs_setup = true;
                d.problem = it->second.prompt.empty() ? "It needs setting up first." : it->second.prompt;
            } else if (d.needs_setup) {
                d.needs_setup = false;
                d.problem.clear();
            }
        }
    });
    return ok;
}

std::string unpair(const std::string &network_id, const std::string &device_id) {
    std::string why;
    Saved saved;
    if (!on_network(network_id, why)) return why;
    if (!find_saved(network_id, device_id, saved)) return "";
    Driver *drv = driver_for(saved);
    if (!drv) return "";
    // The driver decides what is forgotten. Some devices hold the pairing themselves (a HomeKit
    // accessory takes only one controller): forgetting it here without telling the device would
    // leave the device paired with an app that no longer has the key, refusing every new pairing.
    Device dev = saved;
    PairStep next;
    bool told = false;
    {
        std::lock_guard<std::mutex> lk(device_lock(dev.id));
        try {
            if (may_contact(dev, why) && still_on(network_id, why)) { drv->unpair(dev); told = true; }
            next = drv->next_step(dev);
        } catch (const std::exception &) {
            why = "something went wrong while talking to it";
        }
    }
    const bool kept = !dev.auth.empty();
    modify_entry(network_id, [&](NetworkEntry &e) {
        for (auto &d : e.devices)
            if (d.id == device_id) {
                d.auth = dev.auth;
                d.info = dev.info;   // a driver may correct the discovery record (a HomeKit accessory is free again)
                d.needs_setup = !kept && next.kind != Pairing::None;
            }
    });
    if (!kept) return "";
    if (told && why.empty()) return "It could not be reached, so it is still paired. Try again while it is switched on and on this network.";
    return "It is still paired: " + (why.empty() ? std::string("it could not be reached.") : why);
}

// ---------------------------------------------------------------- for the device-free tests
namespace testing {
void reset_control() {
    std::lock_guard<std::mutex> lk(g_ctl_mtx);
    g_turn = 0;
    g_last_said.clear();
    g_untrusted = false;
    g_untrusted_turn = 0;
    g_context_from = 0;
    g_pending.clear();
    g_rate.reset();
    std::lock_guard<std::mutex> lookup(g_lookup_mtx);
    g_found.clear();
    g_found_on.clear();
    g_found_at = 0;
}
void set_scan(bool (*scan)(std::vector<discovery::Device> &found)) {
    std::lock_guard<std::mutex> lk(g_lookup_mtx);
    g_test_scan = scan;
}
}  // namespace testing

}  // namespace home
