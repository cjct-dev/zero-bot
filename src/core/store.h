// Calendar, reminders and long-term memory, each a small JSON file in ~/.local/share/zero-bot.
#pragma once
#include <atomic>
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
    std::string text;          // what is said when it fires; a timer's label
    time_t due = 0;            // when it fires; when a timer ends
    bool fired = false;
    // Timers (countdowns: "a timer for ten minutes") are kept with the reminders, marked by kind,
    // and fire through the same check. Files from before timers have no kind: all reminders.
    std::string kind;          // "" = reminder, "timer"
    time_t started = 0;        // timer: when it was set
    long length = 0;           // timer: its whole length in seconds, with any time added
    bool timer() const { return kind == "timer"; }
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

    // Reminders (never timers)
    std::vector<Reminder> reminders();
    int add_reminder(const std::string &text, time_t due);
    bool delete_reminder(int id);
    void clear_reminders();

    // Timers. Several run at once, up to kMaxTimers.
    static constexpr int kMaxTimers = 10;
    std::vector<Reminder> timers();                       // set and not yet taken as ended, soonest end first
    int add_timer(const std::string &label, long seconds, time_t now);   // its id, or 0 at the cap
    bool extend_timer(int id, long seconds);
    bool delete_timer(int id);
    int clear_timers();                                   // how many there were
    // Timers whose end has come by `now`, soonest first, each returned once. They are kept in
    // memory for kRecentSeconds afterwards, so that "give it five more minutes" can restart one.
    std::vector<Reminder> take_ended_timers(time_t now);
    static constexpr long kRecentSeconds = 15 * 60;
    std::vector<Reminder> recent_timers(time_t now);      // ended in the last kRecentSeconds, newest first
    int restart_timer(int id, long seconds, time_t now);  // a recent one, running again; its id, or 0
    // For the main loop, without taking the lock: the soonest end (0 when none is set), and a
    // number that changes whenever the set of timers or their ends do.
    time_t next_timer_end() const { return next_end_; }
    int timers_revision() const { return revision_; }

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
    void timers_changed();      // with mtx_ held
    std::mutex mtx_;
    std::vector<CalEvent> events_;
    std::vector<Reminder> reminders_;
    std::vector<Memory> memories_;
    std::vector<Reminder> ended_;   // timers that have ended recently (not stored)
    int next_id_ = 1;
    std::atomic<time_t> next_end_{0};
    std::atomic<int> revision_{0};
};
