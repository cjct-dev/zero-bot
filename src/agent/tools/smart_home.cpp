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
    // Its say is not spoken for a question to put to the user first, a status, or a group where something failed.
    Tool t{"smart_home", "Using a home device", "", obj({}), t_smart_home, takes_say("", home::tool_say_ok)};
    // Description and parameters (with its say) are sized to the home on the current network (home::tool_spec).
    // The tool exists only on a network the user has scanned, and names exactly the things saved for it. The
    // names are passed as allowed values, as data, never as instructions.
    t.spec = [] { return home::tool_spec(); };
    return t;
}

}  // namespace tools
