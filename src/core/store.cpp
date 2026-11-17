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
    events_.clear(); reminders_.clear(); memories_.clear();
    int max_id = 0;
    for (auto &j : util::load_json(path_of("calendar.json"), json::array())) {
        CalEvent e;
        e.id = jget(j, "id", 0); e.title = jget(j, "title", std::string());
        e.start = jget<long long>(j, "start", 0); e.end = jget<long long>(j, "end", 0);
        e.notes = jget(j, "notes", std::string()); e.remind_min = jget(j, "remind_min", -1);
        e.alerted = jget(j, "alerted", false);
        if (e.id > 0 && !e.title.empty()) { events_.push_back(e); max_id = std::max(max_id, e.id); }
    }
    for (auto &j : util::load_json(path_of("reminders.json"), json::array())) {
        Reminder r;
        r.id = jget(j, "id", 0); r.text = jget(j, "text", std::string());
        r.due = jget<long long>(j, "due", 0); r.fired = jget(j, "fired", false);
        if (r.id > 0) { reminders_.push_back(r); max_id = std::max(max_id, r.id); }
    }
    for (auto &j : util::load_json(path_of("memory.json"), json::array())) {
        Memory m;
        m.id = jget(j, "id", 0); m.text = jget(j, "text", std::string());
        m.created = jget<long long>(j, "created", 0);
        if (m.id > 0 && !m.text.empty()) { memories_.push_back(m); max_id = std::max(max_id, m.id); }
    }
    next_id_ = max_id + 1;
}

void Store::save_calendar() {
    json arr = json::array();
    for (auto &e : events_)
        arr.push_back({{"id", e.id}, {"title", e.title}, {"start", static_cast<long long>(e.start)},
                       {"end", static_cast<long long>(e.end)}, {"notes", e.notes},
                       {"remind_min", e.remind_min}, {"alerted", e.alerted}});
    util::save_json(path_of("calendar.json"), arr);
}

void Store::save_reminders() {
    json arr = json::array();
    for (auto &r : reminders_)
        arr.push_back({{"id", r.id}, {"text", r.text}, {"due", static_cast<long long>(r.due)}, {"fired", r.fired}});
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

std::vector<CalEvent> Store::events_between(time_t from, time_t to) {
    std::vector<CalEvent> out;
    for (auto &e : events())
        if (e.start >= from && e.start < to) out.push_back(e);
    return out;
}

int Store::add_event(CalEvent ev) {
    std::lock_guard<std::mutex> lk(mtx_);
    ev.id = next_id_++;
    ev.alerted = false;
    events_.push_back(ev);
    save_calendar();
    return ev.id;
}

bool Store::update_event(const CalEvent &ev) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto &e : events_)
        if (e.id == ev.id) {
            const bool moved = e.start != ev.start || e.remind_min != ev.remind_min;
            e = ev;
            if (moved) e.alerted = false;
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
        if (!r.fired) out.push_back(r);
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
    reminders_.erase(std::remove_if(reminders_.begin(), reminders_.end(), [&](const Reminder &r) { return r.id == id; }),
                     reminders_.end());
    if (reminders_.size() == n) return false;
    save_reminders();
    return true;
}

void Store::clear_reminders() {
    std::lock_guard<std::mutex> lk(mtx_);
    reminders_.clear();
    save_reminders();
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
        if (e.alerted || e.remind_min < 0) continue;
        const time_t at = e.start - static_cast<time_t>(e.remind_min) * 60;
        if (now < at) continue;
        e.alerted = true;
        cal_dirty = true;
        if (now - e.start > 6 * 3600) continue;  // long past (device was off); do not nag
        out.push_back(e.title + (e.remind_min > 0 ? " at " + util::format_time(e.start, "%H:%M") : " now"));
    }
    for (auto &r : reminders_) {
        if (r.fired || now < r.due) continue;
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
