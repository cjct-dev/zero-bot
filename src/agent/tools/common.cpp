// The definitions of common.h: what every tool reads its arguments with, and the schema builders.
#include "agent/tools/common.h"

#include <cmath>
#include <cstdlib>

namespace tools {

bool dry_run() { return getenv("ZB_DRY_RUN") != nullptr; }

// Text arguments. A number sent where text was expected is taken as its digits, so that a date
// sent as 20261225 is refused as a date instead of being read as no date at all (which meant
// today), and a TV key sent as 5 is the key "5".
std::string str_arg(const json &a, const char *key) {
    const auto it = a.find(key);
    if (it == a.end() || it->is_null()) return "";
    if (it->is_string()) return util::trim(it->get<std::string>());
    if (it->is_number()) return it->dump();
    return "";
}

bool given(const json &a, const char *key) {
    const auto it = a.find(key);
    return it != a.end() && !it->is_null();
}

// Models sometimes send numbers as strings; accept both. Only finite numbers count: "nan" and
// "inf" parse as numbers, and converting them (or 1e300) to a whole number is undefined.
bool num_arg(const json &a, const char *key, double &out) {
    auto it = a.find(key);
    if (it == a.end() || it->is_null()) return false;
    if (it->is_number()) { out = it->get<double>(); return std::isfinite(out); }
    if (it->is_string()) {
        char *end = nullptr;
        const std::string s = it->get<std::string>();
        out = strtod(s.c_str(), &end);
        return end != s.c_str() && std::isfinite(out);
    }
    return false;
}

// A finite value brought into [lo, hi] before it becomes an int.
int clamp_int(double v, int lo, int hi) {
    if (!(v > lo)) return lo;
    if (v >= hi) return hi;
    return static_cast<int>(v);
}

// An id from an earlier result. A missing id and one that is not a number get different answers,
// so that the model looks the id up instead of sending the same thing again.
bool id_arg(const json &a, const char *key, int &id, std::string &err, const std::string &where) {
    double v = 0;
    if (!given(a, key)) { err = std::string(key) + " is required; " + where; return false; }
    if (!num_arg(a, key, v) || v != std::floor(v) || v < 1 || v > 1e9) {
        err = std::string(key) + " must be a whole number such as 12; " + where;
        return false;
    }
    id = static_cast<int>(v);
    return true;
}

// Text that is kept and shown again (titles, notes, reminders, memories): control characters, line
// breaks included, become single spaces. Memories go into the instructions of every later
// request, where a line break could pass for a heading or an entry of its own.
std::string clean_text(const std::string &raw) {
    std::string s;
    for (char c : raw) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) c = ' ';
        if (c == ' ' && (s.empty() || s.back() == ' ')) continue;
        s += c;
    }
    return util::trim(s);
}

// The error for a date or time that is malformed or not real (31 February, 25:00), with what
// was sent, so the model can see which part to change.
std::string bad_time(const std::string &field, const std::string &sent, bool date_only) {
    return field + " must be a real " +
           (date_only ? std::string("date written like 2026-03-14") : std::string("date and time written like 2026-03-14 18:30 (24-hour clock)")) +
           (sent.empty() ? "" : "; '" + util::truncate_utf8(sent, 40) + "' is not one");
}

bool bool_arg(const json &a, const char *key, bool fallback) {
    auto it = a.find(key);
    if (it == a.end()) return fallback;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_string()) return util::lower(it->get<std::string>()) == "true";
    return fallback;
}

json obj(std::initializer_list<std::pair<const std::string, json>> props, std::vector<std::string> required) {
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

}  // namespace tools
