// The date_info tool: a date's weekday and its distance from today, by whole-day arithmetic.
#include "agent/tools/common.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace tools {

namespace {

// Calendar arithmetic on whole days, independent of time zone and daylight saving.
long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const long yoe = y - era * 400;
    const long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void civil_from_days(long z, int &y, int &m, int &d) {
    z += 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const long doe = z - era * 146097;
    const long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<int>(yoe + era * 400 + (m <= 2));
}

bool parse_day(const std::string &text, long &days) {
    int y = 0, m = 0, d = 0;
    if (sscanf(text.c_str(), "%d-%d-%d", &y, &m, &d) != 3 || y < 1 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31) return false;
    days = days_from_civil(y, m, d);
    int cy, cm, cd;
    civil_from_days(days, cy, cm, cd);
    return cy == y && cm == m && cd == d;  // rejects 31 February and the like
}

std::string day_text(long days) {
    static const char *wd[] = {"Thursday", "Friday", "Saturday", "Sunday", "Monday", "Tuesday", "Wednesday"};
    static const char *mon[] = {"January", "February", "March", "April", "May", "June", "July", "August",
                                "September", "October", "November", "December"};
    int y, m, d;
    civil_from_days(days, y, m, d);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s %d %s %d (%04d-%02d-%02d)", wd[((days % 7) + 7) % 7], d, mon[m - 1], y, y, m, d);
    return buf;
}

std::string t_date_info(const json &a, Context &) {
    long today;
    parse_day(util::format_time(time(nullptr), "%Y-%m-%d"), today);
    long day = today;
    const std::string date = str_arg(a, "date");
    if (!date.empty() && !parse_day(date, day)) return fail(bad_time("date", date, true));
    double add = 0;
    if (given(a, "add_days")) {
        if (!num_arg(a, "add_days", add)) return fail("add_days must be a number of days, such as 30 or -7");
        // Far enough for any question; beyond it the calendar arithmetic would overflow.
        if (std::fabs(add) > 1e6) return fail("add_days must be within a million days either way");
        day += static_cast<long>(std::llround(add));
    }
    json out = {{"date", day_text(day)}, {"today", day_text(today)}};
    const long diff = day - today;
    if (diff == 0) out["days_from_today"] = "0 (it is today)";
    else {
        const long n = std::labs(diff);
        std::string span = std::to_string(n) + (n == 1 ? " day" : " days");
        if (n >= 14) span += " (" + std::to_string(n / 7) + " weeks" + (n % 7 ? " and " + std::to_string(n % 7) + (n % 7 == 1 ? " day" : " days") : "") + ")";
        out["days_from_today"] = span + (diff > 0 ? " from today" : " ago");
    }
    return out.dump();
}

}  // namespace

Tool make_date_info() {
    return {"date_info", "Checking the date",
            "Exact calendar facts about a date: its weekday and how many days it is from today. Use it for every "
            "'how many days until', 'how long ago', 'what day of the week is', 'what is the date N days from now' "
            "question; counting days in your head goes wrong.",
            obj({{"date", S("The date as YYYY-MM-DD, e.g. the coming 25 December for Christmas. Omit for today.")},
                 {"add_days", N("Optional: move this many days on from the date first (negative for back)")}}), t_date_info};
}

}  // namespace tools
