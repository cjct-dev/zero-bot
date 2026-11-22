// The assistant's smart_home tool: a schema sized to what this home has, and its arguments turned
// into a Command. The model picks from enums wherever it can; everything fuzzy (names, rooms,
// colours, units) is settled in code after the call (targets.cpp, control.cpp). A home with only
// plugs sees the same small schema as before things existed: action and target, plus say.
#include <algorithm>
#include <cmath>

#include "core/result.h"
#include "smarthome/color.h"
#include "smarthome/internal.h"

namespace home {

namespace {

json enum_of(const std::string &desc, const std::vector<std::string> &values) {
    return {{"type", "string"}, {"description", desc}, {"enum", values}};
}

json percent(const std::string &desc) {
    return {{"type", "integer"}, {"minimum", 0}, {"maximum", 100}, {"description", desc}};
}

std::vector<Item> current_items() {
    const discovery::Network net = network_now();
    if (!net.usable) return {};
    return catalog(entry(net.id()));
}

std::string text_arg(const json &a, const char *key) {
    const auto it = a.find(key);
    return it != a.end() && it->is_string() ? util::trim(it->get<std::string>()) : std::string();
}

// A number argument, as models send them: a number, or text that is one as a whole (util::to_number:
// "40oops" is not 40), with its sign for a percentage ("40%" is 40). Not given (absent, null or empty
// text): nullopt. Given but no number: nullopt, and `bad` (when still empty) says so, so that the call
// is refused before any target is chosen.
std::optional<double> number_arg(const json &a, const char *key, bool percent, std::string &bad) {
    const auto it = a.find(key);
    if (it == a.end() || it->is_null()) return std::nullopt;
    double v = 0;
    if (it->is_number() && std::isfinite(v = it->get<double>())) return v;
    if (it->is_string()) {
        std::string s = util::trim(it->get<std::string>());
        if (s.empty()) return std::nullopt;
        if (percent && s.back() == '%') s.pop_back();
        if (util::to_number(s, v)) return v;
    }
    if (bad.empty())
        bad = std::string(key) + " must be a number, such as " + (percent ? "40" : "5") + "; " +
              util::truncate_utf8(it->dump(-1, ' ', false, json::error_handler_t::replace), 40) + " is not one";
    return std::nullopt;
}

std::optional<int> int_arg(const json &a, const char *key, bool percent, std::string &bad) {
    const std::optional<double> v = number_arg(a, key, percent, bad);
    if (!v) return std::nullopt;
    return static_cast<int>(std::lround(std::max(-1e6, std::min(1e6, *v))));
}

std::string joined(const std::vector<std::string> &parts) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) out += (i == 0 ? "" : i + 1 == parts.size() ? " and " : ", ") + parts[i];
    return out;
}

// What the things offered can do between them, which decides the tool's actions, parameters and
// description: the schema grows with the home.
struct Abilities {
    uint32_t caps = 0;
    bool scenes = false, lights = false;
    bool brightness = false, colour = false, heat = false, position = false, volume = false, source = false,
         transport = false, volume_keys = false, keys = false;
    bool values = false;       // set has something to change
    bool plugs_only = false;   // only switching on and off: the plainest wording
};

Abilities abilities_of(const std::vector<Item> &items) {
    Abilities ab;
    for (const auto &i : items) {
        ab.caps |= i.thing.caps;
        ab.scenes = ab.scenes || i.thing.type == "scene";
        ab.lights = ab.lights || is_light(i.thing);
    }
    const uint32_t caps = ab.caps;
    ab.brightness = caps & kBrightness;
    ab.colour = caps & (kColor | kColorTemp);
    ab.heat = caps & kTargetTemp;
    ab.position = caps & kPosition;
    ab.volume = caps & kVolume;
    ab.source = caps & kSource;
    ab.transport = caps & kTransport;
    ab.volume_keys = caps & kVolumeStep;
    ab.keys = caps & kKeys;
    ab.values = ab.brightness || ab.colour || ab.heat || ab.position || ab.volume || ab.volume_keys || ab.source;
    ab.plugs_only = !ab.values && !ab.scenes && !ab.transport && !ab.lights;
    return ab;
}

std::vector<std::string> actions_for(const Abilities &ab) {
    std::vector<std::string> actions = {"on", "off"};
    if (ab.values) actions.push_back("set");
    actions.push_back("status");
    if (ab.scenes) actions.push_back("scene");
    if (ab.transport)
        for (const char *a : {"play", "pause", "stop", "next", "previous"}) actions.push_back(a);
    if (ab.volume) { actions.push_back("mute"); actions.push_back("unmute"); }
    if (ab.keys) actions.push_back("press");
    return actions;
}

// The target parameter's description. `all`: "all" is among the targets; `overflow`: not every name is.
std::string target_text(const Abilities &ab, bool all, bool overflow) {
    std::string target = ab.plugs_only ? "The device's name, exactly as listed." : "A device, room or group, exactly as listed.";
    if (ab.lights) target += " A room means the lights in that room: when the user names a room (\"the lounge\"), use the "
                             "room itself, not a device whose name mentions it.";
    if (all) target += ab.plugs_only ? " \"all\" means every device, for switching everything off or checking everything"
                                     : " \"all\" is only for switching everything off or checking everything.";
    if (overflow) target += (all && ab.plugs_only ? ". " : " ") + std::string("Other device names are also accepted.");
    return target;
}

// The values set can change, each only when a thing offered has it.
void add_values(json &props, const Abilities &ab) {
    if (ab.brightness) props["brightness"] = percent("Light level in percent, with set");
    if (ab.colour) {
        std::vector<std::string> shades;
        for (const auto &c : color::names())
            if (c.kelvin || (ab.caps & kColor)) shades.push_back(c.name);
        props["color"] = enum_of("A colour or shade of white, with set", shades);
    }
    if (ab.heat) props["temperature"] = {{"type", "number"}, {"description", "Heating target in degrees Celsius, with set"}};
    if (ab.position) props["position"] = percent("Blinds and curtains: 0 closed to 100 open, with set");
    if (ab.volume) props["volume"] = percent("Speaker or TV volume, 0 to 100, with set");
    if (ab.volume_keys)
        props["volume_step"] = {{"type", "integer"}, {"minimum", -10}, {"maximum", 10},
                                {"description", "Volume key presses, with set, for a device that only has volume up and down: "
                                                "positive louder, negative quieter, e.g. 3 or -3"}};
    if (ab.source) props["source"] = {{"type", "string"}, {"description", "A TV input or app, with set, e.g. \"Netflix\" or \"HDMI 1\""}};
    if (ab.keys)
        props["key"] = enum_of("With press: the remote-control key to press once (moving around a TV's menus)",
                               std::vector<std::string>(std::begin(kTvKeys), std::end(kTvKeys)));
}

json properties_for(const Abilities &ab, const std::vector<Item> &items) {
    const std::vector<std::string> actions = actions_for(ab);
    bool overflow = false;
    const std::vector<std::string> targets = target_names(items, &overflow);
    const bool all = std::find(targets.begin(), targets.end(), "all") != targets.end();

    json props = json::object();
    props["action"] = enum_of(ab.values ? "What to do. set changes the values given with it; status with no target reports everything"
                                        : "What to do; status with no target reports all of them",
                              actions);
    props["target"] = enum_of(target_text(ab, all, overflow), targets);
    add_values(props, ab);
    props["say"] = {{"type", "string"},
                    {"description", "The one short sentence to speak to the user once this has worked, in their language, with "
                                    "the detail that matters, e.g. \"Your timer is set for 12 minutes.\" Omit for status."}};
    return props;
}

std::string description_for(const Abilities &ab) {
    if (ab.plugs_only)
        return "Switch one of the user's smart-home devices on this network on or off, or check whether it is on. "
               "Calling this is the only way a device gets switched. The devices that exist are listed in the target "
               "parameter; if what the user asks for is not one of them, say so instead of picking another.";
    std::vector<std::string> verbs;
    if (ab.caps & kPower) verbs.push_back("switch on or off");
    if (ab.brightness) verbs.push_back("dim");
    if (ab.colour) verbs.push_back("change colour");
    if (ab.heat) verbs.push_back("set the heating temperature");
    if (ab.position) verbs.push_back("open or close blinds");
    if (ab.volume) verbs.push_back("set the volume");
    if (ab.volume_keys) verbs.push_back("turn a TV's volume up or down");
    if (ab.transport) verbs.push_back("control playback");
    if (ab.source) verbs.push_back("change a TV's input or open an app");
    if (ab.keys) verbs.push_back("press a TV's remote keys (use this, not tv_remote, for the TVs listed here)");
    if (ab.scenes) verbs.push_back("start scenes");
    return "Control the user's smart-home devices on this network: " + joined(verbs) +
           ", or check their state. Calling this is the only way a device changes. The targets are listed in the "
           "target parameter" + std::string(ab.lights ? "; a room name means the lights in that room" : "") +
           ". If what the user asks for is not listed, say so instead of picking another.";
}

json spec_for(const std::vector<Item> &items) {
    const Abilities ab = abilities_of(items);
    const json props = properties_for(ab, items);
    return {{"description", description_for(ab)},
            {"parameters", {{"type", "object"}, {"properties", props}, {"required", json::array({"action"})}}}};
}

}  // namespace

size_t thing_count() { return current_items().size(); }

bool available() { return thing_count() > 0; }

json tool_spec() {
    const std::vector<Item> items = current_items();
    if (items.empty()) return nullptr;
    return spec_for(items);
}

std::string tool_run(const json &args, bool dry_run) {
    Command c;
    c.action = text_arg(args, "action");
    c.target = text_arg(args, "target");
    if (c.target.empty()) c.target = text_arg(args, "device");   // the parameter's earlier name
    std::string bad;   // the first number given that is no number
    c.brightness = int_arg(args, "brightness", true, bad);
    c.position = int_arg(args, "position", true, bad);
    c.volume = int_arg(args, "volume", true, bad);
    c.volume_step = int_arg(args, "volume_step", false, bad);
    c.temperature = number_arg(args, "temperature", false, bad);
    if (!bad.empty()) return result::fail(bad);
    c.key = text_arg(args, "key");
    c.color = text_arg(args, "color");
    if (c.color.empty()) c.color = text_arg(args, "colour");
    c.source = text_arg(args, "source");
    return execute(c, dry_run);
}

bool tool_say_ok(const json &args, const std::string &result) {
    const std::string action = util::lower(text_arg(args, "action"));
    if (action.empty() || action == "status" || action == "list" || action == "check") return false;
    if (result::failed(result) || result.compare(0, 7, "CONFIRM") == 0) return false;
    const json j = json::parse(result, nullptr, false);
    // A group where something failed has to be reported from the result, not from the sentence.
    return j.is_object() && !j.contains("failed");
}

}  // namespace home
