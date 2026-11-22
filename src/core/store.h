// Calendar, reminders and long-term memory, each a small JSON file in ~/.local/share/zero-bot.
#pragma once
#include <atomic>
#include <climits>
#include <ctime>
#include <map>
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
    // Timer: read from the file when the app started, so set (or last restarted) by an earlier run
    // of it. Not stored.
    bool carried = false;
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

    // Timers. Several run at once, up to kMaxTimers. `offset_ms`, given with `now`, is how far the
    // wall clock stood from the steady one when `now` was read (see reckon).
    static constexpr int kMaxTimers = 10;
    static constexpr long long kNoClock = LLONG_MIN;      // no reading of the clocks: `now` is taken as it is
    std::vector<Reminder> timers();                       // set and not yet taken as ended, soonest end first
    int add_timer(const std::string &label, long seconds, time_t now, long long offset_ms = kNoClock);   // its id, or 0 at the cap
    bool extend_timer(int id, long seconds);
    bool delete_timer(int id);
    int clear_timers();                                   // how many there were
    // Timers whose end has come by `now`, soonest first, each returned once. They are kept in
    // memory for kRecentSeconds afterwards, so that "give it five more minutes" can restart one.
    // They leave the list at once, but the file only with the next save_timers(), so that an
    // alarm can be under way before the disk is touched.
    std::vector<Reminder> take_ended_timers(time_t now);
    void save_timers();                                   // writes what take_ended_timers left unwritten
    static constexpr long kRecentSeconds = 15 * 60;
    std::vector<Reminder> recent_timers(time_t now);      // ended in the last kRecentSeconds, newest first
    int restart_timer(int id, long seconds, time_t now, long long offset_ms = kNoClock);  // a recent one, running again; its id, or 0
    // For the main loop, without taking the lock: the soonest end (0 when none is set), and a
    // number that changes whenever the set of timers or their ends do.
    time_t next_timer_end() const { return next_end_; }
    int timers_revision() const { return revision_; }
    // Timers set while the app runs count the time that passes, whatever the wall clock does:
    // when it is set (by a time sync after start-up, on a device without a clock of its own),
    // their ends move with it. Call often, from one thread, with the wall clock and a clock that
    // runs at a steady pace (both in milliseconds). Timers carried over from an earlier run keep
    // their ends: those were reckoned by that run's clock.
    void follow_clock(long long wall_ms, long long steady_ms);
    static constexpr long long kClockStepMs = 2000;       // the wall clock moving this much more than the steady one is a step
    // A time read on another thread (the timer tool's "now"), as the timers here reckon time.
    // `offset_ms` is how far the wall clock stood from the steady one when it was read (wall minus
    // steady, as follow_clock is given them). Read on the other side of a step from the timers
    // (follow_clock has seen the step since, or has yet to see it), the time is moved across it, as
    // the timers were or will be. `offset_ms` then holds the offset the result is reckoned by, to
    // pass on with it to add_timer or restart_timer.
    time_t reckon(time_t now, long long &offset_ms);
    // Whether a time read with this offset is on the other side of a step from the timers, as when
    // the wall clock has just been set and follow_clock has yet to see it. Without the lock.
    bool clock_stepped(long long offset_ms) const;

    // Memory
    std::vector<Memory> memories();
    int add_memory(const std::string &text);
    bool update_memory(int id, const std::string &text);
    bool delete_memory(int id);
    void clear_memories();

    // Alerts that have just become due (marks them so each fires once).
    std::vector<std::string> take_due_alerts(time_t now);

private:
    // Saving. The UI thread takes mtx_ every second, and a card can take a long while to write to, so no
    // file is written with mtx_ held: save_calendar() and the others (with it held) only queue the
    // file's new contents, and the change's public function writes them once it has let go of mtx_ (a
    // WriteAfter, declared before its lock). When that function returns, its change is in the file.
    class WriteAfter;
    void save_calendar();
    void save_reminders();
    void save_memory();
    void queue(const char *file, json contents);   // with mtx_ held
    void write_queued();                           // without it
    void timers_changed();      // with mtx_ held
    time_t reckon_locked(time_t now, long long &offset_ms) const;
    std::mutex mtx_;
    std::mutex write_mtx_;                         // one writer at a time, so the newest contents are written last
    std::map<std::string, json> queued_;           // under mtx_: each file's newest contents not yet written
    std::vector<CalEvent> events_;
    std::vector<Reminder> reminders_;
    std::vector<Memory> memories_;
    std::vector<Reminder> ended_;   // timers that have ended recently (not stored)
    int next_id_ = 1;
    bool timers_unsaved_ = false;   // take_ended_timers has changed the list since the file was written
    std::atomic<time_t> next_end_{0};
    std::atomic<int> revision_{0};
    // Wall minus steady clock as of the last follow_clock: what the timers set in this run are
    // reckoned by. Only follow_clock changes it, and by a step only with mtx_ held, together with
    // the timers.
    std::atomic<long long> clock_offset_{0};
    std::atomic<bool> clock_known_{false};
};
