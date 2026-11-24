// The tools the model is offered, in the order it is offered them (--schema shows the list).
//
// Adding a tool is one line in the list below, X(<name>), plus its file in this folder, <name>.cpp,
// which holds the tool's function and defines its spec (tools.h: name, label, description, schema, and
// whether it takes "say"; common.h has the helpers):
//     Tool make_<name>();
// Nothing else changes: the schema, the "say" parameter and confirmations, and the runner's checks of
// the arguments (run.cpp) all follow from the spec.
// The list's order is the order the model reads the tools in: put a new one where it reads best.
#include "agent/tools/common.h"

#include "agent/controls.h"

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

std::vector<Tool> build() {
    std::vector<Tool> list = {
#define ZB_MAKE(name) make_##name(),
        ZB_TOOLS(ZB_MAKE)
#undef ZB_MAKE
    };
    // "say" (tools.h), in the same words for every tool that takes it, with the tool's own note after them.
    for (Tool &t : list) {
        if (!t.say.taken || t.spec) continue;   // a schema made at run time brings its own say
        std::string desc = "The one short sentence to speak to the user once this has worked, in their language, with "
                           "the detail that matters, e.g. \"Your timer is set for 12 minutes.\"";
        if (!t.say.note.empty()) desc += " " + t.say.note;
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
    for (auto &t : all()) {
        if (t.offered && !t.offered()) continue;
        json description = t.description, parameters = t.parameters;
        if (t.spec) {   // made for this request from the user's data (smart_home, radio)
            const json spec = t.spec();
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
    const Tool *tool = find(name);
    if (!tool || !tool->say.taken) return "";
    if (failed(result) || result::awaits_yes(result)) return "";   // the model has the user's yes to ask for first
    const json args = util::parse_untrusted_json(raw_args.empty() ? "{}" : raw_args);
    if (!args.is_object()) return "";
    if (tool->say.when && !tool->say.when(args, result)) return "";   // the tool's own exceptions (Say::when)
    return str_arg(args, "say");
}

}  // namespace tools
