// Web search (search.h): which service answers, and what the model reads of its answer.
#include "agent/search/search.h"

#include <mutex>
#include <vector>

#include "agent/search/provider.h"
#include "agent/tools/common.h"
#include "smarthome/home.h"

namespace search {

namespace {

// The registry: filled before the first search (the services built in, or a test's), then only read.
std::vector<Registered> g_registry;
std::once_flag g_builtin_once;
bool g_skip_builtin = false;

void ensure_builtin() {
    std::call_once(g_builtin_once, [] {
        if (!g_skip_builtin) register_builtin_providers();
    });
}

constexpr size_t kMaxResults = 4;   // the results the model reads

// A service set up: the user's entry and the provider for its kind.
struct Usable {
    const SearchService *service;
    const Provider *provider;
};

// The services set up, in the user's order (configured()).
std::vector<Usable> usable(const Settings &s) {
    std::vector<Usable> out;
    for (const SearchService &service : s.search)
        if (const Provider *p = provider_for(service.kind); p && service.enabled && !service.key.empty())
            out.push_back({&service, p});
    return out;
}

// What the model reads, whichever service answered: the summary when there is one, and the first results,
// each part capped and the title cleaned of control characters.
std::string shaped(const Reply &reply) {
    json out = json::object();
    if (!reply.summary.empty()) out["summary"] = reply.summary;
    json results = json::array();
    for (const Hit &h : reply.hits) {
        if (results.size() >= kMaxResults) break;
        results.push_back({{"title", util::truncate_utf8(tools::clean_text(h.title), 200)},
                           {"source", util::truncate_utf8(h.url, 300)},
                           {"text", util::truncate_utf8(h.text, 500)}});
    }
    out["results"] = results;
    return out.dump(-1, ' ', false, json::error_handler_t::replace);
}

}  // namespace

const std::vector<Registered> &registry() {
    ensure_builtin();
    return g_registry;
}

const Provider *provider_for(const std::string &kind) {
    for (const Registered &r : registry())
        if (kind == r.provider->kind()) return r.provider.get();
    return nullptr;
}

void register_provider(std::unique_ptr<Provider> provider, const std::string &name) {
    if (provider) g_registry.push_back({std::move(provider), name});
}

namespace testing {

void clear_providers() {
    g_skip_builtin = true;
    ensure_builtin();
    g_registry.clear();
}

}  // namespace testing

Outcome outcome_of(const tools::Fetched &got, const std::string &err) {
    if (err.empty()) return Outcome::Ok;
    if (got.garbled) return Outcome::Garbled;
    if (got.timed_out) return got.sent ? Outcome::Slow : Outcome::Unreachable;
    if (got.status < 300) return Outcome::Unreachable;   // no answer, or a success cut off on the way
    if (got.status == 401 || got.status == 403) return Outcome::KeyRefused;
    if (got.status == 402) return Outcome::Spent;
    return Outcome::Failing;
}

bool configured(const Settings &s) { return !usable(s).empty(); }

// The first service set up is asked; when it fails, the model is told why in fetch_json's words.
std::string run(const std::string &query, tools::Context &ctx) {
    const Settings s = Settings::get();
    const std::vector<Usable> services = usable(s);
    if (services.empty())
        return result::fail("web search is not set up on this device (a search key can be added in settings), so this could "
                            "not be looked up. Tell the user that; if you answer from memory, say it may be out of date");
    const Usable &first = services.front();
    const Reply reply = first.provider->run(query, *first.service, tools::Fetch(), ctx);
    if (reply.outcome != Outcome::Ok && reply.outcome != Outcome::Empty) return result::fail(reply.error);
    home::saw_untrusted_text();   // a web page's words may now steer the model: smart-home changes need a yes
    return shaped(reply);
}

}  // namespace search
