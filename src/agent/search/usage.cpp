// Web search's allowance (usage.h): the file, the counts and their periods, spent allowances.
#include "agent/search/usage.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>

#include "core/settings.h"
#include "core/util.h"
#include "platform/sys.h"

namespace search {

namespace {
unsigned long (*g_test_clock)() = nullptr;
time_t (*g_test_wall)() = nullptr;
}  // namespace

namespace testing {
void set_clock(unsigned long (*clock_ms)()) { g_test_clock = clock_ms; }
void set_wall_clock(time_t (*clock)()) { g_test_wall = clock; }
}  // namespace testing

namespace usage {

namespace {

constexpr time_t kClockSetFrom = 1767225600;               // 1 January 2026: a clock that reads earlier has not been set
constexpr unsigned long kSpentForMs = 6 * 3600 * 1000UL;   // a spent allowance whose service did not say when it comes back
constexpr long kLongestResetS = 40 * 86400L;               // a service's own reset time is believed up to this
constexpr time_t kPutRightS = 2 * 86400;                   // a step back further than this is a clock put right

bool clock_set(time_t t) { return t >= kClockSetFrom; }

struct Entry {
    std::string month;            // the month `used` counts in, "2026-11" ("" while the clock has never been set)
    long used = 0;
    long left = -1, limit = -1;
    time_t said_at = 0;
    unsigned long spent_ms = 0;   // spent until then, on the monotonic clock (0: not spent)
    time_t spent_wall = 0;        // spent until then, as the file had it: put on the monotonic clock once the wall clock is set
    uint64_t revision = 0;        // what this run has learnt of it (note, said), in memory only
};

struct State {
    bool loaded = false;
    std::string day;              // the day `today` counts in, "2026-11-23"
    int today = 0;
    time_t latest = 0;            // the latest time the clock has said since it was set (0: none): the counts' period
    std::map<std::string, Entry> services;   // by id_of
    bool changed = false;         // since the last save
    uint64_t version = 0;         // counts changes saved, so that an older save never overwrites a newer one
};

std::mutex g_mtx;        // guards g_state
State g_state;
std::mutex g_file_mtx;   // one save at a time; guards g_written
uint64_t g_written = 0;  // the version on disk

std::string path() { return util::data_dir() + "/search_usage.json"; }

void load_locked() {
    using util::jget;
    g_state = State();
    g_state.loaded = true;
    const json j = util::load_json(path(), json::object());
    if (!j.is_object()) return;
    g_state.day = jget(j, "day", std::string());
    g_state.today = std::max(0, jget(j, "today", 0));
    g_state.latest = static_cast<time_t>(jget(j, "latest", 0LL));
    const auto services = j.find("services");
    if (services == j.end() || !services->is_object()) return;
    for (const auto &[id, e] : services->items()) {
        if (!e.is_object()) continue;
        Entry &x = g_state.services[id];
        x.month = jget(e, "month", std::string());
        x.used = std::max(0L, jget(e, "used", 0L));
        x.left = jget(e, "left", -1L);
        x.limit = jget(e, "limit", -1L);
        x.said_at = static_cast<time_t>(jget(e, "said_at", 0LL));
        x.spent_wall = static_cast<time_t>(jget(e, "spent_until", 0LL));
        // An end learnt while the clock was not set: the time it still had then, counted from this run's start.
        const long spent_for = std::min(kLongestResetS, jget(e, "spent_for", 0L));
        if (!x.spent_wall && spent_for > 0) x.spent_ms = now_ms() + static_cast<unsigned long>(spent_for) * 1000UL;
    }
}

// The month and day the counts are in now: those of the latest time the clock has said, so that a clock that steps
// back leaves them where they are, unless it steps back more than two days (put right after running ahead: a date set
// wrong by hand, a time sync gone wrong), when the counts follow it rather than stand still until the wrong date
// comes round. Nothing while the clock is not set.
bool period_now(std::string &month, std::string &day) {
    const time_t now = wall_now();
    if (!clock_set(now)) return false;
    if (now > g_state.latest || g_state.latest - now > kPutRightS) g_state.latest = now;
    day = util::format_time(g_state.latest, "%Y-%m-%d");
    month = day.substr(0, 7);
    return true;
}

// A count moved on to the period the clock says now: started again in a new one; one counted before the clock was
// ever set belongs to the first period it reads.
template <typename Count>
void roll(std::string &period, Count &count, const std::string &now) {
    if (period == now) return;
    if (!period.empty()) count = 0;
    period = now;
    g_state.changed = true;
}

// The state, read from the file at its first use, its counts moved on to the month and day the clock says now.
State &state_locked() {
    if (!g_state.loaded) load_locked();
    std::string month, day;
    if (!period_now(month, day)) return g_state;
    roll(g_state.day, g_state.today, day);
    for (auto &[id, e] : g_state.services) roll(e.month, e.used, month);
    return g_state;
}

Entry &entry_locked(const std::string &id) {
    state_locked();
    Entry &e = g_state.services[id];
    std::string month, day;
    if (e.month.empty() && period_now(month, day)) e.month = month;
    return e;
}

// Whether the allowance is used up at `now` (the monotonic clock, read once by the caller for all it works out). An end
// the file held is put on this run's clock once the wall clock can say how far away it is; until then it stands.
bool spent_locked(Entry &e, unsigned long now) {
    if (e.spent_wall) {
        const time_t wall = wall_now();
        if (!clock_set(wall)) return true;
        const long ahead = std::min<long>(kLongestResetS, static_cast<long>(e.spent_wall - wall));
        e.spent_ms = ahead > 0 ? now + static_cast<unsigned long>(ahead) * 1000UL : 0;
        e.spent_wall = 0;
    }
    if (e.spent_ms && now >= e.spent_ms) {
        e.spent_ms = 0;
        g_state.changed = true;
    }
    return e.spent_ms != 0;
}

// Whole seconds, rounded up, until a spent allowance on this run's clock comes back; 0 when it is not spent at `now`.
long spent_left_s(const Entry &e, unsigned long now) {
    return e.spent_ms > now ? static_cast<long>((e.spent_ms - now + 999) / 1000) : 0;
}

// The end of a spent allowance on the wall clock at `now`: what the file said while the clock is not set; 0 when not
// spent, or when it was learnt in this run while the clock is not set.
time_t spent_until_locked(Entry &e, unsigned long now) {
    if (!spent_locked(e, now)) return 0;
    if (e.spent_wall) return e.spent_wall;
    const time_t wall = wall_now();
    return clock_set(wall) ? wall + spent_left_s(e, now) : 0;
}

void mark_spent_locked(Entry &e, long reset_s) {
    e.spent_ms = now_ms() + (reset_s > 0 ? static_cast<unsigned long>(std::min(reset_s, kLongestResetS)) * 1000UL : kSpentForMs);
    e.spent_wall = 0;
}

void keep_figure_locked(Entry &e, const Reply &reply) {
    if (reply.left < 0) return;
    e.left = reply.left;
    e.limit = reply.limit;
    const time_t wall = wall_now();
    e.said_at = clock_set(wall) ? wall : 0;
}

// The file: a spent allowance's end as a wall time when the clock can say it, else as the time it still has to run.
json to_json_locked() {
    const unsigned long now = now_ms();
    json services = json::object();
    for (auto &[id, e] : g_state.services) {
        const time_t until = spent_until_locked(e, now);
        services[id] = {{"month", e.month}, {"used", e.used}, {"left", e.left}, {"limit", e.limit},
                        {"said_at", static_cast<long long>(e.said_at)}, {"spent_until", static_cast<long long>(until)},
                        {"spent_for", until ? 0L : spent_left_s(e, now)}};
    }
    return {{"version", 1}, {"day", g_state.day}, {"today", g_state.today}, {"latest", static_cast<long long>(g_state.latest)},
            {"services", services}};
}

void write(const json &j, uint64_t version) {
    std::lock_guard<std::mutex> lk(g_file_mtx);
    if (version <= g_written) return;
    if (util::save_json(path(), j)) g_written = version;
}

}  // namespace

unsigned long now_ms() { return g_test_clock ? g_test_clock() : sys::now_ms(); }
time_t wall_now() { return g_test_wall ? g_test_wall() : time(nullptr); }

Figures of(const std::string &id) {
    std::lock_guard<std::mutex> lk(g_mtx);
    Entry &e = entry_locked(id);
    const unsigned long now = now_ms();
    Figures f;
    f.used = e.used;
    f.left = e.left;
    f.limit = e.limit;
    f.said_at = e.said_at;
    f.spent = spent_locked(e, now);
    f.spent_until = spent_until_locked(e, now);
    return f;
}

bool spent(const std::string &id) {
    std::lock_guard<std::mutex> lk(g_mtx);
    return spent_locked(entry_locked(id), now_ms());
}

void note(const std::string &id, const Reply &reply) {
    std::lock_guard<std::mutex> lk(g_mtx);
    Entry &e = entry_locked(id);
    const bool answered = reply.outcome == Outcome::Ok || reply.outcome == Outcome::Empty;
    if (answered) {
        ++e.used;
        e.spent_ms = 0;   // it answered: whatever was thought of its allowance, there was some
        e.spent_wall = 0;
    }
    keep_figure_locked(e, reply);
    if (reply.outcome == Outcome::Spent || (answered && reply.left == 0)) mark_spent_locked(e, reply.reset_s);
    ++e.revision;
    g_state.changed = true;
}

void said(const std::string &id, const Reply &reply, uint64_t since) {
    if (reply.outcome != Outcome::Ok || reply.left < 0) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    Entry &e = entry_locked(id);
    if (e.revision != since) return;
    ++e.revision;
    keep_figure_locked(e, reply);
    if (reply.left == 0) {
        mark_spent_locked(e, reply.reset_s);
    } else {
        e.spent_ms = 0;
        e.spent_wall = 0;
    }
    g_state.changed = true;
}

uint64_t revision(const std::string &id) {
    std::lock_guard<std::mutex> lk(g_mtx);
    return entry_locked(id).revision;
}

int today() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return state_locked().today;
}

void sent() {
    std::lock_guard<std::mutex> lk(g_mtx);
    ++state_locked().today;
    g_state.changed = true;
}

void save() {
    std::set<std::string> listed;
    for (const SearchService &s : Settings::get().search) listed.insert(id_of(s));
    json j;
    uint64_t version;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        state_locked();
        for (auto it = g_state.services.begin(); it != g_state.services.end();) {
            if (listed.count(it->first)) { ++it; continue; }
            it = g_state.services.erase(it);
            g_state.changed = true;
        }
        if (!g_state.changed) return;
        g_state.changed = false;
        j = to_json_locked();
        version = ++g_state.version;
    }
    write(j, version);   // outside the lock: the settings page reads the figures meanwhile
}

void forget() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_state = State();
    std::lock_guard<std::mutex> fl(g_file_mtx);
    g_written = 0;
}

}  // namespace usage

}  // namespace search
