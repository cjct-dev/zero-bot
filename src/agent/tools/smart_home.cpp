// The smart_home tool: the user's smart-home devices on this network.
#include "agent/tools/common.h"

#include "smarthome/home.h"

namespace tools {

namespace {

// Schema, name matching, safety questions and the devices themselves are all in smarthome/.
// With ZB_DRY_RUN the choice of things is checked without switching anything in the house.
std::string t_smart_home(const json &a, Context &) { return home::tool_run(a, dry_run()); }

}  // namespace

Tool make_smart_home() {
    // Description and parameters are sized to the home on the current network (home::tool_spec).
    return {"smart_home", "Using a home device", "", obj({}), t_smart_home};
}

}  // namespace tools
