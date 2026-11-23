// The web search services built into the app.
//
// Adding a service is one line in the list below, X(<kind>, "<Name>"), plus its file in this folder,
// <kind>.cpp, which implements search::Provider (agent/search/provider.h) and defines
//     std::unique_ptr<search::Provider> make_<kind>_provider();
// The kind is what the settings call it (SearchService::kind); the name is what people do. The order
// here is not the order services are tried in (that is the user's list in the settings): the first one
// is the service whose key the settings' one key row and the setup guide ask for.
#include "agent/search/provider.h"

#define ZB_SEARCH_PROVIDERS(X) \
    X(tavily, "Tavily") \
    /* end of the list */

#define ZB_DECLARE(kind, name) std::unique_ptr<search::Provider> make_##kind##_provider();
ZB_SEARCH_PROVIDERS(ZB_DECLARE)
#undef ZB_DECLARE

namespace search {

void register_builtin_providers() {
#define ZB_REGISTER(kind, name) register_provider(make_##kind##_provider(), name);
    ZB_SEARCH_PROVIDERS(ZB_REGISTER)
#undef ZB_REGISTER
}

}  // namespace search
