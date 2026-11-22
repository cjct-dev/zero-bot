// The calendar_add tool: an event saved to the calendar.
#include "agent/tools/common.h"

#include <algorithm>

#include "agent/tools/events.h"
#include "agent/tools/say_checks.h"
#include "core/store.h"

namespace tools {

namespace {

std::string t_calendar_add(const json &a, Context &ctx) {
    CalEvent e;
    std::string err;
    if (clean_text(str_arg(a, "title")).empty()) return fail("title is required: a short name for the event");
    if (!event_text_args(a, e, err)) return fail(err);
    const std::string start = str_arg(a, "start");
    if (start.empty()) return fail("start is required: the day, and the time unless it is an all-day event");
    if (!util::parse_local(start, e.start)) return fail(bad_time("start", start));
    if (!repeat_arg(a, e.repeat, err)) return fail(err);
    e.all_day = bool_arg(a, "all_day", false);
    if (e.all_day) e.start = day_start(e.start);
    double since;
    if (e.repeat == "yearly" && num_arg(a, "since_year", since) && since >= 1900 && since <= util::local_fields(e.start).year) {
        // The year it all began (a birth year): the series starts there, which is what lets a
        // later answer say how many years it has been.
        const util::LocalTime f = util::local_fields(e.start);
        const int year = static_cast<int>(since);
        e.start = util::make_local(year, f.month, std::min(f.day, util::days_in_month(year, f.month)), f.hour, f.minute);
    }
    const std::string end = str_arg(a, "end");
    if (!end.empty() && !util::parse_local(end, e.end)) return fail(bad_time("end", end));
    if (e.end && e.end < e.start) return fail("end is before start");
    if (const std::string slip = weekday_slip(a, ctx, e.start, "start"); !slip.empty()) return fail(slip);
    if (in_the_past(e)) return past_error();
    if (!given(a, "remind_minutes_before") && e.all_day && e.repeat == "yearly")
        e.remind_min = 0;   // a birthday nobody is told about is no use
    e.id = Store::get().add_event(e);
    e.created = time(nullptr);
    return json{{"saved", event_json(e)}}.dump();
}

}  // namespace

Tool make_calendar_add() {
    return {"calendar_add", "Adding to calendar",
            "Add an event to the user's calendar. Calling this is the only way an event gets saved. A timed event "
            "needs a day and a time from the user; if either is missing, ask instead of guessing. A birthday, "
            "anniversary or holiday needs only its date: set all_day, and for a birthday or anniversary repeat "
            "yearly. Birthdays and anniversaries are saved here, not with memory_save.",
            obj({{"title", S("Short event title, e.g. \"Dinner with Marta\" or \"Mum's birthday\"")},
                 {"start", S("Local start as YYYY-MM-DD HH:MM (24-hour), or YYYY-MM-DD for an all-day event. For a "
                             "repeating event, the next time it happens")},
                 {"since_year", N("Optional, for a birthday or anniversary: the birth year or the year it began, "
                                  "if the user happened to say it (born 1990 -> 1990). Never ask for it; save "
                                  "without when it was not given")},
                 {"end", S("Optional local end time as YYYY-MM-DD HH:MM")},
                 {"all_day", B("True for a whole-day event with no clock time (birthday, holiday, day off)")},
                 {"repeat", E("How often it comes round again. Leave out for a one-off", {"none", "daily", "weekly", "monthly", "yearly"})},
                 {"notes", S("Optional extra detail such as place or booking reference")},
                 {"remind_minutes_before", N("Optional: alert this many minutes before the start (0 = at the start). "
                                             "All-day events count from 09:00 on the day, and yearly ones alert "
                                             "on the day unless told otherwise")}},
                {"title", "start"}), t_calendar_add};
}

}  // namespace tools
