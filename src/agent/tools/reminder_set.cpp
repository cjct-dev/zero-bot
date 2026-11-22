// The reminder_set tool: a reminder or alarm at a clock time.
#include "agent/tools/common.h"

#include <cmath>

#include "agent/tools/say_checks.h"
#include "core/store.h"

namespace tools {

namespace {

std::string t_reminder_set(const json &a, Context &ctx) {
    const std::string text = clean_text(str_arg(a, "text"));
    if (text.empty()) return fail("text is required: the words to say when the reminder goes off");
    if (text.size() > 300) return fail("text must be under 300 characters");
    time_t due = 0;
    double minutes;
    const std::string at = str_arg(a, "at");
    if (num_arg(a, "in_minutes", minutes) && minutes > 0) {
        if (minutes > 366.0 * 24 * 60) return fail("in_minutes must be within a year; for a set day give at instead");
        due = time(nullptr) + static_cast<time_t>(std::llround(minutes * 60));
    } else if (!at.empty()) {
        if (!util::parse_local(at, due)) return fail(bad_time("at", at));
        // A date alone would go off at midnight, when nobody wants to be reminded of anything.
        if (at.find(':') == std::string::npos)
            return fail("at needs a clock time as well as the day (YYYY-MM-DD HH:MM). If the user gave no time, ask what time");
        if (const std::string slip = weekday_slip(a, ctx, due, "at"); !slip.empty()) return fail(slip);
    } else if (given(a, "in_minutes")) {
        return fail("in_minutes must be a number of minutes from now, more than 0");
    } else return fail("give either in_minutes or at. If the user gave no time at all, ask when");
    if (due <= time(nullptr)) return fail("that time is in the past; the current time is " + util::iso_local(time(nullptr)));
    const int id = Store::get().add_reminder(text, due);
    return json{{"set", true}, {"id", id}, {"due", util::format_time(due, "%A %Y-%m-%d %H:%M:%S")}, {"text", text}}
        .dump(-1, ' ', false, json::error_handler_t::replace);
}

}  // namespace

Tool make_reminder_set() {
    return {"reminder_set", "Setting a reminder",
            "Set a reminder or an alarm: a message the device chimes and speaks at a clock time or on a date ('remind "
            "me at 6 on Friday to call the bank', 'wake me at half six', 'set an alarm for 7'). Calling this is the "
            "only way one gets set. Give at "
            "for the clock time. A countdown from now ('set a timer', 'in 5 minutes', 'for an hour') is a timer: use "
            "timer instead. If the user gave no time at all, ask when. It fires only while this app is running. For "
            "an appointment that belongs in the calendar, use calendar_add with remind_minutes_before instead.",
            obj({{"text", S("What to say when it fires, e.g. \"Check the laundry\"")},
                 {"in_minutes", N("Fire this many minutes from now (fractions allowed)")},
                 {"at", S("Or fire at this local time, YYYY-MM-DD HH:MM")}}, {"text"}), t_reminder_set, takes_say()};
}

}  // namespace tools
