#include "agent/tools.h"

#include <algorithm>
#include <sstream>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <random>
#include <unistd.h>

#include "agent/calc.h"
#include "agent/llm.h"
#include "core/music_lib.h"
#include "core/settings.h"
#include "core/store.h"
#include "core/tether.h"
#include "net/http.h"
#include "platform/audio.h"
#include "core/breath.h"
#include "smarthome/home.h"
#include "platform/camera.h"
#include "platform/display.h"
#include "platform/ir.h"
#include "platform/proc.h"

namespace tools {

namespace {

using util::jget;

bool dry_run() { return getenv("ZB_DRY_RUN") != nullptr; }

std::string str_arg(const json &a, const char *key) { return util::trim(jget(a, key, std::string())); }

// Models sometimes send numbers as strings; accept both.
bool num_arg(const json &a, const char *key, double &out) {
    auto it = a.find(key);
    if (it == a.end() || it->is_null()) return false;
    if (it->is_number()) { out = it->get<double>(); return true; }
    if (it->is_string()) {
        char *end = nullptr;
        const std::string s = it->get<std::string>();
        out = strtod(s.c_str(), &end);
        return end != s.c_str();
    }
    return false;
}

bool bool_arg(const json &a, const char *key, bool fallback) {
    auto it = a.find(key);
    if (it == a.end()) return fallback;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_string()) return util::lower(it->get<std::string>()) == "true";
    return fallback;
}

json fetch_json(const std::string &url, std::string &err, Context &ctx, const std::vector<std::string> &headers = {},
                const std::string &post_body = "") {
    http::Request req;
    req.url = url;
    req.headers = headers;
    req.timeout_s = 20;
    req.cancel = ctx.cancel;
    if (!post_body.empty()) { req.method = "POST"; req.body = post_body; }
    const http::Response res = http::send(req);
    if (!res.ok()) { err = res.error == "no connection" ? "no internet connection" : res.describe(); return json(); }
    json j = json::parse(res.body, nullptr, false);
    if (j.is_discarded()) { err = "unexpected reply from the service"; return json(); }
    return j;
}

std::string fail(const std::string &msg) { return "ERROR: " + msg; }

struct Place {
    std::string name, country, timezone;
    double lat = 0, lon = 0;
};

// True when a geocoding hit lies in the country or region the user named ("canada", "UK", "texas").
bool in_region(const json &r, const std::string &region_in) {
    std::string region = util::lower(util::trim(region_in));
    if (region.empty()) return false;
    static const std::pair<const char *, const char *> kAliases[] = {
        {"uk", "gb"}, {"britain", "gb"}, {"great britain", "gb"}, {"usa", "us"}, {"america", "us"},
        {"united states of america", "us"}, {"uae", "ae"}, {"holland", "nl"},
    };
    for (const auto &alias : kAliases)
        if (region == alias.first) region = alias.second;
    return util::lower(jget(r, "country_code", std::string())) == region ||
           (region.size() > 2 && (util::contains_ci(jget(r, "country", std::string()), region) ||
                                  util::contains_ci(jget(r, "admin1", std::string()), region)));
}

// `country` narrows a bare city name to one country; it is only a preference, so a city that
// does not exist there still resolves to its best-known namesake.
bool geocode(const std::string &query, Place &out, std::string &err, Context &ctx, const std::string &country = "") {
    const std::string base = "https://geocoding-api.open-meteo.com/v1/search?language=en&count=";
    json j = fetch_json(base + (country.empty() ? "1" : "10") + "&name=" + http::url_encode(query), err, ctx);
    if (!err.empty()) return false;
    size_t pick = 0;
    if (!country.empty() && j.contains("results") && j["results"].is_array())
        for (size_t i = 0; i < j["results"].size(); ++i)
            if (in_region(j["results"][i], country)) { pick = i; break; }
    // Spoken requests arrive as "paris france" or "portland oregon", which the service does not
    // match as one name. Search for the leading words and prefer the hit in the region named.
    std::string name = query;
    for (char &c : name)
        if (c == ',') c = ' ';
    name = util::trim(name);
    for (size_t cut = name.rfind(' '); (!j.contains("results") || j["results"].empty()) && cut != std::string::npos && cut > 0;
         cut = name.rfind(' ', cut - 1)) {
        const std::string region = util::trim(name.substr(cut + 1));
        j = fetch_json(base + "8&name=" + http::url_encode(util::trim(name.substr(0, cut))), err, ctx);
        if (!err.empty()) return false;
        if (!j.contains("results") || !j["results"].is_array()) continue;
        for (size_t i = 0; i < j["results"].size(); ++i) {
            if (in_region(j["results"][i], region)) { pick = i; break; }
        }
    }
    if (!j.contains("results") || !j["results"].is_array() || j["results"].empty()) {
        err = "could not find a place called '" + query + "'. No weather or time is available for it; ask the user "
              "which town or city they mean";
        return false;
    }
    const json &r = j["results"][pick];
    out.name = jget(r, "name", query);
    out.country = jget(r, "country", std::string());
    out.timezone = jget(r, "timezone", std::string());
    out.lat = jget(r, "latitude", 0.0);
    out.lon = jget(r, "longitude", 0.0);
    return true;
}

const char *weather_text(int code) {
    switch (code) {
        case 0: return "clear sky";
        case 1: return "mainly clear";
        case 2: return "partly cloudy";
        case 3: return "overcast";
        case 45: case 48: return "fog";
        case 51: case 53: case 55: return "drizzle";
        case 56: case 57: return "freezing drizzle";
        case 61: return "light rain";
        case 63: return "rain";
        case 65: return "heavy rain";
        case 66: case 67: return "freezing rain";
        case 71: return "light snow";
        case 73: return "snow";
        case 75: return "heavy snow";
        case 77: return "snow grains";
        case 80: case 81: return "rain showers";
        case 82: return "violent rain showers";
        case 85: case 86: return "snow showers";
        case 95: return "thunderstorm";
        case 96: case 99: return "thunderstorm with hail";
        default: return "unknown conditions";
    }
}

std::string fmt1(double v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f", v);
    return buf;
}

std::string fmt_money(double v) {
    char buf[48];
    snprintf(buf, sizeof(buf), std::fabs(v) >= 100 ? "%.2f" : "%.4g", v);
    return buf;
}

// ---------------------------------------------------------------- calculator / python
std::string t_calculator(const json &a, Context &) {
    const std::string expr = str_arg(a, "expression");
    if (expr.empty()) return fail("expression is required");
    double value;
    std::string err;
    if (!calc::evaluate(expr, value, err)) return fail(err + ". For anything beyond arithmetic use run_python.");
    return calc::format(value);
}

std::string t_python(const json &a, Context &) {
    const std::string code = jget(a, "code", std::string());
    if (util::trim(code).empty()) return fail("code is required");
    if (!proc::have("bwrap")) return fail("the Python sandbox is not available on this device");
    // No network, no access to user files, a throw-away /tmp, and limits on time and memory.
    std::vector<std::string> argv = {"bwrap", "--unshare-all", "--die-with-parent", "--new-session",
                                     "--ro-bind", "/usr", "/usr", "--symlink", "usr/bin", "/bin",
                                     "--symlink", "usr/lib", "/lib", "--proc", "/proc", "--dev", "/dev",
                                     "--tmpfs", "/tmp", "--tmpfs", "/home", "--setenv", "HOME", "/tmp",
                                     "--setenv", "PATH", "/usr/bin", "--chdir", "/tmp"};
    if (access("/usr/lib64", F_OK) == 0) { argv.insert(argv.end(), {"--symlink", "usr/lib64", "/lib64"}); }
    if (access("/etc/ld.so.cache", R_OK) == 0)
        argv.insert(argv.end(), {"--ro-bind", "/etc/ld.so.cache", "/etc/ld.so.cache"});
    argv.insert(argv.end(), {"sh", "-c", "ulimit -v 400000 2>/dev/null; exec python3 -I -"});
    const proc::Result res = proc::run(argv, code, 20000, 6000, true);
    std::string out = util::sanitize_utf8(util::trim(res.out));
    if (res.timed_out) return fail("the code ran for more than 20 seconds and was stopped." + (out.empty() ? "" : " Output so far:\n" + out));
    if (res.code != 0) return "ERROR (exit code " + std::to_string(res.code) + "):\n" + (out.empty() ? "(no output)" : out);
    return out.empty() ? "(the code ran but printed nothing; use print() to see a result)" : out;
}

// ---------------------------------------------------------------- dates
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
    if (sscanf(text.c_str(), "%d-%d-%d", &y, &m, &d) != 3 || y < 1 || m < 1 || m > 12 || d < 1 || d > 31) return false;
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
    if (!date.empty() && !parse_day(date, day)) return fail("date must be a real date written as YYYY-MM-DD");
    double add = 0;
    const bool shifted = num_arg(a, "add_days", add) && add != 0;
    if (shifted) day += static_cast<long>(std::llround(add));
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

// ---------------------------------------------------------------- camera
std::string t_look(const json &a, Context &ctx) {
    std::string question = str_arg(a, "question");
    if (question.empty()) question = "Describe what you see.";
    const std::string path = util::cache_dir() + "/last_photo.jpg";
    util::make_dirs(util::cache_dir());
    // On a phone's Bluetooth link the upload is the slow part, so a smaller photo is taken there:
    // still plenty to read a sign or menu, at about a third of the bytes.
    const bool slow_link = tether::via_phone();
    const bool flip = Settings::get().camera_flip;
    std::string err = slow_link ? camera::capture(path, 960, 720, flip) : camera::capture(path, 1280, 960, flip);
    if (!err.empty()) return fail(err);
    if (ctx.on_image) ctx.on_image(path);
    std::string jpeg;
    if (!util::read_file(path, jpeg)) return fail("could not read the photo");
    const std::string answer = llm::describe_image(jpeg, question, err, ctx.cancel);
    if (!err.empty()) return fail("the photo was taken but could not be analysed: " + err);
    return answer.empty() ? fail("the vision model returned nothing") : answer;
}

// ---------------------------------------------------------------- calendar
const char *const kRepeats[] = {"daily", "weekly", "monthly", "yearly"};

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

std::string t_calendar_add(const json &a, Context &) {
    CalEvent e;
    e.title = str_arg(a, "title");
    if (e.title.empty()) return fail("title is required");
    if (!util::parse_local(str_arg(a, "start"), e.start)) return fail("start must look like 2026-03-14 18:30");
    std::string err;
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
    if (!end.empty() && !util::parse_local(end, e.end)) return fail("end must look like 2026-03-14 19:30");
    if (e.end && e.end < e.start) return fail("end is before start");
    // A year-less date ("the 3rd of April") that has gone by this year is the commonest slip. A
    // repeating event may start in the past: a birthday is entered with its birth year.
    if (e.repeat.empty() && e.start < (e.all_day ? day_start(time(nullptr)) : time(nullptr) - 12 * 3600))
        return fail("that start is in the past; now is " + util::iso_local(time(nullptr)) +
                    ". A date said without a year means the next time it comes round: call again with the "
                    "right year. If the user really means a day that has gone, say the calendar only takes "
                    "coming events");
    e.notes = str_arg(a, "notes");
    double remind;
    if (num_arg(a, "remind_minutes_before", remind)) { if (remind >= 0) e.remind_min = static_cast<int>(remind); }
    else if (e.all_day && e.repeat == "yearly") e.remind_min = 0;   // a birthday nobody is told about is no use
    e.id = Store::get().add_event(e);
    e.created = time(nullptr);
    return json{{"saved", event_json(e)}}.dump();
}

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
    if (!f.empty() && !util::parse_local(f, from)) return fail("from must look like 2026-03-14");
    if (!t.empty()) {
        if (!util::parse_local(t, to)) return fail("to must look like 2026-03-21");
        if (t.size() <= 10) to += 86400;  // a bare date means "through the end of that day"
    } else if (!f.empty()) to = from + 14 * 86400;
    else if (!search.empty()) to = from + 366 * 86400;   // a name can be looked for a whole year ahead
    json arr = json::array();
    std::vector<int> seen;
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
        arr.push_back(event_json(e));
    }
    json out = {{"from", util::format_time(from, "%Y-%m-%d")}, {"to", util::format_time(to - 1, "%Y-%m-%d")}, {"events", arr}};
    if (!search.empty() && arr.empty()) out["note"] = "nothing on the calendar matches; try fewer or other words before saying it is not there";
    return out.dump();
}

std::string t_calendar_update(const json &a, Context &) {
    double idv;
    if (!num_arg(a, "id", idv)) return fail("id is required; use calendar_list to find it");
    for (auto e : Store::get().events()) {
        if (e.id != static_cast<int>(idv)) continue;
        const std::string title = str_arg(a, "title"), start = str_arg(a, "start"), end = str_arg(a, "end");
        if (!title.empty()) e.title = title;
        if (!start.empty()) {
            const time_t old = e.start;
            if (!util::parse_local(start, e.start)) return fail("start must look like 2026-03-14 18:30");
            if (e.end && end.empty()) e.end += e.start - old;  // keep the duration when moving
        }
        if (!end.empty() && !util::parse_local(end, e.end)) return fail("end must look like 2026-03-14 19:30");
        if (a.contains("notes")) e.notes = str_arg(a, "notes");
        double remind;
        if (num_arg(a, "remind_minutes_before", remind)) e.remind_min = remind < 0 ? -1 : static_cast<int>(remind);
        std::string err;
        if (!repeat_arg(a, e.repeat, err)) return fail(err);
        if (a.contains("all_day")) e.all_day = bool_arg(a, "all_day", e.all_day);
        if (e.all_day) { e.start = day_start(e.start); e.end = 0; }
        Store::get().update_event(e);
        return json{{"updated", event_json(e)}}.dump();
    }
    return fail("no event with that id");
}

std::string t_calendar_delete(const json &a, Context &) {
    double idv;
    if (!num_arg(a, "id", idv)) return fail("id is required; use calendar_list to find it");
    return Store::get().delete_event(static_cast<int>(idv)) ? "deleted" : fail("no event with that id");
}

// ---------------------------------------------------------------- reminders
std::string t_reminder_set(const json &a, Context &) {
    const std::string text = str_arg(a, "text");
    time_t due = 0;
    double minutes;
    const std::string at = str_arg(a, "at");
    if (num_arg(a, "in_minutes", minutes) && minutes > 0) due = time(nullptr) + static_cast<time_t>(minutes * 60);
    else if (!at.empty()) {
        if (!util::parse_local(at, due)) return fail("at must look like 2026-03-14 18:30");
    } else return fail("give either in_minutes or at");
    if (due <= time(nullptr)) return fail("that time is in the past; the current time is " + util::iso_local(time(nullptr)));
    const int id = Store::get().add_reminder(text, due);
    return json{{"set", true}, {"id", id}, {"due", util::format_time(due, "%A %Y-%m-%d %H:%M:%S")}, {"text", text}}.dump();
}

std::string t_reminder_list(const json &, Context &) {
    json arr = json::array();
    for (auto &r : Store::get().reminders())
        arr.push_back({{"id", r.id}, {"due", util::format_time(r.due, "%Y-%m-%d %H:%M")}, {"text", r.text}});
    return json{{"reminders", arr}}.dump();
}

std::string t_reminder_cancel(const json &a, Context &) {
    double idv;
    if (!num_arg(a, "id", idv)) return fail("id is required; use reminder_list to find it");
    return Store::get().delete_reminder(static_cast<int>(idv)) ? "cancelled" : fail("no reminder with that id");
}

// ---------------------------------------------------------------- memory
std::string t_memory_save(const json &a, Context &) {
    const std::string text = str_arg(a, "text");
    if (text.empty()) return fail("text is required");
    if (text.size() > 400) return fail("keep each memory under 400 characters");
    double idv;
    if (num_arg(a, "replace_id", idv)) {
        if (!Store::get().update_memory(static_cast<int>(idv), text)) return fail("no memory with that id");
        return "updated memory " + std::to_string(static_cast<int>(idv));
    }
    return "saved as memory " + std::to_string(Store::get().add_memory(text));
}

std::string t_memory_delete(const json &a, Context &) {
    double idv;
    if (!num_arg(a, "id", idv)) return fail("id is required; memory ids are listed in your instructions");
    return Store::get().delete_memory(static_cast<int>(idv)) ? "deleted" : fail("no memory with that id");
}

// ---------------------------------------------------------------- web
std::string t_web_search(const json &a, Context &ctx) {
    const std::string query = str_arg(a, "query");
    if (query.empty()) return fail("query is required");
    const std::string key = Settings::get().search_key;
    if (key.empty())
        return fail("web search is not set up on this device (a search key can be added in settings), so this could "
                    "not be looked up. Tell the user that; if you answer from memory, say it may be out of date");
    const json body = {{"query", query}, {"search_depth", "basic"}, {"include_answer", true}, {"max_results", 5}};
    std::string err;
    const json j = fetch_json("https://api.tavily.com/search", err, ctx,
                              {"Content-Type: application/json", "Authorization: Bearer " + key}, body.dump());
    if (!err.empty()) return fail(err);
    json out = json::object();
    const std::string answer = jget(j, "answer", std::string());
    if (!answer.empty()) out["summary"] = answer;
    json results = json::array();
    if (j.contains("results") && j["results"].is_array()) {
        for (const json &r : j["results"]) {
            results.push_back({{"title", jget(r, "title", std::string())},
                               {"source", jget(r, "url", std::string())},
                               {"text", util::truncate_utf8(jget(r, "content", std::string()), 500)}});
            if (results.size() >= 4) break;
        }
    }
    out["results"] = results;
    return out.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string t_weather(const json &a, Context &ctx) {
    std::string where = str_arg(a, "location");
    std::string country;
    if (where.empty()) { where = Settings::get().home_city; country = Settings::get().home_country; }
    if (where.empty()) return fail("no location given and no home city is set; ask the user where they are");
    double days_d = 3;
    num_arg(a, "days", days_d);
    const int days = std::max(1, std::min(7, static_cast<int>(days_d)));
    Place p;
    std::string err;
    if (!geocode(where, p, err, ctx, country)) return fail(err);
    const bool imperial = Settings::get().units == "imperial";
    char url[512];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&timezone=auto&forecast_days=%d"
             "&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m,relative_humidity_2m"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset%s",
             p.lat, p.lon, days, imperial ? "&temperature_unit=fahrenheit&wind_speed_unit=mph" : "");
    const json j = fetch_json(url, err, ctx);
    if (!err.empty()) return fail(err);
    const std::string deg = imperial ? "F" : "C", wind = imperial ? " mph" : " km/h";
    json out = {{"place", p.name + (p.country.empty() ? "" : ", " + p.country)}};
    if (j.contains("current")) {
        const json &c = j["current"];
        out["now"] = std::string(weather_text(jget(c, "weather_code", -1))) + ", " +
                     fmt1(jget(c, "temperature_2m", 0.0)) + deg + " (feels like " +
                     fmt1(jget(c, "apparent_temperature", 0.0)) + deg + "), wind " +
                     fmt1(jget(c, "wind_speed_10m", 0.0)) + wind + ", humidity " +
                     std::to_string(jget(c, "relative_humidity_2m", 0)) + "%";
    }
    if (j.contains("daily") && j["daily"].contains("time")) {
        const json &d = j["daily"];
        json arr = json::array();
        for (size_t i = 0; i < d["time"].size(); ++i) {
            auto at = [&](const char *k) -> json { return d.contains(k) && i < d[k].size() ? d[k][i] : json(); };
            const json hi = at("temperature_2m_max"), lo = at("temperature_2m_min"), rain = at("precipitation_probability_max");
            arr.push_back(d["time"][i].get<std::string>() + ": " +
                          weather_text(at("weather_code").is_number() ? at("weather_code").get<int>() : -1) + ", " +
                          (lo.is_number() ? fmt1(lo.get<double>()) : "?") + " to " +
                          (hi.is_number() ? fmt1(hi.get<double>()) : "?") + deg +
                          (rain.is_number() ? ", rain chance " + std::to_string(rain.get<int>()) + "%" : ""));
        }
        out["forecast"] = arr;
    }
    return out.dump();
}

std::string t_currency(const json &a, Context &ctx) {
    double amount = 1;
    num_arg(a, "amount", amount);
    std::string from = str_arg(a, "from"), to = str_arg(a, "to");
    std::transform(from.begin(), from.end(), from.begin(), ::toupper);
    std::transform(to.begin(), to.end(), to.begin(), ::toupper);
    if (from.size() != 3 || to.size() != 3) return fail("from and to must be 3-letter currency codes such as GBP, EUR, JPY");
    std::string err;
    const json j = fetch_json("https://open.er-api.com/v6/latest/" + from, err, ctx);
    if (!err.empty()) return fail(err);
    if (jget(j, "result", std::string()) != "success" || !j.contains("rates") || !j["rates"].contains(to))
        return fail("no exchange rate available for " + from + " to " + to);
    const double rate = j["rates"][to].get<double>();
    return json{{"amount", amount}, {"from", from}, {"to", to}, {"rate", rate},
                {"result", fmt_money(amount * rate) + " " + to},
                {"rate_date", jget(j, "time_last_update_utc", std::string()).substr(0, 16)}}.dump();
}

std::string t_stock(const json &a, Context &ctx) {
    std::string symbol = str_arg(a, "symbol");
    if (symbol.empty()) return fail("symbol (a ticker or company name) is required");
    const std::vector<std::string> hdr = {"Accept: application/json"};
    std::string err, name;
    // Anything that does not look like a ticker is looked up by name first.
    const bool tickerish = symbol.size() <= 8 && symbol.find(' ') == std::string::npos &&
                           std::all_of(symbol.begin(), symbol.end(), [](unsigned char c) { return isupper(c) || isdigit(c) || c == '.' || c == '-' || c == '^' || c == '='; });
    if (!tickerish) {
        const json s = fetch_json("https://query1.finance.yahoo.com/v1/finance/search?quotesCount=3&newsCount=0&q=" +
                                      http::url_encode(symbol), err, ctx, hdr);
        if (!err.empty()) return fail(err);
        if (!s.contains("quotes") || !s["quotes"].is_array() || s["quotes"].empty())
            return fail("no listed company or fund found for '" + symbol + "'");
        name = jget(s["quotes"][0], "longname", jget(s["quotes"][0], "shortname", std::string()));
        symbol = jget(s["quotes"][0], "symbol", std::string());
        if (symbol.empty()) return fail("no ticker found for that name");
    }
    const json j = fetch_json("https://query1.finance.yahoo.com/v8/finance/chart/" + http::url_encode(symbol) +
                                  "?range=5d&interval=1d", err, ctx, hdr);
    if (!err.empty()) return fail(err);
    if (!j.contains("chart") || !j["chart"].contains("result") || !j["chart"]["result"].is_array() ||
        j["chart"]["result"].empty())
        return fail("no price data for '" + symbol + "'; check the ticker");
    const json &m = j["chart"]["result"][0]["meta"];
    const double price = jget(m, "regularMarketPrice", 0.0);
    const double prev = jget(m, "chartPreviousClose", jget(m, "previousClose", 0.0));
    json out = {{"symbol", jget(m, "symbol", symbol)},
                {"name", jget(m, "longName", jget(m, "shortName", name))},
                {"price", price}, {"currency", jget(m, "currency", std::string())},
                {"exchange", jget(m, "fullExchangeName", jget(m, "exchangeName", std::string()))},
                {"day_high", jget(m, "regularMarketDayHigh", 0.0)}, {"day_low", jget(m, "regularMarketDayLow", 0.0)},
                {"year_high", jget(m, "fiftyTwoWeekHigh", 0.0)}, {"year_low", jget(m, "fiftyTwoWeekLow", 0.0)}};
    const long long when = jget<long long>(m, "regularMarketTime", 0);
    if (when) out["as_of"] = util::format_time(static_cast<time_t>(when), "%Y-%m-%d %H:%M");
    if (m.contains("regularMarketChangePercent") && m["regularMarketChangePercent"].is_number())
        out["change_percent_today"] = std::round(m["regularMarketChangePercent"].get<double>() * 100) / 100;
    else if (prev > 0)
        out["change_percent_5_days"] = std::round((price - prev) / prev * 10000) / 100;
    if (jget(m, "currency", std::string()) == "GBp") out["note"] = "price is in pence";
    return out.dump();
}

// ---------------------------------------------------------------- time
std::string t_world_time(const json &a, Context &ctx) {
    const std::string place = str_arg(a, "place");
    if (place.empty()) return fail("place is required (a city or an IANA zone such as Asia/Tokyo)");
    std::string zone = place, label = place, err;
    if (!util::timezone_exists(zone)) {
        Place p;
        if (!geocode(place, p, err, ctx)) return fail(err);
        if (p.timezone.empty() || !util::timezone_exists(p.timezone)) return fail("no time zone known for " + place);
        zone = p.timezone;
        label = p.name + (p.country.empty() ? "" : ", " + p.country);
    }
    time_t t = time(nullptr);
    const std::string at = str_arg(a, "local_time");
    if (!at.empty() && !util::parse_local(at, t)) return fail("local_time must look like 2026-03-14 18:30");
    return json{{"place", label}, {"timezone", zone},
                {"time_there", util::format_time_in(t, "%A %Y-%m-%d %H:%M (UTC%z)", zone)},
                {"time_here", util::format_time(t, "%A %Y-%m-%d %H:%M (UTC%z)")}}.dump();
}

std::string t_set_timezone(const json &a, Context &ctx) {
    const std::string place = str_arg(a, "place");
    if (place.empty()) return fail("place is required (a city or an IANA zone such as Europe/Lisbon)");
    std::string zone = place, err;
    if (!util::timezone_exists(zone)) {
        Place p;
        if (!geocode(place, p, err, ctx)) return fail(err);
        if (p.timezone.empty() || !util::timezone_exists(p.timezone)) return fail("no time zone known for " + place);
        zone = p.timezone;
    }
    Settings &s = Settings::get();
    s.timezone = zone;
    s.save();
    util::apply_timezone(zone);
    return json{{"timezone", zone}, {"local_time_now", util::format_time(time(nullptr), "%A %Y-%m-%d %H:%M")},
                {"note", "Calendar entries keep their absolute moment, so they now show in the new zone."}}.dump();
}

// ---------------------------------------------------------------- music
std::string t_music(const json &a, Context &ctx) {
    const std::string action = util::lower(str_arg(a, "action"));
    audio::Music &player = audio::Music::get();
    MusicLibrary &lib = MusicLibrary::get();
    if (action == "stop") { if (!dry_run()) player.stop(); return "music stopped"; }
    if (action == "next") {
        if (!player.playing()) return fail("nothing is playing");
        player.next();
        return "skipped to the next track";
    }
    if (action == "now_playing") {
        const std::string cur = player.current();
        if (cur.empty()) return "nothing is playing";
        const Track t = lib.lookup(cur);
        return json{{"title", t.title}, {"artist", t.artist}, {"album", t.album}}.dump();
    }
    if (action == "library") {
        json out = {{"tracks", lib.size()}};
        json g = json::array(), ar = json::array();
        for (auto &x : lib.genres()) { g.push_back(x); if (g.size() >= 25) break; }
        for (auto &x : lib.artists()) { ar.push_back(x); if (ar.size() >= 40) break; }
        out["genres"] = g;
        out["top_artists"] = ar;
        return out.dump(-1, ' ', false, json::error_handler_t::replace);
    }
    if (action != "play" && action != "search") return fail("action must be play, search, stop, next, now_playing or library");

    if (lib.size() == 0) return fail("the music library is empty; songs go in " + Settings::get().eff_music_dir());
    std::vector<Track> found = lib.find(str_arg(a, "query"), str_arg(a, "artist"), str_arg(a, "genre"),
                                        str_arg(a, "title"), str_arg(a, "album"));
    if (found.empty()) {
        json g = json::array();
        for (auto &x : lib.genres()) { g.push_back(x); if (g.size() >= 20) break; }
        return json{{"error", "nothing in the library matches, so nothing was started"},
                    {"hint", "tell the user it is not in their library; these genres are"},
                    {"genres", g}}.dump(-1, ' ', false, json::error_handler_t::replace);
    }
    if (action == "search") {
        json arr = json::array();
        for (auto &t : found) {
            arr.push_back(t.title + (t.artist.empty() ? "" : " - " + t.artist) + (t.genre.empty() ? "" : " [" + t.genre + "]"));
            if (arr.size() >= 15) break;
        }
        return json{{"matches", found.size()}, {"first", arr}}.dump(-1, ' ', false, json::error_handler_t::replace);
    }
    const bool one = str_arg(a, "title").size() > 0 && found.size() <= 3;
    if (bool_arg(a, "shuffle", !one)) {
        static std::mt19937 rng(std::random_device{}());
        std::shuffle(found.begin(), found.end(), rng);
    }
    if (found.size() > 60) found.resize(60);
    std::vector<std::string> paths;
    for (auto &t : found) paths.push_back(t.path);
    if (!dry_run()) {
        // Queue it now, start it once the reply has been spoken (the agent or the speech queue
        // lifts the hold), so the song does not begin, stop for the reply and begin again.
        if (ctx.in_conversation) player.hold(audio::Music::kExchange, true);
        player.play(paths);
    }
    const Track &first = found[0];
    return json{{"now_playing", first.title + (first.artist.empty() ? "" : " by " + first.artist)},
                {"queued_tracks", found.size()}}.dump(-1, ' ', false, json::error_handler_t::replace);
}

// ---------------------------------------------------------------- TV remote
// ---------------------------------------------------------------- breathing
std::string t_breathe(const json &a, Context &ctx) {
    const std::string wanted = util::lower(str_arg(a, "pattern"));
    const breath::Pattern &p = breath::pattern(wanted);
    double minutes = breath::kDefaultMinutes;
    num_arg(a, "minutes", minutes);
    const int m = breath::clamp_minutes(minutes);
    if (ctx.on_breathe) ctx.on_breathe(p.id, m);
    return json{{"started", p.title}, {"minutes", m},
                {"note", "The exercise appears on screen as soon as you finish speaking, and the user follows the orb. "
                         "Say one short, calm sentence and nothing more."}}.dump();
}

// ---------------------------------------------------------------- smart home
std::string t_smart_home(const json &a, Context &) {
    const std::string action = util::lower(str_arg(a, "action")), device = str_arg(a, "device");
    const bool everything = util::lower(device) == "all" || util::lower(device) == "everything";
    if (action == "status" || action == "list") return home::status(everything ? "" : device);
    if (action != "on" && action != "off") return fail("action must be on, off or status");
    if (device.empty()) return fail("say which device");
    if (everything) {
        // Off is always safe to do in one go; on is not (one of them may be a heater).
        if (action == "on") return fail("everything can be switched off together but not on; ask which device to switch on");
        if (dry_run()) return json{{"switched_off", home::switch_names()}}.dump();
        return home::switch_all_off();
    }
    if (dry_run()) {
        // Tests check the choice of device without switching anything in the house.
        for (const std::string &name : home::switch_names())
            if (util::lower(name) == util::lower(device)) return json{{"device", name}, {"now", action}}.dump();
        return fail("there is no device called '" + device + "'");
    }
    return home::switch_power(device, action == "on");
}

std::string t_tv_remote(const json &a, Context &) {
    const std::string key = util::lower(str_arg(a, "key"));
    if (key.empty()) return fail("key is required");
    double repeat = 1;
    num_arg(a, "repeat", repeat);
    std::string brand = util::lower(str_arg(a, "brand"));
    Settings &s = Settings::get();
    if (!brand.empty() && brand != s.tv_brand) {  // remember a brand the user mentions
        const auto known = ir::brands();
        if (std::find(known.begin(), known.end(), brand) != known.end()) { s.tv_brand = brand; s.save(); }
    }
    if (brand.empty()) brand = s.tv_brand;
    const std::string err = ir::send(brand, key, static_cast<int>(repeat));
    if (!err.empty()) return fail(err);
    return "sent '" + key + "'" + (repeat > 1 ? " x" + std::to_string(static_cast<int>(repeat)) : "") + " (" +
           (brand.empty() || brand == "auto" ? "all known brands" : brand) +
           "). The device cannot see whether the TV reacted.";
}

// ---------------------------------------------------------------- device
int read_int_file(const char *path, int fallback) {
    std::ifstream f(path);
    int v;
    return f >> v ? v : fallback;
}

std::string t_device(const json &a, Context &) {
    const std::string setting = util::lower(str_arg(a, "setting"));
    Settings &s = Settings::get();
    if (setting.empty() || setting == "status") {
        json out = {{"volume_percent", s.volume}, {"brightness_percent", s.brightness},
                    {"spoken_replies", s.speak_replies}, {"timezone", s.timezone.empty() ? util::system_timezone() : s.timezone}};
        const int batt = read_int_file("/sys/class/power_supply/bq27220-0/capacity", -1);
        if (batt >= 0) out["battery_percent"] = batt;
        const proc::Result wifi = proc::run({"nmcli", "-t", "-f", "ACTIVE,SSID,SIGNAL", "dev", "wifi"}, "", 4000);
        for (auto &line : util::split(wifi.out, '\n'))
            if (line.compare(0, 4, "yes:") == 0) out["wifi"] = line.substr(4);
        return out.dump();
    }
    double value;
    const bool has_value = num_arg(a, "value", value);
    if (setting == "volume" || setting == "brightness") {
        if (!has_value) return fail("value (0 to 100) is required");
        const int v = std::max(0, std::min(100, static_cast<int>(value)));
        if (setting == "volume") { s.volume = v; if (!dry_run()) audio::set_volume(v); }
        else { s.brightness = std::max(5, v); display::set_brightness(s.brightness); }
        s.save();
        return setting + " set to " + std::to_string(setting == "volume" ? s.volume : s.brightness) + "%";
    }
    if (setting == "spoken_replies") {
        s.speak_replies = a.contains("on") ? bool_arg(a, "on", true) : (has_value ? value != 0 : !s.speak_replies);
        s.save();
        return std::string("spoken replies ") + (s.speak_replies ? "on" : "off");
    }
    return fail("setting must be status, volume, brightness or spoken_replies");
}

// ---------------------------------------------------------------- registry
json obj(std::initializer_list<std::pair<const std::string, json>> props, std::vector<std::string> required = {}) {
    json p = json::object();
    for (auto &kv : props) p[kv.first] = kv.second;
    return {{"type", "object"}, {"properties", p}, {"required", required}};
}
json S(const std::string &desc) { return {{"type", "string"}, {"description", desc}}; }
json N(const std::string &desc) { return {{"type", "number"}, {"description", desc}}; }
json B(const std::string &desc) { return {{"type", "boolean"}, {"description", desc}}; }
json E(const std::string &desc, std::vector<std::string> values) {
    return {{"type", "string"}, {"description", desc}, {"enum", values}};
}

// Tools whose effect is fully known from the request (a timer set, a button pressed). The
// model supplies the confirmation up front in "say", and when the tool succeeds that is spoken
// straight away instead of paying for a second model call just to say "done".
const char *const kConfirmable[] = {"calendar_add", "calendar_update", "calendar_delete", "reminder_set", "reminder_cancel",
                                    "memory_save", "memory_delete", "music", "tv_remote", "device", "smart_home", "breathe"};

std::vector<Tool> build() {
    std::vector<Tool> list = {
        {"calculator", "Calculating",
         "Evaluate arithmetic exactly. Use it for every sum beyond single-digit arithmetic: tips, splits, "
         "percentages, unit conversions. Not for dates (use date_info). Supports + - * / ^ %, brackets, sqrt, sin, cos, tan, ln, log, exp, abs, round, "
         "min, max, factorial (5!), pi and e. '15% of 80' is written 0.15*80.",
         obj({{"expression", S("The expression, e.g. \"(84.50 + 12) * 1.125 / 4\"")}}, {"expression"}), t_calculator},
        {"run_python", "Running Python",
         "Run Python 3 in an isolated sandbox and return what it prints. Use it for multi-step calculations, "
         "text processing or anything calculator and date_info cannot do. The sandbox has the standard library "
         "only, no internet and no access to the user's files or the other tools; each call starts fresh. Always "
         "print() the result. Time limit 20 seconds.",
         obj({{"code", S("Complete Python source code")}}, {"code"}), t_python},
        {"date_info", "Checking the date",
         "Exact calendar facts about a date: its weekday and how many days it is from today. Use it for every "
         "'how many days until', 'how long ago', 'what day of the week is', 'what is the date N days from now' "
         "question; counting days in your head goes wrong.",
         obj({{"date", S("The date as YYYY-MM-DD, e.g. the coming 25 December for Christmas. Omit for today.")},
              {"add_days", N("Optional: move this many days on from the date first (negative for back)")}}), t_date_info},
        {"look", "Looking",
         "Take a photo with the device camera (on the back, pointing away from the user) and answer a question "
         "about it. Use it whenever the user refers to something in front of them: 'what is this', 'read this', "
         "'translate this sign or menu', 'how much is this'. Each call takes a new photo.",
         obj({{"question", S("What to find out from the photo, with any detail the answer needs, e.g. \"Transcribe "
                             "all text on the menu and translate it into English\"")}}, {"question"}), t_look},
        {"calendar_add", "Adding to calendar",
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
             {"title", "start"}), t_calendar_add},
        {"calendar_list", "Checking calendar",
         "Look at the calendar: either a date range (the next 14 days by default), or with search the next time "
         "an event with that name comes up, anywhere in the coming year. Use search for 'when is dad's birthday', "
         "'when is my dentist appointment'. Results carry ids, and repeating events appear once for each time "
         "they fall in a range.",
         obj({{"search", S("Optional words from the event's name, e.g. \"dad\" or \"dentist\"")},
              {"from", S("Optional first day, YYYY-MM-DD")}, {"to", S("Optional last day (inclusive), YYYY-MM-DD")}}),
         t_calendar_list},
        {"calendar_update", "Updating calendar",
         "Change an existing calendar event (move it, rename it, make it repeat). Only the fields given are "
         "changed, and a repeating event changes as a whole. The id is in the result of the earlier calendar_add "
         "or calendar_list; do not add a second event to move one.",
         obj({{"id", N("Event id")}, {"title", S("New title")}, {"start", S("New start, YYYY-MM-DD HH:MM")},
              {"end", S("New end, YYYY-MM-DD HH:MM")}, {"notes", S("New notes")},
              {"all_day", B("Whether it is a whole-day event")},
              {"repeat", E("How often it repeats; none stops it repeating", {"none", "daily", "weekly", "monthly", "yearly"})},
              {"remind_minutes_before", N("New alert lead time in minutes; -1 removes the alert")}}, {"id"}),
         t_calendar_update},
        {"calendar_delete", "Updating calendar", "Delete a calendar event by id (from calendar_list). A repeating event goes as a whole.",
         obj({{"id", N("Event id")}}, {"id"}), t_calendar_delete},
        {"reminder_set", "Setting a reminder",
         "Set a timer, alarm or reminder that makes the device chime and speak. Calling this is the only way one "
         "gets set. Give in_minutes for a timer ('in 10 minutes', 90 seconds = 1.5) or at for a clock time. If the "
         "user gave no time at all, ask when. It fires only while this app is running. For an appointment that "
         "belongs in the calendar, use calendar_add with remind_minutes_before instead.",
         obj({{"text", S("What to say when it fires, e.g. \"Check the laundry\"")},
              {"in_minutes", N("Fire this many minutes from now (fractions allowed)")},
              {"at", S("Or fire at this local time, YYYY-MM-DD HH:MM")}}, {"text"}), t_reminder_set},
        {"reminder_list", "Checking reminders", "List pending timers and reminders with their ids.", obj({}),
         t_reminder_list},
        {"reminder_cancel", "Cancelling a reminder", "Cancel a pending timer or reminder by id (from reminder_list).",
         obj({{"id", N("Reminder id")}}, {"id"}), t_reminder_cancel},
        {"memory_save", "Remembering",
         "Save a lasting fact about the user so it is available in every future conversation: preferences, "
         "diet, allergies, names, travel plans, anything they ask you to remember. Calling this is the only way "
         "something is remembered; nothing is kept otherwise. One fact per call, written so it makes sense on its "
         "own. Use replace_id to correct an existing memory instead of adding a duplicate.",
         obj({{"text", S("The fact, e.g. \"Allergic to shellfish\"")},
              {"replace_id", N("Optional id of an existing memory to overwrite")}}, {"text"}), t_memory_save},
        {"memory_delete", "Forgetting", "Delete a saved memory by id when the user asks you to forget it or it is wrong.",
         obj({{"id", N("Memory id as listed in your instructions")}}, {"id"}), t_memory_delete},
        {"web_search", "Searching the web",
         "Search the web. Always use it for details that change or are easy to misremember: opening hours and "
         "days, ticket prices, timetables, travel rules, news, sports results, who currently holds a post, "
         "anything recent. Not needed for settled general knowledge. Returns a short summary and source snippets.",
         obj({{"query", S("A specific search query including place names and dates where relevant")}}, {"query"}),
         t_web_search},
        {"weather", "Checking the weather",
         "Current weather and a daily forecast for a place.",
         obj({{"location", S("City or town, optionally with country. Omit to use the user's home city.")},
              {"days", N("Forecast length in days, 1 to 7 (default 3)")}}), t_weather},
        {"convert_currency", "Converting currency",
         "Convert an amount between currencies using a current exchange rate.",
         obj({{"amount", N("Amount to convert")}, {"from", S("3-letter code of the source currency, e.g. EUR")},
              {"to", S("3-letter code of the target currency, e.g. GBP")}}, {"amount", "from", "to"}), t_currency},
        {"stock_price", "Checking the price",
         "Latest price for a share, index, fund or cryptocurrency, with the day's change and range.",
         obj({{"symbol", S("A ticker (AAPL, RR.L, ^FTSE, BTC-USD) or a company name (\"Rolls-Royce\")")}}, {"symbol"}),
         t_stock},
        {"world_time", "Checking the time",
         "The current time in another place, or what a given local time corresponds to there. The current local "
         "date and time are already in your instructions; do not call this for the user's own time.",
         obj({{"place", S("City or IANA zone, e.g. \"Tokyo\" or \"America/New_York\"")},
              {"local_time", S("Optional local time to convert, YYYY-MM-DD HH:MM; omit for now")}}, {"place"}),
         t_world_time},
        {"set_timezone", "Changing time zone",
         "Change the time zone this device uses, for when the user has travelled or asks to switch zone.",
         obj({{"place", S("City or IANA zone the user is now in")}}, {"place"}), t_set_timezone},
        {"music", "Music",
         "Control music from the user's own library on this device. action=play starts playback of everything "
         "matching the filters (all filters optional; none = shuffle the whole library) and returns the track that "
         "actually started; tell the user that track. action=search lists matches without playing. Other actions: "
         "stop, next, now_playing, library (counts, genres and artists).",
         obj({{"action", E("What to do", {"play", "search", "stop", "next", "now_playing", "library"})},
              {"artist", S("Artist name or part of it, e.g. \"ABBA\"")},
              {"genre", S("Genre or part of it, e.g. \"Rock\"")},
              {"title", S("Song title or part of it")},
              {"album", S("Album name or part of it")},
              {"query", S("Loose words matched against title, artist, album and genre together")},
              {"shuffle", B("Shuffle the matches (default true unless one specific song was asked for)")}},
             {"action"}), t_music},
        {"tv_remote", "Using the remote",
         "Press a button on a TV using the device's infrared transmitter (the user points the top edge of the "
         "device at the TV). Send one key per call; use repeat for several presses, e.g. volume_up with repeat 5. "
         "Works without knowing the brand; pass brand if the user says it.",
         obj({{"key", E("Button to press", {"power", "volume_up", "volume_down", "mute", "channel_up", "channel_down",
                                            "input", "up", "down", "left", "right", "ok", "menu", "back", "home",
                                            "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"})},
              {"repeat", N("How many times to press (default 1)")},
              {"brand", S("Optional TV brand: samsung, lg, sony, panasonic, philips, toshiba, vizio")}}, {"key"}),
         t_tv_remote},
        {"breathe", "Starting a breathing exercise",
         "Show a guided breathing exercise on the screen: an orb grows and shrinks to pace the breath. Use it when "
         "the user says they are stressed, anxious, overwhelmed, cannot settle or sleep, or asks to meditate, "
         "breathe or calm down. Start it straight away; do not ask which kind or for how long unless they bring it "
         "up, and do not give advice instead.",
         obj({{"pattern", E("box: in, hold, out, hold for four seconds each (the default). calm: slow breathing, "
                            "in for four, out for six. sigh: two breaths in and one long breath out, good for "
                            "winding down", {"box", "calm", "sigh"})},
              {"minutes", N("Length in minutes, 1 to 20. Leave out for the usual five")}}),
         t_breathe},
        {"smart_home", "Using a home device",
         "Switch one of the user's smart-home devices on this network on or off, or check whether it is on. "
         "Calling this is the only way a device gets switched. The devices that exist are listed in the device "
         "parameter; if what the user asks for is not one of them, say so instead of picking another.",
         obj({{"action", E("What to do; status with no device reports all of them", {"on", "off", "status"})},
              {"device", S("The device's name, exactly as listed. \"all\" means every device, for switching "
                           "everything off or checking everything")}}, {"action"}),
         t_smart_home},
        {"device", "Adjusting the device",
         "Read or change this device's own settings. setting=status reports battery, Wi-Fi, volume and brightness.",
         obj({{"setting", E("Which setting", {"status", "volume", "brightness", "spoken_replies"})},
              {"value", N("For volume or brightness: 0 to 100")},
              {"on", B("For spoken_replies: true to speak answers aloud, false for text only")}}, {"setting"}),
         t_device},
    };
    for (Tool &t : list) {
        if (std::find_if(std::begin(kConfirmable), std::end(kConfirmable), [&](const char *n) { return t.name == n; }) ==
            std::end(kConfirmable))
            continue;
        std::string desc = "The one short sentence to speak to the user once this has worked, in their language, with "
                           "the detail that matters, e.g. \"Your timer is set for 12 minutes.\"";
        if (t.name == "music") desc += " Only for stop and next; omit for the other actions.";
        if (t.name == "device") desc += " Omit for status.";
        if (t.name == "smart_home") desc += " Only for on and off; omit for status.";
        t.parameters["properties"]["say"] = S(desc);
    }
    return list;
}

}  // namespace

const std::vector<Tool> &all() {
    static const std::vector<Tool> t = build();
    return t;
}

json schema() {
    json arr = json::array();
    // A tool that can only fail is a wasted round trip; the prompt says search is unavailable.
    const bool no_search = Settings::get().search_key.empty();
    // The smart-home tool exists only on a network the user has scanned, and names exactly the
    // devices saved for it. The names are passed as allowed values, as data, never as instructions.
    const std::vector<std::string> home_devices = home::switch_names();
    for (auto &t : all()) {
        if (no_search && t.name == "web_search") continue;
        json parameters = t.parameters;
        if (t.name == "smart_home") {
            if (home_devices.empty()) continue;
            std::vector<std::string> allowed = home_devices;
            allowed.push_back("all");
            parameters["properties"]["device"]["enum"] = allowed;
        }
        arr.push_back({{"type", "function"},
                       {"function", {{"name", t.name}, {"description", t.description}, {"parameters", parameters}}}});
    }
    return arr;
}

const Tool *find(const std::string &name) {
    for (auto &t : all())
        if (t.name == name) return &t;
    return nullptr;
}

std::string confirmation(const std::string &name, const std::string &raw_args, const std::string &result) {
    if (std::find_if(std::begin(kConfirmable), std::end(kConfirmable), [&](const char *n) { return name == n; }) ==
        std::end(kConfirmable))
        return "";
    if (result.compare(0, 5, "ERROR") == 0 || result.compare(0, 9, "{\"error\"") == 0) return "";
    const json args = json::parse(raw_args.empty() ? "{}" : raw_args, nullptr, false);
    if (!args.is_object()) return "";
    // These return something the model has not seen yet (the track that started, the battery
    // level), so the answer has to be written after the result.
    if (name == "music") {
        const std::string action = util::lower(str_arg(args, "action"));
        if (action != "stop" && action != "next") return "";
    }
    if (name == "smart_home" && util::lower(str_arg(args, "action")) != "on" && util::lower(str_arg(args, "action")) != "off") return "";
    if (name == "device") {
        const std::string setting = util::lower(str_arg(args, "setting"));
        if (setting.empty() || setting == "status") return "";
    }
    return str_arg(args, "say");
}

std::string run(const std::string &name, const std::string &raw_args, Context &ctx) {
    const Tool *tool = find(name);
    if (!tool) return fail("there is no tool called '" + name + "'");
    json args = json::parse(raw_args.empty() ? "{}" : raw_args, nullptr, false);
    if (args.is_discarded() || !args.is_object()) return fail("the arguments were not valid JSON; send a JSON object");
    try {
        return util::sanitize_utf8(tool->run(args, ctx));
    } catch (const std::exception &e) {
        return fail(std::string("the tool failed: ") + e.what());
    }
}

}  // namespace tools
