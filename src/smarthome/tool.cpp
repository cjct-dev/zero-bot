// The assistant's smart_home tool: a schema sized to what this home has, and its arguments turned
// into a Command. The model picks from enums wherever it can; everything fuzzy (names, rooms,
// colours, units) is settled in code after the call (targets.cpp, control.cpp). A home with only
// plugs sees the same small schema as before things existed: action and target, plus say.
#include <algorithm>
#include <cmath>

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

// Models sometimes send numbers as strings; accept both.
bool number_arg(const json &a, const char *key, double &out) {
    const auto it = a.find(key);
    if (it == a.end() || it->is_null()) return false;
    if (it->is_number()) { out = it->get<double>(); return std::isfinite(out); }
    if (it->is_string()) {
        const std::string s = it->get<std::string>();
        char *end = nullptr;
        out = strtod(s.c_str(), &end);
        return end != s.c_str() && std::isfinite(out);
    }
    return false;
}

std::optional<int> int_arg(const json &a, const char *key) {
    double v;
    if (!number_arg(a, key, v)) return std::nullopt;
    return static_cast<int>(std::lround(std::max(-1e6, std::min(1e6, v))));
}

std::string joined(const std::vector<std::string> &parts) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) out += (i == 0 ? "" : i + 1 == parts.size() ? " and " : ", ") + parts[i];
    return out;
}

json spec_for(const std::vector<Item> &items) {
    uint32_t caps = 0;
    bool scenes = false, lights = false, tvs = false;
    for (const auto &i : items) {
        caps |= i.thing.caps;
        scenes = scenes || i.thing.type == "scene";
        lights = lights || is_light(i.thing);
        tvs = tvs || i.thing.type == "tv";
    }
    const bool brightness = caps & kBrightness, colour = caps & (kColor | kColorTemp), heat = caps & kTargetTemp,
               position = caps & kPosition, volume = caps & kVolume, source = caps & kSource, transport = caps & kTransport,
               volume_keys = caps & kVolumeStep, keys = caps & kKeys;
    const bool values = brightness || colour || heat || position || volume || volume_keys || source;
    const bool plugs_only = !values && !scenes && !transport && !lights;

    std::vector<std::string> actions = {"on", "off"};
    if (values) actions.push_back("set");
    actions.push_back("status");
    if (scenes) actions.push_back("scene");
    if (transport)
        for (const char *a : {"play", "pause", "stop", "next", "previous"}) actions.push_back(a);
    if (volume) { actions.push_back("mute"); actions.push_back("unmute"); }
    if (keys) actions.push_back("press");

    bool overflow = false;
    const std::vector<std::string> targets = target_names(items, &overflow);
    const bool all = std::find(targets.begin(), targets.end(), "all") != targets.end();

    json props = json::object();
    props["action"] = enum_of(values ? "What to do. set changes the values given with it; status with no target reports everything"
                                     : "What to do; status with no target reports all of them",
                              actions);
    std::string target = plugs_only ? "The device's name, exactly as listed." : "A device, room or group, exactly as listed.";
    if (lights) target += " A room means the lights in that room: when the user names a room (\"the lounge\"), use the "
                          "room itself, not a device whose name mentions it.";
    if (all) target += plugs_only ? " \"all\" means every device, for switching everything off or checking everything"
                                  : " \"all\" is only for switching everything off or checking everything.";
    if (overflow) target += (all && plugs_only ? ". " : " ") + std::string("Other device names are also accepted.");
    props["target"] = enum_of(target, targets);
    if (brightness) props["brightness"] = percent("Light level in percent, with set");
    if (colour) {
        std::vector<std::string> shades;
        for (const auto &c : color::names())
            if (c.kelvin || (caps & kColor)) shades.push_back(c.name);
        props["color"] = enum_of("A colour or shade of white, with set", shades);
    }
    if (heat) props["temperature"] = {{"type", "number"}, {"description", "Heating target in degrees Celsius, with set"}};
    if (position) props["position"] = percent("Blinds and curtains: 0 closed to 100 open, with set");
    if (volume) props["volume"] = percent("Speaker or TV volume, 0 to 100, with set");
    if (volume_keys)
        props["volume_step"] = {{"type", "integer"}, {"minimum", -10}, {"maximum", 10},
                                {"description", "Volume key presses, with set, for a device that only has volume up and down: "
                                                "positive louder, negative quieter, e.g. 3 or -3"}};
    if (source) props["source"] = {{"type", "string"}, {"description", "A TV input or app, with set, e.g. \"Netflix\" or \"HDMI 1\""}};
    if (keys)
        props["key"] = enum_of("With press: the remote-control key to press once (moving around a TV's menus)",
                               std::vector<std::string>(std::begin(kTvKeys), std::end(kTvKeys)));
    props["say"] = {{"type", "string"},
                    {"description", "The one short sentence to speak to the user once this has worked, in their language, with "
                                    "the detail that matters, e.g. \"Your timer is set for 12 minutes.\" Omit for status."}};

    std::string description;
    if (plugs_only) {
        description = "Switch one of the user's smart-home devices on this network on or off, or check whether it is on. "
                      "Calling this is the only way a device gets switched. The devices that exist are listed in the target "
                      "parameter; if what the user asks for is not one of them, say so instead of picking another.";
    } else {
        std::vector<std::string> verbs;
        if (caps & kPower) verbs.push_back("switch on or off");
        if (brightness) verbs.push_back("dim");
        if (colour) verbs.push_back("change colour");
        if (heat) verbs.push_back("set the heating temperature");
        if (position) verbs.push_back("open or close blinds");
        if (volume) verbs.push_back("set the volume");
        if (volume_keys) verbs.push_back("turn a TV's volume up or down");
        if (transport) verbs.push_back("control playback");
        if (source) verbs.push_back("change a TV's input or open an app");
        if (keys) verbs.push_back("press a TV's remote keys (use this, not tv_remote, for the TVs listed here)");
        if (scenes) verbs.push_back("start scenes");
        description = "Control the user's smart-home devices on this network: " + joined(verbs) +
                      ", or check their state. Calling this is the only way a device changes. The targets are listed in the "
                      "target parameter" + std::string(lights ? "; a room name means the lights in that room" : "") +
                      ". If what the user asks for is not listed, say so instead of picking another.";
        // The infrared remote sends the codes of the one brand in settings, which may be another TV's.
        if (tvs) description += " When a TV listed here does not answer, say so without offering infrared instead: the infrared "
                                "remote may send another TV's codes.";
    }
    return {{"description", description},
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
    c.brightness = int_arg(args, "brightness");
    c.position = int_arg(args, "position");
    c.volume = int_arg(args, "volume");
    c.volume_step = int_arg(args, "volume_step");
    c.key = text_arg(args, "key");
    double t;
    if (number_arg(args, "temperature", t)) c.temperature = t;
    c.color = text_arg(args, "color");
    if (c.color.empty()) c.color = text_arg(args, "colour");
    c.source = text_arg(args, "source");
    return execute(c, dry_run);
}

bool tool_say_ok(const json &args, const std::string &result) {
    const std::string action = util::lower(text_arg(args, "action"));
    if (action.empty() || action == "status" || action == "list" || action == "check") return false;
    if (result.compare(0, 5, "ERROR") == 0 || result.compare(0, 7, "CONFIRM") == 0 || result.compare(0, 9, "{\"error\"") == 0)
        return false;
    const json j = json::parse(result, nullptr, false);
    // A group where something failed has to be reported from the result, not from the sentence.
    return j.is_object() && !j.contains("failed");
}

}  // namespace home
