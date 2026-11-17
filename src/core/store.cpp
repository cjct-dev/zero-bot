#include "core/store.h"

#include <algorithm>

using util::jget;

Store &Store::get() {
    static Store s;
    return s;
}

static std::string path_of(const char *name) { return util::data_dir() + "/" + name; }

void Store::load() {
    std::lock_guard<std::mutex> lk(mtx_);
    events_.clear(); reminders_.clear(); memories_.clear(); ended_.clear();
    int max_id = 0;
    for (auto &j : util::load_json(path_of("calendar.json"), json::array())) {
        CalEvent e;
        e.id = jget(j, "id", 0); e.title = jget(j, "title", std::string());
        e.start = jget<long long>(j, "start", 0); e.end = jget<long long>(j, "end", 0);
        e.notes = jget(j, "notes", std::string()); e.remind_min = jget(j, "remind_min", -1);
        e.repeat = jget(j, "repeat", std::string()); e.all_day = jget(j, "all_day", false);
        e.alerted_for = jget<long long>(j, "alerted_for", 0);
        e.created = jget<long long>(j, "created", 0);
        if (jget(j, "alerted", false) && !e.alerted_for) e.alerted_for = e.start;   // files from before repeats
        if (e.id > 0 && !e.title.empty()) { events_.push_back(e); max_id = std::max(max_id, e.id); }
    }
    for (auto &j : util::load_json(path_of("reminders.json"), json::array())) {
        Reminder r;
        r.id = jget(j, "id", 0); r.text = jget(j, "text", std::string());
        r.due = jget<long long>(j, "due", 0); r.fired = jget(j, "fired", false);
        if (jget(j, "kind", std::string()) == "timer") {   // records from before timers have no kind
            r.kind = "timer";
            r.started = jget<long long>(j, "started", 0);
            r.length = jget<long long>(j, "length", 0);
        }
        if (r.id > 0) { reminders_.push_back(r); max_id = std::max(max_id, r.id); }
    }
    for (auto &j : util::load_json(path_of("memory.json"), json::array())) {
        Memory m;
        m.id = jget(j, "id", 0); m.text = jget(j, "text", std::string());
        m.created = jget<long long>(j, "created", 0);
        if (m.id > 0 && !m.text.empty()) { memories_.push_back(m); max_id = std::max(max_id, m.id); }
    }
    next_id_ = max_id + 1;
    timers_changed();
}

void Store::save_calendar() {
    json arr = json::array();
    for (auto &e : events_)
        arr.push_back({{"id", e.id}, {"title", e.title}, {"start", static_cast<long long>(e.start)},
                       {"end", static_cast<long long>(e.end)}, {"notes", e.notes},
                       {"remind_min", e.remind_min}, {"repeat", e.repeat}, {"all_day", e.all_day},
                       {"alerted_for", static_cast<long long>(e.alerted_for)},
                       {"created", static_cast<long long>(e.created)}});
    util::save_json(path_of("calendar.json"), arr);
}

void Store::save_reminders() {
    json arr = json::array();
    for (auto &r : reminders_) {
        json j = {{"id", r.id}, {"text", r.text}, {"due", static_cast<long long>(r.due)}, {"fired", r.fired}};
        if (r.timer()) {   // a reminder's record keeps its old shape
            j["kind"] = r.kind;
            j["started"] = static_cast<long long>(r.started);
            j["length"] = static_cast<long long>(r.length);
        }
        arr.push_back(j);
    }
    util::save_json(path_of("reminders.json"), arr);
}

void Store::save_memory() {
    json arr = json::array();
    for (auto &m : memories_)
        arr.push_back({{"id", m.id}, {"text", m.text}, {"created", static_cast<long long>(m.created)}});
    util::save_json(path_of("memory.json"), arr);
}

// ---------------------------------------------------------------- calendar
std::vector<CalEvent> Store::events() {
    std::lock_guard<std::mutex> lk(mtx_);
    auto out = events_;
    std::sort(out.begin(), out.end(), [](const CalEvent &a, const CalEvent &b) { return a.start < b.start; });
    return out;
}

time_t next_occurrence(const CalEvent &e, time_t from) {
    if (e.start >= from) return e.start;
    if (e.repeat.empty()) return 0;
    const util::LocalTime s = util::local_fields(e.start), f = util::local_fields(from);
    if (e.repeat == "daily" || e.repeat == "weekly") {
        const int step = e.repeat == "daily" ? 1 : 7;
        // Start a little short of the target and walk up; whole days in local time, so a clock
        // change does not shift the hour.
        long k = static_cast<long>((from - e.start) / (86400L * step)) - 1;
        if (k < 0) k = 0;
        for (int guard = 0; guard < 6; ++guard, ++k) {
            const time_t t = util::make_local(s.year, s.month, s.day + static_cast<int>(k) * step, s.hour, s.minute);
            if (t >= from) return t;
        }
        return 0;
    }
    if (e.repeat == "monthly") {
        for (int m = std::max(s.year * 12 + s.month - 1, f.year * 12 + f.month - 2), guard = 0; guard < 4; ++m, ++guard) {
            const int year = m / 12, month = m % 12 + 1;
            const time_t t = util::make_local(year, month, std::min(s.day, util::days_in_month(year, month)), s.hour, s.minute);
            if (t >= from) return t;
        }
        return 0;
    }
    if (e.repeat == "yearly") {
        for (int year = std::max(s.year, f.year - 1), guard = 0; guard < 4; ++year, ++guard) {
            const time_t t = util::make_local(year, s.month, std::min(s.day, util::days_in_month(year, s.month)), s.hour, s.minute);
            if (t >= from) return t;
        }
    }
    return 0;
}

int years_marked(const CalEvent &e, time_t occurrence) {
    const time_t first = e.first ? e.first : e.start;
    if (e.repeat != "yearly" || !e.created || first >= e.created - 86400) return 0;
    return std::max(0, util::local_fields(occurrence).year - util::local_fields(first).year);
}

time_t alert_base(const CalEvent &e, time_t occurrence) {
    if (!e.all_day) return occurrence;
    // 09:00 on the event's own date, built from the date rather than by adding nine hours to
    // midnight, which would give 08:00 or 10:00 on the days the clocks change.
    const util::LocalTime f = util::local_fields(occurrence);
    return util::make_local(f.year, f.month, f.day, 9, 0);
}

std::vector<CalEvent> Store::events_between(time_t from, time_t to) {
    std::vector<CalEvent> out;
    for (auto &e : events()) {
        // An all-day event is still "on" until its day is over.
        time_t at = next_occurrence(e, e.all_day ? util::start_of_day(from) : from);
        for (int n = 0; at && at < to && n < 60; ++n) {
            CalEvent one = e;
            one.first = e.start;
            one.start = at;
            if (e.end) one.end = at + (e.end - e.start);
            out.push_back(one);
            if (e.repeat.empty()) break;
            at = next_occurrence(e, at + 1);
        }
    }
    std::sort(out.begin(), out.end(), [](const CalEvent &a, const CalEvent &b) { return a.start < b.start; });
    return out;
}

int Store::add_event(CalEvent ev) {
    std::lock_guard<std::mutex> lk(mtx_);
    ev.id = next_id_++;
    ev.alerted_for = 0;
    ev.created = time(nullptr);
    events_.push_back(ev);
    save_calendar();
    return ev.id;
}

bool Store::update_event(const CalEvent &ev) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto &e : events_)
        if (e.id == ev.id) {
            const bool moved = e.start != ev.start || e.remind_min != ev.remind_min || e.repeat != ev.repeat ||
                               e.all_day != ev.all_day;
            const time_t created = e.created ? e.created : ev.created;
            e = ev;
            e.created = created ? created : time(nullptr);
            if (moved) e.alerted_for = 0;
            save_calendar();
            return true;
        }
    return false;
}

bool Store::delete_event(int id) {
    std::lock_guard<std::mutex> lk(mtx_);
    const auto n = events_.size();
    events_.erase(std::remove_if(events_.begin(), events_.end(), [&](const CalEvent &e) { return e.id == id; }),
                  events_.end());
    if (events_.size() == n) return false;
    save_calendar();
    return true;
}

void Store::clear_events() {
    std::lock_guard<std::mutex> lk(mtx_);
    events_.clear();
    save_calendar();
}

// ---------------------------------------------------------------- reminders
std::vector<Reminder> Store::reminders() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<Reminder> out;
    for (auto &r : reminders_)
        if (!r.fired && !r.timer()) out.push_back(r);
    std::sort(out.begin(), out.end(), [](const Reminder &a, const Reminder &b) { return a.due < b.due; });
    return out;
}

int Store::add_reminder(const std::string &text, time_t due) {
    std::lock_guard<std::mutex> lk(mtx_);
    Reminder r;
    r.id = next_id_++; r.text = text; r.due = due;
    reminders_.push_back(r);
    save_reminders();
    return r.id;
}

bool Store::delete_reminder(int id) {
    std::lock_guard<std::mutex> lk(mtx_);
    const auto n = reminders_.size();
    reminders_.erase(std::remove_if(reminders_.begin(), reminders_.end(), [&](const Reminder &r) { return r.id == id && !r.timer(); }),
                     reminders_.end());
    if (reminders_.size() == n) return false;
    save_reminders();
    return true;
}

void Store::clear_reminders() {
    std::lock_guard<std::mutex> lk(mtx_);
    reminders_.erase(std::remove_if(reminders_.begin(), reminders_.end(), [](const Reminder &r) { return !r.timer(); }),
                     reminders_.end());
    save_reminders();
}

// ---------------------------------------------------------------- timers
void Store::timers_changed() {
    time_t next = 0;
    for (auto &r : reminders_)
        if (r.timer() && (!next || r.due < next)) next = r.due;
    next_end_ = next;
    ++revision_;
}

std::vector<Reminder> Store::timers() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<Reminder> out;
    for (auto &r : reminders_)
        if (r.timer()) out.push_back(r);
    std::stable_sort(out.begin(), out.end(), [](const Reminder &a, const Reminder &b) { return a.due < b.due; });
    return out;
}

int Store::add_timer(const std::string &label, long seconds, time_t now) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (std::count_if(reminders_.begin(), reminders_.end(), [](const Reminder &r) { return r.timer(); }) >= kMaxTimers)
        return 0;
    Reminder r;
    r.id = next_id_++;
    r.text = label;
    r.kind = "timer";
    r.started = now;
    r.length = seconds;
    r.due = now + seconds;
    reminders_.push_back(r);
    save_reminders();
    timers_changed();
    return r.id;
}

bool Store::extend_timer(int id, long seconds) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto &r : reminders_)
        if (r.id == id && r.timer()) {
            r.due += seconds;
            r.length += seconds;
            save_reminders();
            timers_changed();
            return true;
        }
    return false;
}

bool Store::delete_timer(int id) {
    std::lock_guard<std::mutex> lk(mtx_);
    const auto n = reminders_.size();
    reminders_.erase(std::remove_if(reminders_.begin(), reminders_.end(), [&](const Reminder &r) { return r.id == id && r.timer(); }),
                     reminders_.end());
    if (reminders_.size() == n) return false;
    save_reminders();
    timers_changed();
    return true;
}

int Store::clear_timers() {
    std::lock_guard<std::mutex> lk(mtx_);
    const auto n = reminders_.size();
    reminders_.erase(std::remove_if(reminders_.begin(), reminders_.end(), [](const Reminder &r) { return r.timer(); }),
                     reminders_.end());
    const int gone = static_cast<int>(n - reminders_.size());
    if (gone) {
        save_reminders();
        timers_changed();
    }
    return gone;
}

std::vector<Reminder> Store::take_ended_timers(time_t now) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<Reminder> out;
    for (auto &r : reminders_)
        if (r.timer() && r.due <= now) out.push_back(r);
    if (out.empty()) return out;
    reminders_.erase(std::remove_if(reminders_.begin(), reminders_.end(), [now](const Reminder &r) { return r.timer() && r.due <= now; }),
                     reminders_.end());
    save_reminders();
    timers_changed();
    std::stable_sort(out.begin(), out.end(), [](const Reminder &a, const Reminder &b) { return a.due < b.due; });
    for (auto &r : out) ended_.insert(ended_.begin(), r);   // newest first
    ended_.erase(std::remove_if(ended_.begin(), ended_.end(), [now](const Reminder &r) { return now - r.due > kRecentSeconds; }),
                 ended_.end());
    if (ended_.size() > static_cast<size_t>(kMaxTimers)) ended_.resize(kMaxTimers);
    return out;
}

std::vector<Reminder> Store::recent_timers(time_t now) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<Reminder> out;
    for (auto &r : ended_)
        if (now - r.due <= kRecentSeconds) out.push_back(r);
    return out;
}

int Store::restart_timer(int id, long seconds, time_t now) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = std::find_if(ended_.begin(), ended_.end(), [id](const Reminder &r) { return r.id == id; });
    if (it == ended_.end() ||
        std::count_if(reminders_.begin(), reminders_.end(), [](const Reminder &r) { return r.timer(); }) >= kMaxTimers)
        return 0;
    Reminder r = *it;
    ended_.erase(it);
    r.started = now;
    r.length = seconds;
    r.due = now + seconds;
    r.fired = false;
    reminders_.push_back(r);
    save_reminders();
    timers_changed();
    return r.id;
}

// ---------------------------------------------------------------- memory
std::vector<Memory> Store::memories() {
    std::lock_guard<std::mutex> lk(mtx_);
    return memories_;
}

int Store::add_memory(const std::string &text) {
    std::lock_guard<std::mutex> lk(mtx_);
    Memory m;
    m.id = next_id_++; m.text = text; m.created = time(nullptr);
    memories_.push_back(m);
    save_memory();
    return m.id;
}

bool Store::update_memory(int id, const std::string &text) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto &m : memories_)
        if (m.id == id) { m.text = text; save_memory(); return true; }
    return false;
}

bool Store::delete_memory(int id) {
    std::lock_guard<std::mutex> lk(mtx_);
    const auto n = memories_.size();
    memories_.erase(std::remove_if(memories_.begin(), memories_.end(), [&](const Memory &m) { return m.id == id; }),
                    memories_.end());
    if (memories_.size() == n) return false;
    save_memory();
    return true;
}

void Store::clear_memories() {
    std::lock_guard<std::mutex> lk(mtx_);
    memories_.clear();
    save_memory();
}

// ---------------------------------------------------------------- alerts
std::vector<std::string> Store::take_due_alerts(time_t now) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::string> out;
    bool cal_dirty = false, rem_dirty = false;
    for (auto &e : events_) {
        if (e.remind_min < 0) continue;
        // The occurrence in play: under way or coming, but not one that ended hours ago while the
        // device was off. An all-day event stays in play for its whole day.
        const time_t occ = next_occurrence(e, e.all_day ? util::start_of_day(now) : now - 6 * 3600);
        if (!occ || occ == e.alerted_for) continue;
        if (now < alert_base(e, occ) - static_cast<time_t>(e.remind_min) * 60) continue;
        e.alerted_for = occ;
        cal_dirty = true;
        std::string when;
        if (e.all_day) {
            const std::string day = util::format_time(occ, "%Y-%m-%d");
            when = day == util::format_time(now, "%Y-%m-%d") ? " today"
                 : day == util::format_time(util::add_local_days(now, 1), "%Y-%m-%d") ? " tomorrow" : " on " + util::friendly_day(occ);
        } else {
            when = occ - now > 60 ? " at " + util::format_time(occ, "%H:%M")
                 : now - occ < 300 ? " now" : " was at " + util::format_time(occ, "%H:%M");
        }
        std::string title = e.title;
        // "Mum's birthday (67 years)" when it was entered with the birth year.
        if (const int years = years_marked(e, occ)) title += " (" + std::to_string(years) + (years == 1 ? " year)" : " years)");
        out.push_back(title + when);
    }
    for (auto &r : reminders_) {
        if (r.timer() || r.fired || now < r.due) continue;   // timers ring through timers::Alarm
        r.fired = true;
        rem_dirty = true;
        out.push_back(r.text.empty() ? "Timer finished" : r.text);
    }
    if (cal_dirty) save_calendar();
    if (rem_dirty) {
        reminders_.erase(std::remove_if(reminders_.begin(), reminders_.end(), [](const Reminder &r) { return r.fired; }),
                         reminders_.end());
        save_reminders();
    }
    return out;
}
