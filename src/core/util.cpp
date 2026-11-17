#include "core/util.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
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
    if (n < 3 || y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31) return false;
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

std::string friendly_when(time_t t) {
    const time_t now = time(nullptr);
    const std::string day = format_time(t, "%Y-%m-%d");
    if (day == format_time(now, "%Y-%m-%d")) return "Today " + format_time(t, "%H:%M");
    if (day == format_time(now + 86400, "%Y-%m-%d")) return "Tomorrow " + format_time(t, "%H:%M");
    return format_time(t, "%a %d %b %H:%M");
}

}  // namespace util
