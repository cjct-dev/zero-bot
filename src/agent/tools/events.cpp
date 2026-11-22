// What the calendar tools share (events.h).
#include "agent/tools/events.h"
#include "agent/tools/common.h"

#include "core/store.h"

namespace tools {

namespace {

const char *const kRepeats[] = {"daily", "weekly", "monthly", "yearly"};

// Titles and notes are kept short enough to show on the screen and in an alert.
constexpr size_t kMaxTitle = 120, kMaxNotes = 1000;
constexpr double kMaxRemindMinutes = 366.0 * 24 * 60;

}  // namespace

// Reads "repeat" into `out`. "none" (or "never") clears it; an absent argument leaves it alone.
bool repeat_arg(const json &a, std::string &out, std::string &err) {
    if (!a.contains("repeat")) return true;
    const std::string r = util::lower(str_arg(a, "repeat"));
    if (r.empty() || r == "none" || r == "never" || r == "no") { out.clear(); return true; }
    for (const char *k : kRepeats)
        if (r == k) { out = r; return true; }
    if (r == "annually" || r == "annual" || r == "every year") { out = "yearly"; return true; }
    err = "repeat must be one of none, daily, weekly, monthly, yearly";
    return false;
}

time_t day_start(time_t t) {
    const util::LocalTime f = util::local_fields(t);
    return util::make_local(f.year, f.month, f.day, 0, 0);
}

// `e` is either a stored event (start = its first time) or one occurrence from events_between.
json event_json(const CalEvent &e) {
    const char *fmt = e.all_day ? "%A %Y-%m-%d" : "%A %Y-%m-%d %H:%M";
    json j = {{"id", e.id}, {"title", e.title}};
    time_t shown = e.start;
    if (!e.repeat.empty()) {
        j["repeats"] = e.repeat;
        if (!e.first) {
            // A stored series: say when it next happens, which is what the user wants to hear.
            j["first"] = util::format_time(e.start, fmt);
            shown = next_occurrence(e, day_start(time(nullptr)));
            if (!shown) shown = e.start;
        }
        // The age, for a birthday entered with its year.
        if (const int years = years_marked(e, shown)) {
            // Stated as flatly as possible: left to subtract, the model is sometimes a year out.
            j["began"] = util::local_fields(e.first ? e.first : e.start).year;
            j["age_on_that_day"] = years;
        }
    }
    j[e.repeat.empty() || e.first ? "start" : "next"] = util::format_time(shown, fmt);
    if (e.all_day) j["all_day"] = true;
    if (e.end) j["end"] = util::format_time(e.end, "%Y-%m-%d %H:%M");
    if (!e.notes.empty()) j["notes"] = e.notes;
    if (e.remind_min >= 0) {
        j["remind_minutes_before"] = e.remind_min;
        if (e.all_day) j["alert_counts_from"] = "09:00 on the day";
    }
    return j;
}

// Title, notes and alert lead time, shared by add and update. Only what is given is changed.
bool event_text_args(const json &a, CalEvent &e, std::string &err) {
    if (given(a, "title")) {
        const std::string title = clean_text(str_arg(a, "title"));
        if (title.size() > kMaxTitle) { err = "title must be under 120 characters; put the details in notes"; return false; }
        if (!title.empty()) e.title = title;
    }
    if (a.contains("notes")) {
        e.notes = clean_text(str_arg(a, "notes"));
        if (e.notes.size() > kMaxNotes) { err = "notes must be under 1000 characters; keep only what the user will need"; return false; }
    }
    if (given(a, "remind_minutes_before")) {
        double remind;
        if (!num_arg(a, "remind_minutes_before", remind))
            { err = "remind_minutes_before must be a number of minutes, such as 30 (or -1 for no alert)"; return false; }
        if (remind > kMaxRemindMinutes) { err = "remind_minutes_before must be at most a year (527040 minutes)"; return false; }
        e.remind_min = remind < 0 ? -1 : static_cast<int>(remind);
    }
    return true;
}

// A one-off event in the past is refused: a year-less date ("the 3rd of April") that has gone by
// this year is the commonest slip. A repeating event may start in the past: a birthday is entered
// with its birth year.
bool in_the_past(const CalEvent &e) {
    return e.repeat.empty() && e.start < (e.all_day ? day_start(time(nullptr)) : time(nullptr) - 12 * 3600);
}

std::string past_error() {
    return fail("that start is in the past; now is " + util::iso_local(time(nullptr)) +
                ". A date said without a year means the next time it comes round: call again with the "
                "right year. If the user really means a day that has gone, say the calendar only takes "
                "coming events");
}

}  // namespace tools
