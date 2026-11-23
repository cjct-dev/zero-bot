// Web search (search.h): which service answers, and what the model reads of its answer.
#include "agent/search/search.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "agent/search/provider.h"
#include "agent/search/usage.h"
#include "agent/tools/common.h"
#include "smarthome/drivers/support.h"
#include "smarthome/home.h"

namespace search {

namespace {

using usage::now_ms;

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

// What this run has seen of one service set up (by id_of), in memory only; its allowance is usage.h's.
struct Health {
    int failures = 0;                  // in a row: no answer, too slow, the service's own trouble, a garbled answer
    unsigned long resting_until = 0;   // until then asked only when no other service can be
    bool answered = false;             // its last search was answered
};
std::mutex g_health_mtx;   // guards g_health
std::map<std::string, Health> g_health;

// Whether the service refused this very key (Settings::search's mark, kept beside the key by its print: another key
// put in its place, by hand or typed, is not refused).
bool refused(const SearchService &service) { return !service.refused.empty() && service.refused == key_print(service.key); }

// The key is never sent again until it is typed again (Settings::set_search_key clears the mark), not even by a later
// run. The settings in memory take the mark at once, for the rest of this run, whether or not the file then can.
void refuse(const SearchService &service) {
    const std::string kind = service.kind, key = service.key, print = key_print(key);
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
            out.push_back({&service, p, id_of(service)});
    return out;
}

// ---------------------------------------------------------------- what the model reads
constexpr size_t kMaxResults = 4;      // the results the model reads
constexpr size_t kMaxSummary = 1000;   // bytes of the service's own summary
const char *const kNothingFound = "{\"results\":[]}";

// An address the model may pass on as a source: a web page's, cleaned and capped; "" for any other kind.
std::string source_of(const std::string &url) {
    const std::string u = tools::clean_text(url);
    const std::string scheme = util::lower(u.substr(0, 8));
    if (scheme.rfind("https://", 0) != 0 && scheme.rfind("http://", 0) != 0) return "";
    return util::truncate_utf8(u, 300);
}

// What the model reads, whichever service answered: the summary when there is one, and the first results that say
// anything, each part capped, the title cleaned of control characters and only a web page's address as the source.
std::string shaped(const Reply &reply) {
    json out = json::object();
    if (!reply.summary.empty()) out["summary"] = util::truncate_utf8(reply.summary, kMaxSummary);
    json results = json::array();
    for (const Hit &h : reply.hits) {
        if (results.size() >= kMaxResults) break;
        const std::string title = tools::clean_text(h.title);
        if (title.empty() && util::trim(h.text).empty()) continue;
        results.push_back({{"title", util::truncate_utf8(title, 200)}, {"source", source_of(h.url)},
                           {"text", util::truncate_utf8(h.text, 500)}});
    }
    out["results"] = results;
    return out.dump(-1, ' ', false, json::error_handler_t::replace);
}

// One sentence for the model, after what went wrong: what it is to do about it.
std::string failure(const std::string &what) {
    return result::fail(what + ", so this could not be looked up. Tell the user that; if you answer from memory, say it may be out of date");
}

// ---------------------------------------------------------------- the cache
constexpr size_t kCacheSize = 32;
constexpr unsigned long kCacheMs = 10 * 60 * 1000UL;

struct Cached {
    std::string query;     // as cache_key gives it
    std::string result;    // what the model read
    unsigned long at;
};
std::mutex g_cache_mtx;            // guards the two below
std::vector<Cached> g_cache;       // the oldest first
std::string g_cache_services;      // the services set up when it was filled (their ids, in order)

// A query as the cache knows it: in small letters, with its words one space apart.
std::string cache_key(const std::string &query) {
    std::string out;
    bool gap = false;
    for (const char c : util::lower(query)) {
        if (isspace(static_cast<unsigned char>(c))) { gap = !out.empty(); continue; }
        if (gap) out += ' ';
        gap = false;
        out += c;
    }
    return out;
}

std::string services_key(const std::vector<Usable> &services) {
    std::string out;
    for (const Usable &u : services) out += u.id + ";";
    return out;
}

// The answer kept for this query, while it is fresh and the services set up are the ones that gave it.
std::optional<std::string> cached(const std::string &query, const std::vector<Usable> &services) {
    const std::string key = cache_key(query), set_up = services_key(services);
    const unsigned long now = now_ms();
    std::lock_guard<std::mutex> lk(g_cache_mtx);
    if (g_cache_services != set_up) {
        g_cache.clear();
        g_cache_services = set_up;
    }
    g_cache.erase(std::remove_if(g_cache.begin(), g_cache.end(), [now](const Cached &c) { return now - c.at >= kCacheMs; }),
                  g_cache.end());
    for (const Cached &c : g_cache)
        if (c.query == key) return c.result;
    return std::nullopt;
}

void keep(const std::string &query, const std::string &result) {
    std::lock_guard<std::mutex> lk(g_cache_mtx);
    const std::string key = cache_key(query);
    g_cache.erase(std::remove_if(g_cache.begin(), g_cache.end(), [&key](const Cached &c) { return c.query == key; }), g_cache.end());
    if (g_cache.size() >= kCacheSize) g_cache.erase(g_cache.begin());
    g_cache.push_back({key, result, now_ms()});
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
    tools::Fetch budget(size_t i, size_t count) const;
    void note(const Usable &u, const Reply &reply);
    std::string answered(const Reply &reply) const;
    std::string failed() const;
    long left_ms() const { return kWholeMs - static_cast<long>(now_ms() - start_); }
    bool cancelled() const { return ctx_.cancel && ctx_.cancel->load(); }

    const std::string &query_;
    tools::Context &ctx_;
    const unsigned long start_;
    int refusals_ = 0;   // services passed over or failed because their key was refused
    int others_ = 0;     // and for any other reason (spent, no answer, the service's trouble)
};

std::string Search::run(const std::vector<Usable> &services) {
    std::optional<Reply> nothing;   // the first answer that found nothing: another service is asked once
    const std::vector<Usable> list = order(services);
    if (!list.empty()) usage::sent();
    for (size_t i = 0; i < list.size() && left_ms() >= kLeastMs && !cancelled(); ++i) {
        const Usable &u = list[i];
        const Reply reply = u.provider->run(query_, *u.service, budget(i, list.size()), ctx_);
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
    for (const Usable &u : services) {
        if (refused(*u.service)) { ++refusals_; continue; }
        if (usage::spent(u.id)) { ++others_; continue; }
        std::lock_guard<std::mutex> lk(g_health_mtx);
        (now < g_health[u.id].resting_until ? resting : ready).push_back(u);
    }
    ready.insert(ready.end(), resting.begin(), resting.end());
    return ready;
}

// How long the next of `count` services that may be asked may take: the first, when another could follow, 5 s of the
// 8; any other, what is left. Each is sent once when another service could answer instead (a resend costs the user's
// wait, and the next service is the better try); one that is the only one may be sent twice, as any read is.
tools::Fetch Search::budget(size_t i, size_t count) const {
    tools::Fetch how;
    how.deadline_ms = left_ms();
    if (i == 0 && count > 1) how.deadline_ms = std::min(how.deadline_ms, kFirstMs);
    how.attempts = count > 1 ? 1 : 2;
    return how;
}

// What a reply says of the service: its key refused, what it said of its allowance and the search counted (usage.h),
// or its health (two failures in a row and it rests for two minutes; an answer ends the count).
void Search::note(const Usable &u, const Reply &reply) {
    if (reply.outcome == Outcome::KeyRefused) return refuse(*u.service);
    usage::note(u.id, reply);
    if (reply.outcome == Outcome::Spent) return;
    const bool ok = reply.outcome == Outcome::Ok || reply.outcome == Outcome::Empty;
    std::lock_guard<std::mutex> lk(g_health_mtx);
    Health &h = g_health[u.id];
    h.answered = ok;
    if (ok) {
        h.failures = 0;
        h.resting_until = 0;
    } else if (++h.failures >= kRestAfter) {
        h.resting_until = now_ms() + kRestMs;
    }
}

// An answer is kept for the same query for ten minutes, unless it found nothing.
std::string Search::answered(const Reply &reply) const {
    home::saw_untrusted_text();   // a web page's words may now steer the model: smart-home changes need a yes
    const std::string out = shaped(reply);
    if (reply.outcome == Outcome::Ok && out != kNothingFound) keep(query_, out);
    return out;
}

// One sentence when no service answered, whichever and however many were set up.
std::string Search::failed() const {
    if (refusals_ > 0 && others_ == 0)
        return failure(std::string(refusals_ == 1 ? "the search service refused the key set up on this device"
                                                  : "the search services refused the keys set up on this device") +
                       " (a new key can be typed in settings)");
    return failure("web search is not available right now (every search service failed or has used its allowance)");
}

// ---------------------------------------------------------------- the settings page
constexpr unsigned long kAskEveryMs = 60 * 1000UL;   // Tavily answers GET /usage at most ten times in ten minutes
std::mutex g_asked_mtx;                               // guards g_asked
std::map<std::string, unsigned long> g_asked;         // when each service (id_of) was last asked what is left

// The end of a spent allowance in words: its day when it is a day or more away, else its time.
std::string until_words(time_t until) {
    if (until <= 0) return "allowance used up";
    const bool far = until - usage::wall_now() >= 20 * 3600;
    return "allowance used until " + util::format_time(until, far ? "%-d %B" : "%H:%M");
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

std::string key_print(const std::string &key) { return support::hex(support::digest(EVP_sha256(), key)).substr(0, 16); }

std::string id_of(const SearchService &service) { return service.kind + ":" + key_print(service.key); }

namespace testing {

void clear_providers() {
    g_skip_builtin = true;
    ensure_builtin();
    g_registry.clear();
}

void forget() {
    {
        std::lock_guard<std::mutex> lk(g_health_mtx);
        g_health.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_cache_mtx);
        g_cache.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_asked_mtx);
        g_asked.clear();
    }
    usage::forget();
}

}  // namespace testing

Outcome outcome_of(const tools::Fetched &got, const std::string &err) {
    if (err.empty()) return Outcome::Ok;
    if (got.garbled) return Outcome::Garbled;
    if (got.timed_out) return got.sent ? Outcome::Slow : Outcome::Unreachable;
    if (got.status < 300) return Outcome::Unreachable;   // no answer, or a success cut off on the way
    if (got.status == 401) return Outcome::KeyRefused;
    if (got.status == 402) return Outcome::Spent;
    return Outcome::Failing;
}

bool configured(const Settings &s) { return !usable(s).empty(); }

std::string run(const std::string &query, tools::Context &ctx) {
    const Settings s = Settings::get();
    const std::vector<Usable> services = usable(s);
    if (services.empty())
        return failure("web search is not set up on this device (a search key can be added in settings)");
    if (const std::optional<std::string> kept = cached(query, services)) {
        home::saw_untrusted_text();   // the model reads a web page's words again
        return *kept;
    }
    if (usage::today() >= usage::kDailyCap)
        return failure("web search has reached this device's limit of " + std::to_string(usage::kDailyCap) +
                       " searches a day (it starts again tomorrow)");
    const std::string out = Search(query, ctx).run(services);
    usage::save();
    return out;
}

State state_of(const SearchService &service) {
    if (!service.enabled) return State::Off;
    if (service.key.empty()) return State::NoKey;
    if (refused(service)) return State::KeyRefused;
    const std::string id = id_of(service);
    if (usage::spent(id)) return State::Spent;
    std::lock_guard<std::mutex> lk(g_health_mtx);
    const auto h = g_health.find(id);
    if (h == g_health.end()) return State::Ready;
    if (now_ms() < h->second.resting_until) return State::Resting;
    if (h->second.failures > 0) return State::FailedLast;
    return h->second.answered ? State::Working : State::Ready;
}

std::string describe(const SearchService &service) {
    switch (state_of(service)) {
        case State::Off: return "off";
        case State::NoKey: return "no key";
        case State::KeyRefused: return "key refused: type it again";
        case State::Spent: return until_words(usage::of(id_of(service)).spent_until);
        case State::Resting: return "resting after failures";
        case State::FailedLast: return "failed last time";
        case State::Working: return "working";
        case State::Ready: break;
    }
    return "ready";
}

std::string ask_allowance(const std::string &kind, tools::Context &ctx) {
    const Settings s = Settings::get();
    const auto entry = std::find_if(s.search.begin(), s.search.end(), [&](const SearchService &e) { return e.kind == kind; });
    const Provider *p = provider_for(kind);
    if (!p || !p->tells_allowance()) return "This service says what is left only with its searches.";
    if (entry == s.search.end() || entry->key.empty()) return "Type a key first.";
    if (refused(*entry)) return "The service refused this key, so it is not sent again. Type it again first.";
    const std::string id = id_of(*entry);
    {
        std::lock_guard<std::mutex> lk(g_asked_mtx);
        const auto last = g_asked.find(id);
        if (last != g_asked.end() && now_ms() - last->second < kAskEveryMs) return "Asked a moment ago: try again in a minute.";
        g_asked[id] = now_ms();
    }
    const Reply reply = p->allowance(*entry, ctx);
    if (reply.outcome == Outcome::KeyRefused) {
        refuse(*entry);
        return "The service refused this key, so it is not sent again. Type it again, or a new one.";
    }
    if (reply.outcome != Outcome::Ok || reply.left < 0)
        return "The service did not say: " + (reply.error.empty() ? std::string("its answer could not be read") : reply.error) + ".";
    usage::said(id, reply);
    usage::save();
    return "";
}

}  // namespace search
