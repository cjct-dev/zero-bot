// Checks of what a call's "say" claims, made before anything is saved or cancelled (say_checks.h).
#include "agent/tools/say_checks.h"
#include "agent/tools/common.h"

#include <algorithm>
#include <cstdlib>
#include <regex>

#include "agent/controls.h"

namespace tools {

namespace {

// The call's say in lower case, with typographic apostrophes as plain ones.
std::string plain_say(const json &a) {
    std::string say = util::lower(str_arg(a, "say"));
    for (size_t at; (at = say.find("\xE2\x80\x99")) != std::string::npos;) say.replace(at, 3, "'");   // ’ as '
    return say;
}

// The first words of `say` that `claim` finds in a sentence that is not a question, or "".
std::string claimed(const std::string &say, const std::regex &claim) {
    for (size_t from = 0; from < say.size();) {
        const size_t end = say.find_first_of(".!?", from);
        const std::string sentence = say.substr(from, end == std::string::npos ? std::string::npos : end - from);
        std::smatch m;
        if (!(end != std::string::npos && say[end] == '?') && std::regex_search(sentence, m, claim)) return m.str(0);
        if (end == std::string::npos) break;
        from = end + 1;
    }
    return "";
}

}  // namespace

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
    static const std::regex claim(
        "\\b(instead|moved|rescheduled|(changed|updated|switched|pushed|brought) (it |that |this |them |the [a-z]+ )?"
        "(to|for|back|forward)|now (at|for|due|set (for|to|at))|new (time|date|reminder|event|timer|one) (at|for|on|is)|"
        "(i've|i have|i'll|i will) (set|added|made|booked|scheduled|moved)|(i'll|i will) remind you|you'll be reminded|"
        "you will be reminded)\\b");
    const std::string m = claimed(plain_say(a), claim);
    if (m.empty()) return "";
    ctx.say_refused = true;
    return done + ", but say claims more (\"" + m + "\"), so nothing was done. " + fix;
}

// A stop whose confirmation announces the next step as well. Asked "play some music" while a station played, the
// model stopped the radio with say "Turning off the radio and starting your music." meaning to start the music in
// its next call; every call had worked with a say, so the loop spoke it and ended the request, and no music started
// (ZB-118, the owner's saved conversation of 2026-11-24). A radio stop now never ends the request (radio.cpp); a
// music stop that claims a start does not either. The stop stands (it is what the user wanted, in part); only its
// say does not end the request. Only English, only words that something
// starts or plays, and not in a question ("Shall I put some music on?"). A stop said in such words ("Stopped
// playing the radio") costs one model call more, nothing else.
bool claims_start(const json &a) {
    static const std::regex start(
        "\\b(start(s|ing|ed)?|play(s|ing)?|put on|putting|"
        "switch(ing|ed)? (to|over)|turn(ing)? (it |them |some )?on|here is|here's|on its way|coming up|resum(e|es|ing|ed))\\b");
    return !claimed(plain_say(a), start).empty();
}

}  // namespace tools
