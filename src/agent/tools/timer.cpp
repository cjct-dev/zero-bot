// The timer tool: countdown timers.
#include "agent/tools/common.h"

#include "agent/tools/say_checks.h"
#include "core/timers.h"

namespace tools {

namespace {

// The actions live in core/timers.cpp, where the unit tests reach them.
std::string t_timer(const json &a, Context &ctx) {
    if (util::lower(str_arg(a, "action")) == "cancel")
        if (const std::string more = claims_new(a, ctx, "cancelling a timer starts no new one",
                                                "To change a running timer, use add_time, or send the new timer's start and "
                                                "this cancel together, in one reply; or send say naming only the cancellation");
            !more.empty())
            return fail(more);
    return timers::run_tool(a);
}

}  // namespace

Tool make_timer() {
    return {"timer", "Timers",
            "Countdown timers: the time left shows on the screen and an alarm rings when one ends. Use it for "
            "anything counted down from now: 'set a timer for 10 minutes', 'the eggs need six minutes', 'give me "
            "half an hour to read'. Several can run at once, each with a short label. start needs the length; if the "
            "user gave none, ask how long. cancel and add_time pick a timer by words from its label, or with only one "
            "running need none. list gives each timer's time left. Calling this is the only way a timer is started, "
            "changed or cancelled.",
            obj({{"action", E("What to do", {"start", "cancel", "list", "add_time"})},
                 {"minutes", N("For start and add_time: minutes (fractions allowed; an hour is 60)")},
                 {"seconds", N("For start and add_time: seconds, alone or on top of minutes")},
                 {"label", S("For start: a short name taken from the user's words, e.g. \"Study\" or \"Food out of the "
                             "oven\"; leave out if they gave none. For cancel and add_time: words from the label of the "
                             "timer meant")},
                 {"all", B("For cancel: true to cancel every timer")}},
                {"action"}), t_timer};
}

}  // namespace tools
