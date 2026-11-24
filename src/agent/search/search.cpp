// Web search (search.h): which service answers, and what the model reads of its answer.
#include "agent/search/search.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

#include "agent/consent.h"
#include "agent/experiment.h"
#include "agent/search/provider.h"
#include "agent/search/usage.h"
#include "agent/tools/common.h"
#include "smarthome/drivers/support.h"

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
std::atomic<uint64_t> g_learnt{0};   // revision(): what this run has learnt of the services

// Whether the service refused this very key (Settings::search's mark, kept beside the key by its print: another key
// put in its place, by hand or typed, is not refused).
bool refused(const SearchService &service) { return !service.refused.empty() && service.refused == key_print(service.key); }

// The key is never sent again until it is typed again (Settings::set_search_key clears the mark), not even by a later
// run. The settings in memory take the mark at once, for the rest of this run, whether or not the file then can. The
// mark goes only on the key as it was sent: one typed again since (even unchanged, SearchService::typed) is a key to try.
void refuse(const SearchService &sent) {
    const std::string print = key_print(sent.key);
    Settings::update([&](Settings &s) {
        for (SearchService &e : s.search)
            if (e.kind == sent.kind && e.key == sent.key && e.typed == sent.typed) e.refused = print;
    });
}

// The entry that stands for `kind`: the first of that kind in the list (the settings keep one per kind; a second put
// there by other means is passed over by the search, the settings page and the allowance alike).
const SearchService *entry_of(const Settings &s, const std::string &kind) {
    for (const SearchService &e : s.search)
        if (e.kind == kind) return &e;
    return nullptr;
}

// A service set up: the user's entry as it was when last looked at, the provider for its kind, and what this run
// remembers it by.
struct Usable {
    SearchService service;
    const Provider *provider;
    std::string id;
};

// The services set up, in the user's order (configured()): the first entry of each kind, switched on, with a key.
std::vector<Usable> usable(const Settings &s) {
    std::vector<Usable> out;
    std::set<std::string> kinds;
    for (const SearchService &service : s.search) {
        const Provider *p = provider_for(service.kind);
        if (kinds.insert(service.kind).second && p && service.enabled && !service.key.empty()) out.push_back({service, p, id_of(service)});
    }
    return out;
}

// ---------------------------------------------------------------- what the model reads
constexpr size_t kMaxSummary = 1000;   // bytes of the service's own summary

// An address the model may pass on as a source: a web page's, cleaned and capped; "" for any other kind.
std::string source_of(const std::string &url) {
    const std::string u = tools::clean_text(url);
    const std::string scheme = util::lower(u.substr(0, 8));
    if (scheme.rfind("https://", 0) != 0 && scheme.rfind("http://", 0) != 0) return "";
    return util::truncate_utf8(u, 300);
}

// A service's answer as the layer goes on with it: the results that say nothing left out, so that one of only such and
// no summary found nothing (the next service is then asked), and every key the settings hold taken out of its words (a
// service that echoes the key it was sent must not hand it to the model, the cache or the diagnostics).
Reply sifted(Reply reply, const std::vector<std::string> &keys) {
    if (reply.outcome != Outcome::Ok) return reply;
    reply.hits.erase(std::remove_if(reply.hits.begin(), reply.hits.end(), [](const Hit &h) { return !says_something(h); }),
                     reply.hits.end());
    if (util::trim(reply.summary).empty()) reply.summary.clear();
    if (reply.summary.empty() && reply.hits.empty()) reply.outcome = Outcome::Empty;
    reply.summary = util::blank_out(reply.summary, keys);
    for (Hit &h : reply.hits)
        for (std::string *text : {&h.title, &h.url, &h.text}) *text = util::blank_out(*text, keys);
    return reply;
}

// What the model reads, whichever service answered: the summary when there is one, and the first results, each part
// capped, the title cleaned of control characters and only a web page's address as the source.
std::string shaped(const Reply &reply) {
    json out = json::object();
    if (!reply.summary.empty()) out["summary"] = util::truncate_utf8(reply.summary, kMaxSummary);
    json results = json::array();
    for (const Hit &h : reply.hits) {
        if (results.size() >= kMaxHits) break;
        results.push_back({{"title", util::truncate_utf8(tools::clean_text(h.title), 200)}, {"source", source_of(h.url)},
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
uint64_t g_cache_revision = 0;     // the settings' services when it was filled (Settings::search_revision)

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

// The answer kept for this query, while it is fresh and the settings' services are as they were when it was kept
// (`revision`, the search's own reading of them): any change since, even one taken back, starts the cache again.
std::optional<std::string> cached(const std::string &query, uint64_t revision) {
    const std::string key = cache_key(query);
    const unsigned long now = now_ms();
    std::lock_guard<std::mutex> lk(g_cache_mtx);
    if (g_cache_revision != revision) {
        g_cache.clear();
        g_cache_revision = revision;
    }
    g_cache.erase(std::remove_if(g_cache.begin(), g_cache.end(), [now](const Cached &c) { return now - c.at >= kCacheMs; }),
                  g_cache.end());
    for (const Cached &c : g_cache)
        if (c.query == key) return c.result;
    return std::nullopt;
}

// Keeps an answer made under the settings' services of `revision`, unless they changed while it was made.
void keep(const std::string &query, const std::string &result, uint64_t revision) {
    const uint64_t now = Settings::get().search_revision;
    std::lock_guard<std::mutex> lk(g_cache_mtx);
    if (revision != now || revision != g_cache_revision) return;
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
    Search(const std::string &query, tools::Context &ctx, const Settings &s)
        : query_(query), ctx_(ctx), keys_(s.secret_values()), revision_(s.search_revision), start_(now_ms()) {}
    std::string run(const std::vector<Usable> &services);

private:
    std::vector<Usable> order(const std::vector<Usable> &services);
    bool still_set_up(Usable &u);
    tools::Fetch budget(bool more_after, bool others) const;
    void note(const Usable &u, const Reply &reply);
    std::string answered(const Reply &reply) const;
    std::string failed() const;
    long left_ms() const { return kWholeMs - static_cast<long>(now_ms() - start_); }
    bool cancelled() const { return ctx_.cancel && ctx_.cancel->load(); }

    const std::string &query_;
    tools::Context &ctx_;
    const std::vector<std::string> keys_;   // every key the settings hold, taken out of what a service says
    const uint64_t revision_;               // the settings' services the search began under (the cache's)
    const unsigned long start_;
    int sent_ = 0;       // services sent the query
    int refusals_ = 0;   // services passed over or failed because their key was refused
    int others_ = 0;     // and for any other reason (spent, no answer, the service's trouble)
};

std::string Search::run(const std::vector<Usable> &services) {
    std::optional<Reply> nothing;   // the first answer that found nothing: another service is asked once
    const std::vector<Usable> list = order(services);
    for (size_t i = 0; i < list.size() && left_ms() >= kLeastMs && !cancelled(); ++i) {
        Usable u = list[i];
        if (!still_set_up(u)) continue;
        const tools::Fetch how = budget(i + 1 < list.size(), list.size() > 1);
        if (sent_++ == 0) usage::sent();   // a search of the day's is one that sends something
        const Reply reply = sifted(u.provider->run(query_, u.service, how, ctx_), keys_);
        if (cancelled()) return result::fail("cancelled");   // never the answer's words: it may quote the key
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
        if (refused(u.service)) { ++refusals_; continue; }
        if (usage::spent(u.id)) { ++others_; continue; }
        std::lock_guard<std::mutex> lk(g_health_mtx);
        (now < g_health[u.id].resting_until ? resting : ready).push_back(u);
    }
    ready.insert(ready.end(), resting.begin(), resting.end());
    return ready;
}

// Whether a service may still be sent, just before it is: while another was asked, the settings may have changed (its
// key refused by the settings page's lookup, the service switched off, another key put in, the entry removed) and
// another search may have found its allowance spent. Its entry as it stands then is what a refusal is marked against.
bool Search::still_set_up(Usable &u) {
    const Settings now = Settings::get();
    const SearchService *e = entry_of(now, u.service.kind);
    if (!e || !e->enabled || e->key != u.service.key) return false;
    if (refused(*e)) { ++refusals_; return false; }
    if (usage::spent(u.id)) { ++others_; return false; }
    u.service = *e;
    return true;
}

// How long the next service may take: the first sent, when another could follow, 5 s of the 8; any other, what is
// left. Each is sent once when another service could answer instead (`others`: a resend costs the user's wait, and the
// next service is the better try); one that is the only one may be sent twice, as any read is.
tools::Fetch Search::budget(bool more_after, bool others) const {
    tools::Fetch how;
    how.deadline_ms = left_ms();
    if (sent_ == 0 && more_after) how.deadline_ms = std::min(how.deadline_ms, kFirstMs);
    how.attempts = others ? 1 : 2;
    return how;
}

// What a reply says of the service: its key refused, what it said of its allowance and the search counted (usage.h),
// or its health (two failures in a row and it rests for two minutes; an answer ends the count).
void Search::note(const Usable &u, const Reply &reply) {
    ++g_learnt;
    if (reply.outcome == Outcome::KeyRefused) return refuse(u.service);
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

// An answer with results is kept for the same query for ten minutes; one that found nothing, or only a summary, is not.
std::string Search::answered(const Reply &reply) const {
    consent::saw_untrusted_text();   // a web page's words may now steer the model: changes and memories need a yes
    const std::string out = shaped(reply);
    if (!reply.hits.empty()) keep(query_, out, revision_);
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

// Why a service did not say what is left, in the app's own words: never the service's (its answer may quote the key).
std::string reason(const Reply &reply) {
    switch (reply.outcome) {
        case Outcome::Unreachable: return "it could not be reached";
        case Outcome::Slow: return "it did not answer in time";
        case Outcome::Failing:
            return "it answered with an error" + (reply.status ? " (HTTP " + std::to_string(reply.status) + ")" : std::string());
        case Outcome::Spent: return "it says the allowance is used up";
        default: return "its answer could not be read";
    }
}

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

bool says_something(const Hit &hit) { return !tools::clean_text(hit.title).empty() || !util::trim(hit.text).empty(); }

json send(const std::string &address, tools::Fetch how, tools::Context &ctx, tools::Fetched &got, std::string &err) {
    constexpr size_t kMaxReplyBytes = 512 * 1024;   // decompressed: a service's answer is a few kilobytes
    how.no_redirect = true;
    how.max_bytes = kMaxReplyBytes;
    return tools::fetch_json(experiment::web(address), err, ctx, how, &got);
}

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
    if (got.status == 401) return Outcome::KeyRefused;   // whatever then happened to its body
    if (got.status == 402) return Outcome::Spent;
    if (got.status >= 300) return Outcome::Failing;
    if (got.timed_out) return got.sent ? Outcome::Slow : Outcome::Unreachable;
    return Outcome::Unreachable;   // no answer, or a success cut off on the way
}

bool configured(const Settings &s) { return !usable(s).empty(); }

uint64_t revision() { return g_learnt; }

std::string run(const std::string &query, tools::Context &ctx) {
    const Settings s = Settings::get();
    const std::vector<Usable> services = usable(s);
    if (services.empty())
        return failure("web search is not set up on this device (a search key can be added in settings)");
    if (const std::optional<std::string> kept = cached(query, s.search_revision)) {
        consent::saw_untrusted_text();   // the model reads a web page's words again
        return *kept;
    }
    if (usage::today() >= usage::kDailyCap)
        return failure("web search has reached this device's limit of " + std::to_string(usage::kDailyCap) +
                       " searches a day (it starts again tomorrow)");
    const std::string out = Search(query, ctx, s).run(services);
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
    const SearchService *entry = entry_of(s, kind);
    const Provider *p = provider_for(kind);
    if (!p || !p->tells_allowance()) return "This service says what is left only with its searches.";
    if (!entry || entry->key.empty()) return "Type a key first.";
    if (refused(*entry)) return "The service refused this key, so it is not sent again. Type it again first.";
    const std::string id = id_of(*entry);
    {
        std::lock_guard<std::mutex> lk(g_asked_mtx);
        const auto last = g_asked.find(id);
        if (last != g_asked.end() && now_ms() - last->second < kAskEveryMs) return "Asked a moment ago: try again in a minute.";
        g_asked[id] = now_ms();
    }
    const uint64_t since = usage::revision(id);   // what a search learns meanwhile is newer than this answer
    const Reply reply = p->allowance(*entry, ctx);
    ++g_learnt;
    if (reply.outcome == Outcome::KeyRefused) {
        refuse(*entry);
        return "The service refused this key, so it is not sent again. Type it again, or a new one.";
    }
    if (reply.outcome != Outcome::Ok || reply.left < 0) return "The service did not say: " + reason(reply) + ".";
    usage::said(id, reply, since);
    usage::save();
    return "";
}

}  // namespace search
