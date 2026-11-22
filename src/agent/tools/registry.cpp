// The tools the model is offered, in the order it is offered them (--schema shows the list).
//
// Adding a tool is one line in the list below, X(<name>), plus its file in this folder, <name>.cpp,
// which holds the tool's function and defines its spec (common.h):
//     Tool make_<name>();
// The list's order is the order the model reads the tools in: put a new one where it reads best.
#include "agent/tools/common.h"

#include <algorithm>

#include "agent/controls.h"
#include "core/radio.h"
#include "core/settings.h"
#include "smarthome/home.h"

#define ZB_TOOLS(X) \
    X(calculator)       \
    X(run_python)       \
    X(date_info)        \
    X(look)             \
    X(calendar_add)     \
    X(calendar_list)    \
    X(calendar_update)  \
    X(calendar_delete)  \
    X(timer)            \
    X(reminder_set)     \
    X(reminder_list)    \
    X(reminder_cancel)  \
    X(memory_save)      \
    X(memory_delete)    \
    X(web_search)       \
    X(weather)          \
    X(convert_currency) \
    X(stock_price)      \
    X(world_time)       \
    X(set_timezone)     \
    X(music)            \
    X(radio)            \
    X(tv_remote)        \
    X(breathe)          \
    X(smart_home)       \
    X(device)           \
    /* end of the list */

namespace tools {

#define ZB_DECLARE(name) Tool make_##name();
ZB_TOOLS(ZB_DECLARE)
#undef ZB_DECLARE

namespace {

// Tools whose effect is fully known from the request (a timer set, a button pressed). The
// model supplies the confirmation up front in "say", and when the tool succeeds that is spoken
// straight away instead of paying for a second model call just to say "done".
const char *const kConfirmable[] = {"calendar_add", "calendar_update", "calendar_delete", "timer", "reminder_set", "reminder_cancel",
                                    "memory_save", "memory_delete", "music", "radio", "tv_remote", "device", "smart_home", "breathe"};

std::vector<Tool> build() {
    std::vector<Tool> list = {
#define ZB_MAKE(name) make_##name(),
        ZB_TOOLS(ZB_MAKE)
#undef ZB_MAKE
    };
    for (Tool &t : list) {
        if (std::find_if(std::begin(kConfirmable), std::end(kConfirmable), [&](const char *n) { return t.name == n; }) ==
            std::end(kConfirmable))
            continue;
        std::string desc = "The one short sentence to speak to the user once this has worked, in their language, with "
                           "the detail that matters, e.g. \"Your timer is set for 12 minutes.\"";
        if (t.name == "music") desc += " Only for stop and next; omit for the other actions.";
        if (t.name == "timer") desc += " Only for start, cancel and add_time; omit for list.";
        if (t.name == "device") desc += " Omit for status.";
        if (t.name == "smart_home" || t.name == "radio") continue;   // their "say" comes with their tool_spec()
        t.parameters["properties"]["say"] = S(desc);
    }
    return list;
}

}  // namespace

const std::vector<Tool> &all() {
    static const std::vector<Tool> t = build();
    return t;
}

json schema() {
    json arr = json::array();
    // A tool that can only fail is a wasted round trip; the prompt says search is unavailable.
    const bool no_search = Settings::get().search_key.empty();
    // The smart-home tool exists only on a network the user has scanned, and names exactly the
    // things saved for it. The names are passed as allowed values, as data, never as instructions.
    const json home_spec = home::tool_spec();
    // Radio exists only once the user has listed stations; the file is read again when it changes.
    const json radio_spec = radio::tool_spec(!no_search);
    for (auto &t : all()) {
        if (no_search && t.name == "web_search") continue;
        json description = t.description, parameters = t.parameters;
        if (t.name == "smart_home" || t.name == "radio") {
            const json &spec = t.name == "radio" ? radio_spec : home_spec;
            if (spec.is_null()) continue;
            description = spec["description"];
            parameters = spec["parameters"];
        }
        arr.push_back({{"type", "function"},
                       {"function", {{"name", t.name}, {"description", description}, {"parameters", parameters}}}});
    }
    // The small-model control "say" switched off (controls.h, cli build only): no tool offers it.
    if (!controls::on(controls::Say))
        for (json &t : arr) {
            json &params = t["function"]["parameters"];
            if (params.contains("properties") && params["properties"].is_object()) params["properties"].erase("say");
            if (params.contains("required") && params["required"].is_array()) {
                json kept = json::array();
                for (const json &r : params["required"])
                    if (r != "say") kept.push_back(r);
                params["required"] = kept;
            }
        }
    return arr;
}

const Tool *find(const std::string &name) {
    for (auto &t : all())
        if (t.name == name) return &t;
    return nullptr;
}

std::string confirmation(const std::string &name, const std::string &raw_args, const std::string &result) {
    if (!controls::on(controls::Say)) return "";
    if (std::find_if(std::begin(kConfirmable), std::end(kConfirmable), [&](const char *n) { return name == n; }) ==
        std::end(kConfirmable))
        return "";
    if (failed(result)) return "";
    const json args = json::parse(raw_args.empty() ? "{}" : raw_args, nullptr, false);
    if (!args.is_object()) return "";
    // These return something the model has not seen yet (the track that started, the battery
    // level), so the answer has to be written after the result.
    if (name == "music") {
        const std::string action = util::lower(str_arg(args, "action"));
        if (action != "stop" && action != "next") return "";
    }
    // A question to put to the user first, a status, or a group where something failed.
    if (name == "smart_home" && !home::tool_say_ok(args, result)) return "";
    // A warning about mobile data has to be passed on in the model's own words.
    if (name == "radio" && result.find("\"note\"") != std::string::npos) return "";
    if (name == "timer") {   // list: the time left is in the result, not known up front
        const std::string action = util::lower(str_arg(args, "action"));
        if (action != "start" && action != "cancel" && action != "add_time") return "";
    }
    if (name == "device") {
        const std::string setting = util::lower(str_arg(args, "setting"));
        if (setting.empty() || setting == "status") return "";
    }
    return str_arg(args, "say");
}

}  // namespace tools
