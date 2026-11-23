// The contract between the web search layer (search.h) and the code that talks to one search service.
// A provider turns a query into a reply: the service's summary and results, and what its answer meant.
//
// Each service is one file, providers/<kind>.cpp, defining
//     std::unique_ptr<search::Provider> make_<kind>_provider();
// and one line in providers/registry.cpp. What a provider does: build its request, send it with
// tools::fetch_json(experiment::web(address), ..., how), map the answer onto Reply and say what it meant.
// What it never does, because search.cpp does it once for every service: retry or wait (fetch_json does
// what `how` says), pick another service, count allowance, cache, write files, or shape what the model
// reads (a provider fills Reply with the service's own words; search.cpp alone caps and cleans them).
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "agent/tools.h"
#include "agent/tools/web.h"
#include "core/settings.h"

namespace search {

// One result as the service gave it.
struct Hit {
    std::string title, url, text;
};

// What a search came to. Every outcome but Ok and Empty is a failure.
enum class Outcome {
    Ok,            // a summary or results
    Empty,         // the service answered, with neither
    Unreachable,   // no answer: no connection, a name or TLS failure, a connection dropped, a connect timeout
    Slow,          // the request went out and no answer came within the deadline
    Failing,       // the service's own trouble or a refusal of the request: 5xx, 429, 408, any other 3xx or 4xx
    KeyRefused,    // the key was refused (401, 403)
    Spent,         // the allowance is used up
    Garbled,       // a success whose body is not what the service sends
};

struct Reply {
    Outcome outcome = Outcome::Unreachable;
    std::string summary;           // the service's own short answer, when it gives one
    std::vector<Hit> hits;         // in the service's order
    std::string error;             // on a failure: a short sentence for the model (fetch_json's words)
};

// The outcome any HTTP service's answer means (fetch_json's `err` and `got`); a provider adds its own
// codes on top (Tavily's 432 and 433 for a spent allowance).
Outcome outcome_of(const tools::Fetched &got, const std::string &err);

class Provider {
public:
    virtual ~Provider() = default;
    virtual const char *kind() const = 0;   // as the settings name it (SearchService::kind): "tavily"
    // One search for `query` with the service's settings; `how` holds the deadline and attempts this
    // search may take, to which the provider adds its headers and body.
    virtual Reply run(const std::string &query, const SearchService &service, tools::Fetch how,
                      tools::Context &ctx) const = 0;
};

// The services this version knows, in the registry's order, each with the name people call it by.
struct Registered {
    std::unique_ptr<Provider> provider;
    std::string name;
};
const std::vector<Registered> &registry();
const Provider *provider_for(const std::string &kind);   // null for a kind this version does not know

}  // namespace search
