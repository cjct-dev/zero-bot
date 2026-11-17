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

std::string trim(const std::string &s);
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
std::string friendly_when(time_t t);                  // "Today 18:30", "Tue 18 Nov 09:00"

template <typename T>
T jget(const json &j, const char *key, const T &fallback) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    try { return it->get<T>(); } catch (...) { return fallback; }
}

}  // namespace util
