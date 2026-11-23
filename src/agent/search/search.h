// Web search: the one layer between the web_search tool and the services that answer it. Each service is
// a Provider (provider.h), one file in providers/ and one line in providers/registry.cpp; this layer asks
// the services of the user's list (Settings::search) in its order until one answers, and shapes what the
// model reads, the same whichever service answered. Definitions in search.cpp.
//
// Failover, inside one search and never back through the model: a service that cannot be reached, is too
// slow, fails or answers garbled is followed by the next at once; one that found nothing by the next, once.
// A search takes 8 s at most, the first of several services 5 s of them. A key a service refused is not
// sent again until it is typed again (marked beside it in the settings, so a later run does not send it
// either); a spent allowance is passed over until it comes back (when the service says, else for six
// hours); a service that failed twice in a row rests for two minutes (asked only when no other can be).
// The order never changes by itself. When no service answered, the model reads one ERROR sentence.
#pragma once
#include <string>

#include "agent/tools.h"
#include "core/settings.h"

namespace search {

// Whether a service is set up: an entry of the user's list that is switched on, of a kind this version
// knows, with a key. The web_search tool is offered, and the prompt says search is available, by this
// alone, never by whether a service has allowance left today or took its key: both stay the same all
// conversation long.
bool configured(const Settings &s);

// The web_search tool's result for `query`: {"summary"?, "results": [{"title", "source", "text"}]}, or one
// "ERROR: ..." when no service answered.
std::string run(const std::string &query, tools::Context &ctx);

}  // namespace search
