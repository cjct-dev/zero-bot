// The calendar_list tool: the calendar for a range of days, or the next time an event of that name comes up.
#include "agent/tools/common.h"

#include <algorithm>
#include <sstream>

#include "agent/tools/events.h"
#include "core/store.h"

namespace tools {

namespace {

// Letters and digits only, lower case, so that "dads" finds "Dad's".
std::string plain_words(const std::string &text) {
    std::string out;
    for (const char c : util::lower(text))
        if (isalnum(static_cast<unsigned char>(c)) || (c & 0x80)) out += c;
        else if (c == ' ' || c == '-') out += ' ';
    return out;
}

std::string t_calendar_list(const json &a, Context &) {
    const std::string search = plain_words(str_arg(a, "search"));
    time_t from = time(nullptr) - 3600, to = from + 14 * 86400;
    const std::string f = str_arg(a, "from"), t = str_arg(a, "to");
    if (!f.empty() && !util::parse_local(f, from)) return fail(bad_time("from", f, true));
    if (!t.empty()) {
        if (!util::parse_local(t, to)) return fail(bad_time("to", t, true));
        if (t.size() <= 10) to = util::add_local_days(to, 1);  // a bare date means "through the end of that day"
    } else if (!f.empty()) to = from + 14 * 86400;
    else if (!search.empty()) to = from + 366 * 86400;   // a name can be looked for a whole year ahead
    if (to <= from) return fail("to is before from; give the earlier day as from");
    // Every result stays in the conversation and is sent with each later request, so a long range
    // (or a daily event over a year) is cut short with a note rather than sent whole.
    constexpr size_t kMaxListed = 40;
    json arr = json::array();
    std::vector<int> seen;
    size_t more = 0;
    for (auto &e : Store::get().events_between(from, to)) {
        if (!search.empty()) {
            // Every word asked for has to appear; a repeating event is reported once, at its next time.
            const std::string hay = plain_words(e.title + " " + e.notes);
            bool match = true;
            std::istringstream words(search);
            for (std::string w; words >> w;) match = match && hay.find(w) != std::string::npos;
            if (!match || std::find(seen.begin(), seen.end(), e.id) != seen.end()) continue;
            seen.push_back(e.id);
        }
        if (arr.size() >= kMaxListed) { ++more; continue; }
        arr.push_back(event_json(e));
    }
    json out = {{"from", util::format_time(from, "%Y-%m-%d")}, {"to", util::format_time(to - 1, "%Y-%m-%d")}, {"events", arr}};
    if (!search.empty() && arr.empty()) out["note"] = "nothing on the calendar matches; try fewer or other words before saying it is not there";
    if (more) out["note"] = std::to_string(more) + " more events in this range are not listed; ask about a shorter range to see them";
    return out.dump(-1, ' ', false, json::error_handler_t::replace);
}

}  // namespace

Tool make_calendar_list() {
    return {"calendar_list", "Checking calendar",
            "Look at the calendar: either a date range (the next 14 days by default), or with search the next time "
            "an event with that name comes up, anywhere in the coming year. Use search for 'when is dad's birthday', "
            "'when is my dentist appointment'. Results carry ids, and repeating events appear once for each time "
            "they fall in a range.",
            obj({{"search", S("Optional words from the event's name, e.g. \"dad\" or \"dentist\"")},
                 {"from", S("Optional first day, YYYY-MM-DD")}, {"to", S("Optional last day (inclusive), YYYY-MM-DD")}}),
            t_calendar_list};
}

}  // namespace tools
