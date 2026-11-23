// The contract between the web search layer (search.h) and the code that talks to one search service.
// A provider turns a query into a reply: the service's summary and results, and what its answer meant.
//
// Each service is one file, providers/<kind>.cpp, defining
//     std::unique_ptr<search::Provider> make_<kind>_provider();
// and one line in providers/registry.cpp. What a provider does: build its request, send it with
// tools::fetch_json(experiment::web(address), ..., how), map the answer onto Reply and say what it meant.
// What it never does, because search.cpp and usage.cpp do it once for every service: retry or wait (fetch_json
// does what `how` says), pick another service, count allowance, cache, write files, or shape what the model
// reads (a provider fills Reply with the service's own words; search.cpp alone caps and cleans them).
#pragma once
#include <ctime>
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
    Failing,       // the service's own trouble or a refusal of the request: 5xx, 429, 408, 403, any other 3xx or 4xx
    KeyRefused,    // the key was refused (401, or the service's own word for it)
    Spent,         // the allowance is used up
    Garbled,       // a success whose body is not what the service sends
};

struct Reply {
    Outcome outcome = Outcome::Unreachable;
    std::string summary;           // the service's own short answer, when it gives one
    std::vector<Hit> hits;         // in the service's order
    std::string error;             // on a failure: a short sentence (fetch_json's words)
    // What the service said of its allowance, where it says: how much is left after this search, of how much, in
    // its own units (-1 when it did not say; 0 left is spent), and in how many seconds it comes back (with that, or
    // with Spent; 0 when it did not say).
    long left = -1, limit = -1;
    long reset_s = 0;
};

// The outcome any HTTP service's answer means (fetch_json's `err` and `got`); a provider adds its own
// codes on top (Tavily's 432 and 433 for a spent allowance, Brave's error codes). Only 401 says the key was
// refused: a bare 403 is what a firewall, a proxy or a country block answers as well, and a refusal is kept
// until the key is typed again, so it would switch search off for a fault that is not the key's.
Outcome outcome_of(const tools::Fetched &got, const std::string &err);

class Provider {
public:
    virtual ~Provider() = default;
    virtual const char *kind() const = 0;   // as the settings name it (SearchService::kind): "tavily"
    // One search for `query` with the service's settings; `how` holds the deadline and attempts this
    // search may take, to which the provider adds its headers and body.
    virtual Reply run(const std::string &query, const SearchService &service, tools::Fetch how,
                      tools::Context &ctx) const = 0;
    // Whether the service can be asked what is left of its allowance in a way that costs none of it (Tavily's
    // GET /usage), and the asking: the outcome and Reply::left and limit, nothing else. Only when the user asks
    // on the settings page (search::ask_allowance), never for a search.
    virtual bool tells_allowance() const { return false; }
    virtual Reply allowance(const SearchService &, tools::Context &) const { return Reply(); }
};

// A key told apart from others without being kept: the first 16 hex digits of its SHA-256. The settings mark a
// refused key by it, and the layer and the allowance file know a service set up by its kind and this.
std::string key_print(const std::string &key);
// What the layer knows a service of the user's list by: "kind:print".
std::string id_of(const SearchService &service);

// The services this version knows, in the registry's order, each with the name people call it by.
struct Registered {
    std::unique_ptr<Provider> provider;
    std::string name;
};
const std::vector<Registered> &registry();
const Provider *provider_for(const std::string &kind);   // null for a kind this version does not know
// Adds a service at the end of the registry. The services built into the app are added by
// register_builtin_providers() (providers/registry.cpp) the first time the registry is read; anything
// else is a test's stand-in, added before the first search (testing::clear_providers first).
void register_provider(std::unique_ptr<Provider> provider, const std::string &name);
void register_builtin_providers();

namespace testing {
void clear_providers();   // no service at all: not even those built in, now or later
void set_clock(unsigned long (*clock_ms)());   // the layer's milliseconds (deadlines, rests, allowances)
void set_wall_clock(time_t (*clock)());        // its wall clock (the month, the day, an allowance's end)
// Everything this run remembers of the services, as a restart forgets it: failures and rests, and the allowance file
// as read (read again at the next use; the file itself stays).
void forget();
}  // namespace testing

}  // namespace search
