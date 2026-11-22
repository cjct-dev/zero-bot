// The radio tool: the user's own radio stations.
#include "agent/tools/common.h"

#include "core/radio.h"

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

}  // namespace

Tool make_radio() {
    // Description and parameters are made from the user's station list (radio::tool_spec).
    return {"radio", "Tuning in", "", obj({}), t_radio};
}

}  // namespace tools
