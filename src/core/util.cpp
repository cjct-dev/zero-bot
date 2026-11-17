#include "core/util.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace util {

std::string home_dir() {
    const char *h = getenv("HOME");
    return h && *h ? h : "/tmp";
}

static std::string xdg(const char *var, const char *fallback) {
    const char *v = getenv(var);
    std::string base = v && *v ? v : home_dir() + fallback;
    return base + "/zero-bot";
}

std::string config_dir() { return xdg("XDG_CONFIG_HOME", "/.config"); }
std::string data_dir() { return xdg("XDG_DATA_HOME", "/.local/share"); }
std::string cache_dir() { return xdg("XDG_CACHE_HOME", "/.cache"); }

void make_dirs(const std::string &path) {
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!cur.empty()) mkdir(cur.c_str(), 0700);
        }
        if (i < path.size()) cur += path[i];
    }
}

bool read_file(const std::string &path, std::string &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool write_file_atomic(const std::string &path, const std::string &data, int mode) {
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos) make_dirs(path.substr(0, slash));
    const std::string tmp = path + ".tmp";
    FILE *fp = fopen(tmp.c_str(), "wb");
    if (!fp) return false;
    fchmod(fileno(fp), static_cast<mode_t>(mode));
    const bool ok = fwrite(data.data(), 1, data.size(), fp) == data.size();
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    if (!ok) { unlink(tmp.c_str()); return false; }
    return rename(tmp.c_str(), path.c_str()) == 0;
}

json load_json(const std::string &path, const json &fallback) {
    std::string text;
    if (!read_file(path, text)) return fallback;
    json j = json::parse(text, nullptr, false);
    return j.is_discarded() ? fallback : j;
}

bool save_json(const std::string &path, const json &value) {
    return write_file_atomic(path, value.dump(2, ' ', false, json::error_handler_t::replace));
}

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower(const std::string &s) {
    std::string out = s;
    for (char &c : out) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return out;
}

std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

bool contains_ci(const std::string &haystack, const std::string &needle) {
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

std::string truncate_utf8(const std::string &s, size_t max_bytes, const char *ellipsis) {
    if (s.size() <= max_bytes) return s;
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + ellipsis;
}

std::string sanitize_utf8(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        bool good = len > 0 && i + len <= s.size();
        for (size_t k = 1; good && k < len; ++k)
            good = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
        if (good) { out.append(s, i, len); i += len; }
        else { out += '?'; ++i; }
    }
    return out;
}

std::string base64(const std::string &data) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < data.size()) {
        const uint32_t v = (static_cast<unsigned char>(data[i]) << 16) | (static_cast<unsigned char>(data[i + 1]) << 8) |
                           static_cast<unsigned char>(data[i + 2]);
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += tbl[v & 63];
        i += 3;
    }
    if (i + 1 == data.size()) {
        const uint32_t v = static_cast<unsigned char>(data[i]) << 16;
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == data.size()) {
        const uint32_t v = (static_cast<unsigned char>(data[i]) << 16) | (static_cast<unsigned char>(data[i + 1]) << 8);
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += '=';
    }
    return out;
}

// ---------------------------------------------------------------- time
namespace {
std::mutex g_tz_mtx;
std::string g_app_tz;  // "" = system

struct TzScope {  // temporarily switch TZ; callers hold g_tz_mtx
    explicit TzScope(const std::string &iana) {
        if (iana.empty()) unsetenv("TZ"); else setenv("TZ", (":" + iana).c_str(), 1);
        tzset();
    }
    ~TzScope() {
        if (g_app_tz.empty()) unsetenv("TZ"); else setenv("TZ", (":" + g_app_tz).c_str(), 1);
        tzset();
    }
};
}  // namespace

void apply_timezone(const std::string &iana) {
    std::lock_guard<std::mutex> lk(g_tz_mtx);
    g_app_tz = timezone_exists(iana) ? iana : "";
    if (g_app_tz.empty()) unsetenv("TZ"); else setenv("TZ", (":" + g_app_tz).c_str(), 1);
    tzset();
}

std::string system_timezone() {
    char buf[256];
    const ssize_t n = readlink("/etc/localtime", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        const char *p = strstr(buf, "zoneinfo/");
        if (p) return p + 9;
    }
    std::string text;
    if (read_file("/etc/timezone", text)) return trim(text);
    return "UTC";
}

bool timezone_exists(const std::string &iana) {
    if (iana.empty() || iana.find("..") != std::string::npos || iana[0] == '/') return false;
    return access(("/usr/share/zoneinfo/" + iana).c_str(), R_OK) == 0;
}

std::string format_time(time_t t, const char *fmt) {
    std::lock_guard<std::mutex> lk(g_tz_mtx);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[96];
    strftime(buf, sizeof(buf), fmt, &tmv);
    return buf;
}

std::string format_time_in(time_t t, const char *fmt, const std::string &iana) {
    std::lock_guard<std::mutex> lk(g_tz_mtx);
    TzScope scope(iana);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[96];
    strftime(buf, sizeof(buf), fmt, &tmv);
    return buf;
}

std::string iso_local(time_t t) { return format_time(t, "%Y-%m-%dT%H:%M"); }

bool parse_local(const std::string &text, time_t &out) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    std::string t = trim(text);
    for (char &c : t)
        if (c == 'T') c = ' ';
    const int n = sscanf(t.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s);
    if (n < 3 || y < 1900 || mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    if (n < 5) { h = n == 4 ? h : 0; mi = 0; }
    if (n < 6) s = 0;
    std::lock_guard<std::mutex> lk(g_tz_mtx);
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = y - 1900; tmv.tm_mon = mo - 1; tmv.tm_mday = d;
    tmv.tm_hour = h; tmv.tm_min = mi; tmv.tm_sec = s; tmv.tm_isdst = -1;
    out = mktime(&tmv);
    return out != static_cast<time_t>(-1);
}

LocalTime local_fields(time_t t) {
    LocalTime f;
    sscanf(format_time(t, "%Y %m %d %H %M").c_str(), "%d %d %d %d %d", &f.year, &f.month, &f.day, &f.hour, &f.minute);
    return f;
}

time_t make_local(int year, int month, int day, int hour, int minute) {
    std::lock_guard<std::mutex> lk(g_tz_mtx);
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = year - 1900; tmv.tm_mon = month - 1; tmv.tm_mday = day;
    tmv.tm_hour = hour; tmv.tm_min = minute; tmv.tm_isdst = -1;
    return mktime(&tmv);
}

int days_in_month(int year, int month) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return kDays[(month - 1) % 12];
}

// A two-digit year more than twenty years ahead is far likelier a birth year than a plan.
int pivot_year(int two_digits) {
    const int this_year = atoi(format_time(time(nullptr), "%Y").c_str());
    const int y = 2000 + two_digits;
    return y > this_year + 20 ? y - 100 : y;
}

static std::atomic<bool> g_month_first{false};
void set_month_first(bool month_first) { g_month_first = month_first; }
bool month_first() { return g_month_first; }

std::string short_date(time_t t) { return format_time(t, month_first() ? "%a %b %-d %H:%M" : "%a %-d %b %H:%M"); }
std::string short_day(time_t t) { return format_time(t, month_first() ? "%a %b %-d %Y" : "%a %-d %b %Y"); }
std::string typed_day(time_t t) { return format_time(t, month_first() ? "%m/%d/%Y" : "%d/%m/%Y"); }
std::string typed_date(time_t t) { return format_time(t, month_first() ? "%m/%d/%Y %H:%M" : "%d/%m/%Y %H:%M"); }
const char *typed_date_hint() {
    return month_first() ? "Month/day/year hour:minute, 24-hour clock" : "Day/month/year hour:minute, 24-hour clock";
}

bool parse_typed(const std::string &text, time_t &out) {
    std::string t = trim(text);
    for (char &c : t)
        if (c == 'T' || c == ',') c = ' ';
    int a = 0, b = 0, y = 0, h = 0, mi = 0, used = 0;
    char s1 = 0, s2 = 0;
    // A leading four-digit year is the unambiguous form whatever the setting.
    if (sscanf(t.c_str(), "%d%c", &a, &s1) == 2 && a >= 1900 && s1 == '-') return parse_local(t, out);
    auto is_sep = [](char c) { return c == '/' || c == '.' || c == '-'; };
    bool has_year = false;
    if (sscanf(t.c_str(), "%d%c%d%c%d%n", &a, &s1, &b, &s2, &y, &used) == 5 && is_sep(s1) && s2 == s1) {
        has_year = true;
        if (y < 100) y = pivot_year(y);
    } else if (sscanf(t.c_str(), "%d%c%d%n", &a, &s1, &b, &used) == 3 && is_sep(s1)) {
        y = 0;
    } else {
        return false;
    }
    const std::string rest = trim(t.substr(static_cast<size_t>(used)));
    if (!rest.empty()) {
        char colon = 0;
        const int n = sscanf(rest.c_str(), "%d%c%d", &h, &colon, &mi);
        if (n == 1) mi = 0;
        else if (n != 3 || (colon != ':' && colon != '.')) return false;
        if (h < 0 || h > 23 || mi < 0 || mi > 59) return false;
    }
    const int d = month_first() ? b : a, mo = month_first() ? a : b;
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    char iso[40];
    if (!has_year) {
        // No year given: the next time that date comes round.
        const int this_year = atoi(format_time(time(nullptr), "%Y").c_str());
        const std::string today = format_time(time(nullptr), "%m-%d");
        snprintf(iso, sizeof(iso), "%02d-%02d", mo, d);
        y = iso < today ? this_year + 1 : this_year;
    }
    if (y < 1900) return false;
    snprintf(iso, sizeof(iso), "%04d-%02d-%02d %02d:%02d", y, mo, d, h, mi);
    if (!parse_local(iso, out)) return false;
    return format_time(out, "%d") == (d < 10 ? "0" : "") + std::to_string(d);   // rejects 31/02
}

std::string spell_numeric_dates(const std::string &text) {
    // day/month or month/day, optionally with a year; "." and "-" only count in the three-part form,
    // where they cannot be a decimal or a range.
    static const std::regex pattern(R"((\d{1,2})([/.\-])(\d{1,2})(?:\2(\d{4}|\d{2}))?(?![0-9])(?![/.\-][0-9]))");
    static const char *const kCues[] = {"on", "for", "from", "until", "till", "by", "since", "before", "after", "date",
                                        "dated", "starting", "ending", "through", "to", "el", "le", "am", "il", "em"};
    // A message that is plainly about a date makes any day/month pair in it one.
    bool about_dates = false;
    {
        const std::string low = lower(text);
        for (const char *word : {"birthday", "anniversary", "born", "calendar", "appointment", "remind", "meeting", "flight"})
            about_dates = about_dates || low.find(word) != std::string::npos;
    }
    std::string out;
    size_t copied = 0;
    for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
        const std::smatch &m = *it;
        const size_t at = static_cast<size_t>(m.position(0)), after = at + static_cast<size_t>(m.length(0));
        if (at > 0 && (isalnum(static_cast<unsigned char>(text[at - 1])) || strchr("/.-", text[at - 1]))) continue;
        const bool has_year = m[4].matched;
        if (!has_year) {
            if (m[2].str() != "/") continue;
            // Two numbers and a slash is as likely a fraction; take it as a date only where one is expected.
            size_t w_end = at;
            while (w_end > 0 && text[w_end - 1] == ' ') --w_end;
            size_t w_start = w_end;
            while (w_start > 0 && isalpha(static_cast<unsigned char>(text[w_start - 1]))) --w_start;
            const std::string before = lower(text.substr(w_start, w_end - w_start));
            bool cued = about_dates;
            for (const char *cue : kCues) cued = cued || before == cue;
            const std::string rest = lower(text.substr(after, 4));
            cued = cued || rest == " at " || rest == " at";
            cued = cued || (copied > 0 && (before == "and" || before == "or"));   // "on 3/4 and 5/4"
            if (!cued) continue;
        }
        const int a = atoi(m[1].str().c_str()), b = atoi(m[3].str().c_str());
        int d = month_first() ? b : a, mo = month_first() ? a : b;
        if (mo > 12 && d <= 12) std::swap(d, mo);   // only one reading is a real date
        if (mo < 1 || mo > 12 || d < 1 || d > 31) continue;
        int y = has_year ? atoi(m[4].str().c_str()) : 0;
        if (has_year && y < 100) y = pivot_year(y);
        char iso[40];
        if (!has_year) {
            snprintf(iso, sizeof(iso), "%02d-%02d", mo, d);
            const int this_year = atoi(format_time(time(nullptr), "%Y").c_str());
            y = iso < format_time(time(nullptr), "%m-%d") ? this_year + 1 : this_year;
        }
        time_t when = 0;
        snprintf(iso, sizeof(iso), "%04d-%02d-%02d 12:00", y, mo, d);
        if (y < 1900 || !parse_local(iso, when) || atoi(format_time(when, "%d").c_str()) != d) continue;
        out += text.substr(copied, after - copied);
        out += " (" + format_time(when, month_first() ? "%A, %B %-d, %Y" : "%A %-d %B %Y") + ")";
        copied = after;
    }
    return copied == 0 ? text : out + text.substr(copied);
}

std::string friendly_day(time_t t) {
    const time_t now = time(nullptr);
    const std::string day = format_time(t, "%Y-%m-%d");
    if (day == format_time(now, "%Y-%m-%d")) return "Today";
    if (day == format_time(now + 86400, "%Y-%m-%d")) return "Tomorrow";
    return format_time(t, month_first() ? "%a %b %-d" : "%a %-d %b");
}

std::string friendly_when(time_t t) { return friendly_day(t) + " " + format_time(t, "%H:%M"); }

}  // namespace util
