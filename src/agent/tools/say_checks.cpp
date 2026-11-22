// Checks of what a call's "say" claims, made before anything is saved or cancelled (say_checks.h).
#include "agent/tools/say_checks.h"
#include "agent/tools/common.h"

#include <algorithm>
#include <cstdlib>
#include <regex>

#include "agent/controls.h"

namespace tools {

// The confirmation the model wrote ("Added, every Tuesday at 7am") names a weekday that the date it
// chose does not fall on (2026-11-23, a Monday). That is a slip in its own date arithmetic, made
// often enough on some weekdays to fail a scored case six times in six, so the call is refused,
// naming the nearest date that is that weekday, before anything is saved or said. Only English
// names, and only when `say` names exactly one weekday ("Tuesdays and Thursdays" is left alone).
std::string weekday_slip(const json &a, Context &ctx, time_t when, const char *field) {
    if (!controls::on(controls::WeekdayCheck)) return "";
    static const char *const names[] = {"sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"};
    static const char *const shown[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    const std::string say = util::lower(str_arg(a, "say"));
    int named = -1;
    for (int i = 0; i < 7; ++i) {
        if (say.find(names[i]) == std::string::npos) continue;
        if (named >= 0) return "";
        named = i;
    }
    if (named < 0) return "";
    const int is = atoi(util::format_time(when, "%w").c_str());
    if (is == named) return "";
    int diff = (named - is + 7) % 7;   // days forward to the weekday said
    if (diff > 3) diff -= 7;           // or back, when that is nearer
    const time_t nearest = util::add_local_days(when, diff);
    ctx.say_refused = true;   // nothing saved: the call may come again with the date or the say put right
    return std::string(field) + " " + util::format_time(when, "%Y-%m-%d") + " is a " + shown[is] + ", but say calls it " +
           shown[named] + ". The nearest " + shown[named] + " is " + util::format_time(nearest, "%Y-%m-%d") +
           ": send the call again with that date, or fix say if the date was right";
}

// A cancel or delete whose confirmation claims more than the call does. In suite case reminder-028
// ("remind me tomorrow at 9 to call the bank", then "actually make it 10") the model called
// reminder_cancel alone with say "Done, I'll remind you to call the bank tomorrow at 10 instead":
// the user heard of a reminder at 10 and had none, three runs in three. Like weekday_slip, the call
// is refused before anything is cancelled, saying what is missing (`done` says what the tool does,
// `fix` what to do). The claim stands when something was set or changed earlier in this request
// (the new reminder first, then the old one cancelled). Only English, only words that say something
// new was set or moved, and not in a question ("Shall I set a new one for 10?"): "Cancelled your
// 9 o'clock reminder" and "It is now cancelled" pass.
std::string claims_new(const json &a, Context &ctx, const std::string &done, const std::string &fix) {
    if (!controls::on(controls::CancelClaims)) return "";
    static const char *const makers[] = {"reminder_set", "calendar_add", "calendar_update", "timer:start", "timer:add_time", "memory_save"};
    for (const std::string &w : ctx.worked)
        if (std::find(std::begin(makers), std::end(makers), w) != std::end(makers)) return "";
    std::string say = util::lower(str_arg(a, "say"));
    for (size_t at; (at = say.find("\xE2\x80\x99")) != std::string::npos;) say.replace(at, 3, "'");   // ’ as '
    static const std::regex claim(
        "\\b(instead|moved|rescheduled|(changed|updated|switched|pushed|brought) (it |that |this |them |the [a-z]+ )?"
        "(to|for|back|forward)|now (at|for|due|set (for|to|at))|new (time|date|reminder|event|timer|one) (at|for|on|is)|"
        "(i've|i have|i'll|i will) (set|added|made|booked|scheduled|moved)|(i'll|i will) remind you|you'll be reminded|"
        "you will be reminded)\\b");
    for (size_t from = 0; from < say.size();) {
        const size_t end = say.find_first_of(".!?", from);
        const std::string sentence = say.substr(from, end == std::string::npos ? std::string::npos : end - from);
        std::smatch m;
        if (!(end != std::string::npos && say[end] == '?') && std::regex_search(sentence, m, claim)) {
            ctx.say_refused = true;
            return done + ", but say claims more (\"" + m.str(0) + "\"), so nothing was done. " + fix;
        }
        if (end == std::string::npos) break;
        from = end + 1;
    }
    return "";
}

}  // namespace tools
