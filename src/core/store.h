// Calendar, reminders and long-term memory, each a small JSON file in ~/.local/share/zero-bot.
#pragma once
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include "core/util.h"

struct CalEvent {
    int id = 0;
    std::string title;
    time_t start = 0;
    time_t end = 0;            // 0 = no end time
    std::string notes;
    int remind_min = -1;       // minutes before start to alert; -1 = no alert
    std::string repeat;        // "", "daily", "weekly", "monthly" or "yearly"; start is the first time
    bool all_day = false;      // a whole day (birthday, holiday): start is that day's midnight
    time_t alerted_for = 0;    // the occurrence whose alert has already fired
    time_t created = 0;        // when it was entered
    time_t first = 0;          // on entries from events_between: the series' first start (not stored)
};

// The first time the event happens at or after `from`, or 0 if it never does again. A yearly
// 29 February falls on the 28th in other years; a monthly 31st on the month's last day.
time_t next_occurrence(const CalEvent &e, time_t from);
// The time of day an all-day event's alert is measured from (09:00), so that "on the day" is a
// useful moment rather than midnight.
time_t alert_base(const CalEvent &e, time_t occurrence);
// For a yearly event entered with its original date (a birth year), how many years have passed
// by `occurrence`; 0 when the first date was simply the next one coming.
int years_marked(const CalEvent &e, time_t occurrence);

struct Reminder {
    int id = 0;
    std::string text;
    time_t due = 0;
    bool fired = false;
};

struct Memory {
    int id = 0;
    std::string text;
    time_t created = 0;
};

class Store {
public:
    static Store &get();
    void load();

    // Calendar
    std::vector<CalEvent> events();                               // as stored, sorted by first start
    // What happens in [from, to): one entry per occurrence, with start and end moved to it.
    std::vector<CalEvent> events_between(time_t from, time_t to);
    int add_event(CalEvent ev);
    bool update_event(const CalEvent &ev);
    bool delete_event(int id);
    void clear_events();

    // Reminders
    std::vector<Reminder> reminders();
    int add_reminder(const std::string &text, time_t due);
    bool delete_reminder(int id);
    void clear_reminders();

    // Memory
    std::vector<Memory> memories();
    int add_memory(const std::string &text);
    bool update_memory(int id, const std::string &text);
    bool delete_memory(int id);
    void clear_memories();

    // Alerts that have just become due (marks them so each fires once).
    std::vector<std::string> take_due_alerts(time_t now);

private:
    void save_calendar();
    void save_reminders();
    void save_memory();
    std::mutex mtx_;
    std::vector<CalEvent> events_;
    std::vector<Reminder> reminders_;
    std::vector<Memory> memories_;
    int next_id_ = 1;
};
