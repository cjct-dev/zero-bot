// The calendar_update tool: an event changed (moved, renamed, made to repeat).
#include "agent/tools/common.h"

#include "agent/tools/events.h"
#include "agent/tools/say_checks.h"
#include "core/store.h"

namespace tools {

namespace {

std::string t_calendar_update(const json &a, Context &ctx) {
    int id = 0;
    std::string err;
    if (!id_arg(a, "id", id, err, "use calendar_list to find it")) return fail(err);
    for (auto e : Store::get().events()) {
        if (e.id != id) continue;
        const std::string start = str_arg(a, "start"), end = str_arg(a, "end");
        if (!event_text_args(a, e, err)) return fail(err);
        const time_t old_start = e.start;
        if (!start.empty()) {
            if (!util::parse_local(start, e.start)) return fail(bad_time("start", start));
            if (e.end && end.empty()) e.end += e.start - old_start;  // keep the duration when moving
        }
        if (!end.empty() && !util::parse_local(end, e.end)) return fail(bad_time("end", end));
        if (!repeat_arg(a, e.repeat, err)) return fail(err);
        if (a.contains("all_day")) e.all_day = bool_arg(a, "all_day", e.all_day);
        if (e.all_day) { e.start = day_start(e.start); e.end = 0; }
        if (e.end && e.end < e.start) return fail("end is before start");
        if (!start.empty())
            if (const std::string slip = weekday_slip(a, ctx, e.start, "start"); !slip.empty()) return fail(slip);
        if (e.start != old_start && in_the_past(e)) return past_error();
        if (!Store::get().update_event(e)) return fail("that event has just been deleted; nothing was changed");
        return json{{"updated", event_json(e)}}.dump();
    }
    return fail("no event has id " + std::to_string(id) + "; use calendar_list to find it");
}

}  // namespace

Tool make_calendar_update() {
    return {"calendar_update", "Updating calendar",
            "Change an existing calendar event (move it, rename it, make it repeat). Only the fields given are "
            "changed, and a repeating event changes as a whole. The id is in the result of the earlier calendar_add "
            "or calendar_list; do not add a second event to move one.",
            obj({{"id", N("Event id")}, {"title", S("New title")}, {"start", S("New start, YYYY-MM-DD HH:MM")},
                 {"end", S("New end, YYYY-MM-DD HH:MM")}, {"notes", S("New notes")},
                 {"all_day", B("Whether it is a whole-day event")},
                 {"repeat", E("How often it repeats; none stops it repeating", {"none", "daily", "weekly", "monthly", "yearly"})},
                 {"remind_minutes_before", N("New alert lead time in minutes; -1 removes the alert")}}, {"id"}),
            t_calendar_update};
}

}  // namespace tools
