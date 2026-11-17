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
    bool alerted = false;
};

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
    std::vector<CalEvent> events();                               // sorted by start
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
