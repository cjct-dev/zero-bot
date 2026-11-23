// Web search (search.h): which service answers, and what the model reads of its answer.
#include "agent/search/search.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "agent/search/provider.h"
#include "agent/tools/common.h"
#include "platform/sys.h"
#include "smarthome/drivers/support.h"
#include "smarthome/home.h"

namespace search {

namespace {

// ---------------------------------------------------------------- the registry
// Filled before the first search (the services built in, or a test's), then only read.
std::vector<Registered> g_registry;
std::once_flag g_builtin_once;
bool g_skip_builtin = false;

void ensure_builtin() {
    std::call_once(g_builtin_once, [] {
        if (!g_skip_builtin) register_builtin_providers();
    });
}

// ---------------------------------------------------------------- time and health
constexpr long kWholeMs = 8000;    // a whole search, every service it asks included
constexpr long kFirstMs = 5000;    // the first service asked, when another could follow
constexpr long kLeastMs = 1000;    // no service is asked with less than this left
constexpr int kRestAfter = 2;      // failures in a row after which a service rests
constexpr unsigned long kRestMs = 2 * 60 * 1000UL;
// A service whose allowance is spent and that did not say when it comes back is asked again after this (a refusal
// costs no allowance, only the time it takes; the plan may come back on any day of the month).
constexpr unsigned long kSpentForMs = 6 * 3600 * 1000UL;
constexpr long kLongestResetS = 40 * 86400L;   // a service's own reset time is believed up to this

// What this run has seen of one service set up (an entry of the user's list: its kind and key), in memory only.
struct Health {
    int failures = 0;                  // in a row: no answer, too slow, the service's own trouble, a garbled answer
    unsigned long resting_until = 0;   // until then asked only when no other service can be
    unsigned long spent_until = 0;     // until then not asked: its allowance is used up
};
std::mutex g_health_mtx;   // guards g_health
std::map<std::string, Health> g_health;
unsigned long (*g_test_clock)() = nullptr;

unsigned long now_ms() { return g_test_clock ? g_test_clock() : sys::now_ms(); }

// A key told apart from others without being kept: the first 16 hex digits of its SHA-256.
std::string print_of(const std::string &key) { return support::hex(support::digest(EVP_sha256(), key)).substr(0, 16); }

// Whether the service refused this very key (Settings::search's mark, kept beside the key by its print: another key
// put in its place, by hand or typed, is not refused).
bool refused(const SearchService &service) { return !service.refused.empty() && service.refused == print_of(service.key); }

// The key is never sent again until it is typed again (Settings::set_search_key clears the mark), not even by a later
// run. The settings in memory take the mark at once, for the rest of this run, whether or not the file then can.
void refuse(const SearchService &service) {
    const std::string kind = service.kind, key = service.key, print = print_of(key);
    Settings::update([&](Settings &s) {
        for (SearchService &e : s.search)
            if (e.kind == kind && e.key == key) e.refused = print;
    });
}

// A service set up: the user's entry, the provider for its kind, and what this run remembers it by.
struct Usable {
    const SearchService *service;
    const Provider *provider;
    std::string id;
};

// The services set up, in the user's order (configured()).
std::vector<Usable> usable(const Settings &s) {
    std::vector<Usable> out;
    for (const SearchService &service : s.search)
        if (const Provider *p = provider_for(service.kind); p && service.enabled && !service.key.empty())
            out.push_back({&service, p, service.kind + ":" + print_of(service.key)});
    return out;
}

// ---------------------------------------------------------------- what the model reads
constexpr size_t kMaxResults = 4;      // the results the model reads
constexpr size_t kMaxSummary = 1000;   // bytes of the service's own summary

// An address the model may pass on as a source: a web page's, cleaned and capped; "" for any other kind.
std::string source_of(const std::string &url) {
    const std::string u = tools::clean_text(url);
    const std::string scheme = util::lower(u.substr(0, 8));
    if (scheme.rfind("https://", 0) != 0 && scheme.rfind("http://", 0) != 0) return "";
    return util::truncate_utf8(u, 300);
}

// What the model reads, whichever service answered: the summary when there is one, and the first results,
// each part capped, the title cleaned of control characters and only a web page's address as the source.
std::string shaped(const Reply &reply) {
    json out = json::object();
    if (!reply.summary.empty()) out["summary"] = util::truncate_utf8(reply.summary, kMaxSummary);
    json results = json::array();
    for (const Hit &h : reply.hits) {
        if (results.size() >= kMaxResults) break;
        results.push_back({{"title", util::truncate_utf8(tools::clean_text(h.title), 200)},
                           {"source", source_of(h.url)},
                           {"text", util::truncate_utf8(h.text, 500)}});
    }
    out["results"] = results;
    return out.dump(-1, ' ', false, json::error_handler_t::replace);
}

// ---------------------------------------------------------------- one search
// The services set up, asked in the user's order until one answers, inside one deadline. A service is passed over
// while its key stands refused or its allowance spent; one resting after failures is asked only after every other.
class Search {
public:
    Search(const std::string &query, tools::Context &ctx) : query_(query), ctx_(ctx), start_(now_ms()) {}
    std::string run(const std::vector<Usable> &services);

private:
    std::vector<Usable> order(const std::vector<Usable> &services);
    tools::Fetch budget(bool another_follows) const;
    void note(const Usable &u, const Reply &reply);
    std::string answered(const Reply &reply) const;
    std::string failed() const;
    long left_ms() const { return kWholeMs - static_cast<long>(now_ms() - start_); }
    bool cancelled() const { return ctx_.cancel && ctx_.cancel->load(); }

    const std::string &query_;
    tools::Context &ctx_;
    const unsigned long start_;
    int asked_ = 0;
    int refusals_ = 0;   // services passed over or failed because their key was refused
    int others_ = 0;     // and for any other reason (spent, no answer, the service's trouble)
};

std::string Search::run(const std::vector<Usable> &services) {
    std::optional<Reply> nothing;   // the first answer that found nothing: another service is asked once
    const std::vector<Usable> list = order(services);
    for (size_t i = 0; i < list.size() && left_ms() >= kLeastMs && !cancelled(); ++i) {
        const Usable &u = list[i];
        const Reply reply = u.provider->run(query_, *u.service, budget(i + 1 < list.size()), ctx_);
        ++asked_;
        if (cancelled()) return result::fail(reply.error.empty() ? "cancelled" : reply.error);
        note(u, reply);
        if (reply.outcome == Outcome::Ok) return answered(reply);
        if (nothing) break;
        if (reply.outcome == Outcome::Empty) nothing = reply;
        else if (reply.outcome == Outcome::KeyRefused) ++refusals_;
        else ++others_;
    }
    return nothing ? answered(*nothing) : failed();
}

// The services that may be asked now, in the user's order, those resting after the others: the order never changes
// by itself, and a rest never makes a search fail on its own.
std::vector<Usable> Search::order(const std::vector<Usable> &services) {
    std::vector<Usable> ready, resting;
    const unsigned long now = now_ms();
    std::lock_guard<std::mutex> lk(g_health_mtx);
    for (const Usable &u : services) {
        const Health &h = g_health[u.id];
        if (refused(*u.service)) ++refusals_;
        else if (now < h.spent_until) ++others_;
        else (now < h.resting_until ? resting : ready).push_back(u);
    }
    ready.insert(ready.end(), resting.begin(), resting.end());
    return ready;
}

// How long the next service may take: the first asked, when another could follow, 5 s of the 8; any other, what is
// left. It is sent once when another could answer instead (a resend costs the user's wait), twice when it is the last.
tools::Fetch Search::budget(bool another_follows) const {
    tools::Fetch how;
    how.deadline_ms = left_ms();
    if (another_follows && asked_ == 0) how.deadline_ms = std::min(how.deadline_ms, kFirstMs);
    how.attempts = another_follows ? 1 : 2;
    return how;
}

// What a reply says of the service: its key refused, its allowance spent (until the service says it comes back), or
// its health (two failures in a row and it rests for two minutes; an answer ends the count).
void Search::note(const Usable &u, const Reply &reply) {
    if (reply.outcome == Outcome::KeyRefused) return refuse(*u.service);
    const unsigned long now = now_ms();
    const unsigned long until_reset =
        reply.reset_s > 0 ? static_cast<unsigned long>(std::min(reply.reset_s, kLongestResetS)) * 1000UL : kSpentForMs;
    std::lock_guard<std::mutex> lk(g_health_mtx);
    Health &h = g_health[u.id];
    if (reply.outcome == Outcome::Ok || reply.outcome == Outcome::Empty) {
        h.failures = 0;
        h.resting_until = 0;
        if (reply.none_left) h.spent_until = now + until_reset;
    } else if (reply.outcome == Outcome::Spent) {
        h.spent_until = now + until_reset;
    } else if (++h.failures >= kRestAfter) {
        h.resting_until = now + kRestMs;
    }
}

std::string Search::answered(const Reply &reply) const {
    home::saw_untrusted_text();   // a web page's words may now steer the model: smart-home changes need a yes
    return shaped(reply);
}

// One sentence when no service answered, whichever and however many were set up.
std::string Search::failed() const {
    const char *then = ", so this could not be looked up. Tell the user that; if you answer from memory, say it may be out of date";
    if (refusals_ > 0 && others_ == 0)
        return result::fail(std::string(refusals_ == 1 ? "the search service refused the key set up on this device"
                                                       : "the search services refused the keys set up on this device") +
                            " (a new key can be typed in settings)" + then);
    return result::fail(std::string("web search is not available right now (every search service failed or has used its "
                                    "allowance)") + then);
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

void set_clock(unsigned long (*clock_ms)()) { g_test_clock = clock_ms; }

void forget_health() {
    std::lock_guard<std::mutex> lk(g_health_mtx);
    g_health.clear();
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

std::string run(const std::string &query, tools::Context &ctx) {
    const Settings s = Settings::get();
    const std::vector<Usable> services = usable(s);
    if (services.empty())
        return result::fail("web search is not set up on this device (a search key can be added in settings), so this could "
                            "not be looked up. Tell the user that; if you answer from memory, say it may be out of date");
    return Search(query, ctx).run(services);
}

}  // namespace search
