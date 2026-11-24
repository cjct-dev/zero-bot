// The radio tool: the user's own radio stations.
#include "agent/tools/common.h"

#include "agent/search/search.h"
#include "agent/tools/say_checks.h"
#include "core/radio.h"
#include "core/settings.h"

namespace tools {

namespace {

// The stations, the check of a stream and the tool's wording are in core/radio.
std::string t_radio(const json &a, Context &ctx) {
    radio::ToolEnv env;
    env.dry_run = dry_run();
    env.in_conversation = ctx.in_conversation;
    env.cancel = ctx.cancel;
    return radio::run_tool(a, env);
}

// A warning about mobile data has to be passed on in the model's own words, and a stop that announces what starts
// next leaves that step to the model (claims_start).
bool say_confirms(const json &a, const std::string &result) {
    if (result.find("\"note\"") != std::string::npos) return false;
    return !(util::lower(str_arg(a, "action")) == "stop" && claims_start(a));
}

}  // namespace

Tool make_radio() {
    Tool t{"radio", "Tuning in", "", obj({}), t_radio, takes_say("", say_confirms)};
    // Description and parameters are made from the user's station list (radio::tool_spec), with its say. Radio
    // exists only once the user has listed stations; the file is read again when it changes.
    t.spec = [] { return radio::tool_spec(search::configured(Settings::get())); };
    return t;
}

}  // namespace tools
