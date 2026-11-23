// Web search: the one layer between the web_search tool and the services that answer it. Each service is
// a Provider (provider.h), one file in providers/ and one line in providers/registry.cpp; this layer picks
// the service from the user's list (Settings::search) and shapes what the model reads, the same whichever
// service answered. Definitions in search.cpp.
#pragma once
#include <string>

#include "agent/tools.h"
#include "core/settings.h"

namespace search {

// Whether a service is set up: an entry of the user's list that is switched on, of a kind this version
// knows, with a key. The web_search tool is offered, and the prompt says search is available, by this
// alone, never by whether a service has allowance left today: both stay the same all conversation long.
bool configured(const Settings &s);

// The web_search tool's result for `query`: {"summary"?, "results": [{"title", "source", "text"}]}, or one
// "ERROR: ..." when the search failed.
std::string run(const std::string &query, tools::Context &ctx);

}  // namespace search
