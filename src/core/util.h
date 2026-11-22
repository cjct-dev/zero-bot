// Paths, time and small string helpers.
#pragma once
#include <ctime>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace util {

std::string home_dir();
std::string config_dir();                 // ~/.config/zero-bot
std::string data_dir();                   // ~/.local/share/zero-bot
std::string cache_dir();                  // ~/.cache/zero-bot
void make_dirs(const std::string &path);

bool read_file(const std::string &path, std::string &out);
bool write_file_atomic(const std::string &path, const std::string &data, int mode = 0600);
json load_json(const std::string &path, const json &fallback);
bool save_json(const std::string &path, const json &value);

// JSON from another machine: a device, a web service, the AI service (ZB-093). The parser itself does
// not recurse, but copying, comparing or printing the tree does, once a level, and each level costs
// memory: a hostile answer of a megabyte of '[' would take the stack and tens of megabytes with it.
// No device, service or model nests half as deep as this; the Hue driver allows 48, its own limit.
constexpr int kMaxJsonDepth = 32;
// Whether a JSON text nests arrays and objects deeper than `max` (strings and their escapes are
// skipped; a closing bracket with nothing open counts the depth below zero). Read before parsing.
bool json_too_deep(const std::string &text, int max);
// The text parsed without throwing: discarded (is_discarded()) when it is not JSON or nests deeper than
// `max_depth`, which is then not parsed at all. Callers treat both alike: an answer that cannot be read.
json parse_untrusted_json(const std::string &text, int max_depth = kMaxJsonDepth);

std::string trim(const std::string &s);
// A number written out in plain decimal ("12", "-3.5", "1e3"), as a whole: spaces round it are allowed,
// nothing else ("12oops", "20 minutes", "0x10" and "nan" are not numbers). Finite only. The one reading
// of a number sent as text, for the tools' argument checks and their own reading. False, with `out`
// unchanged, for anything else.
bool to_number(const std::string &text, double &out);
std::string lower(const std::string &s);
std::vector<std::string> split(const std::string &s, char sep);
bool contains_ci(const std::string &haystack, const std::string &needle);
std::string truncate_utf8(const std::string &s, size_t max_bytes, const char *ellipsis = "…");
std::string sanitize_utf8(const std::string &s);     // replaces invalid bytes so JSON never throws
std::string base64(const std::string &data);

// Time. The app can run in a different zone from the system (useful when travelling).
void apply_timezone(const std::string &iana);         // "" = system default
std::string system_timezone();
bool timezone_exists(const std::string &iana);
std::string format_time(time_t t, const char *fmt);   // strftime in the app zone
std::string format_time_in(time_t t, const char *fmt, const std::string &iana);
std::string iso_local(time_t t);                      // 2026-11-16T23:15
bool parse_local(const std::string &text, time_t &out);  // "YYYY-MM-DD HH:MM", "YYYY-MM-DDTHH:MM[:SS]" or date only
// The user's date order. Dates are shown, typed and read day first unless this is set.
void set_month_first(bool month_first);
bool month_first();
std::string short_date(time_t t);                     // "Tue 18 Nov 09:00" or "Tue Nov 18 09:00"
std::string typed_date(time_t t);                     // "18/11/2026 09:00" or "11/18/2026 09:00", for editing
const char *typed_date_hint();                        // "Day/month/year hour:minute, 24-hour clock"
// Reads what a person types: the typed_date form (year and time optional, / . or - between the
// parts) or the YYYY-MM-DD form. A date without a year means its next occurrence.
bool parse_typed(const std::string &text, time_t &out);
// Writes the meaning after each date given in numbers, in the user's date order and with the
// year filled in: "on 3/4 at ten" becomes "on 3/4 (Saturday 3 April 2027) at ten". Text that
// only looks like a date (a fraction, "24/7") is left alone.
std::string spell_numeric_dates(const std::string &text);
// Calendar arithmetic in the app's zone. make_local normalises out-of-range fields (day 32,
// month 13), which is what stepping a date forward needs.
struct LocalTime { int year = 0, month = 0, day = 0, hour = 0, minute = 0; };
LocalTime local_fields(time_t t);
time_t make_local(int year, int month, int day, int hour, int minute);
int days_in_month(int year, int month);
// Calendar days, not 24-hour steps: a day with a clock change is 23 or 25 hours long.
time_t start_of_day(time_t t);                        // local midnight of t's day
time_t add_local_days(time_t t, int days);            // same local clock time, `days` dates later
int pivot_year(int two_digits);                       // 27 -> 2027, 60 -> 1960
std::string short_day(time_t t);                      // "Sat 14 Mar 2027" or "Sat Mar 14 2027"
std::string typed_day(time_t t);                      // "14/03/2027" or "03/14/2027"
std::string friendly_day(time_t t);                   // "Today", "Tomorrow", "Sat 14 Mar"
std::string friendly_when(time_t t);                  // "Today 18:30", "Tue 18 Nov 09:00"

template <typename T>
T jget(const json &j, const char *key, const T &fallback) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    try { return it->get<T>(); } catch (...) { return fallback; }
}

}  // namespace util
