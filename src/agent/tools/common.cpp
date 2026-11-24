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

// Models sometimes send numbers as strings; accept both, text only when it is a number as a whole
// (util::to_number, as the runner's checks read it: "12oops" is not reminder 12). Only finite numbers
// count: "nan" and "inf" parse as numbers, and converting them (or 1e300) to a whole number is undefined.
bool num_arg(const json &a, const char *key, double &out) {
    auto it = a.find(key);
    if (it == a.end() || it->is_null()) return false;
    if (it->is_number()) { out = it->get<double>(); return std::isfinite(out); }
    if (it->is_string()) return util::to_number(it->get<std::string>(), out);
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

namespace {

// Characters that show as nothing, so that words can hide in them, unseen by the user and read by the model:
// the soft hyphen, the zero-width space, non-joiner and joiner, the word joiner and the invisible operators,
// the byte-order mark, the Mongolian vowel separator, the bidirectional marks, embeddings, overrides and
// isolates, the variation selectors, and Unicode's tag characters (U+E0000 to U+E007F), which can spell out
// a whole sentence no screen shows.
bool hidden(uint32_t cp) {
    return cp == 0x00AD || cp == 0x061C || cp == 0x180E || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
           (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0x2066 && cp <= 0x2069) || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF ||
           (cp >= 0xE0000 && cp <= 0xE007F) || (cp >= 0xE0100 && cp <= 0xE01EF);
}

// Control characters (C0, DEL, C1) and the line and paragraph separators: each becomes a space.
bool breaks(uint32_t cp) { return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0x2028 || cp == 0x2029; }

}  // namespace

// Text that is kept and shown again (titles, notes, reminders, memories): control characters, line
// breaks included, become single spaces, and characters that show as nothing are removed. Memories go
// into the instructions of every later request, where a line break could pass for a heading or an entry
// of its own, and hidden characters for words the user never saw.
std::string clean_text(const std::string &raw) {
    const unsigned char *p = reinterpret_cast<const unsigned char *>(raw.data());
    std::string s;
    for (size_t i = 0; i < raw.size();) {
        uint32_t cp = 0;
        const size_t len = util::utf8_at(p, raw.size(), i, cp);
        if (!len) { s += raw[i++]; continue; }   // not UTF-8: kept as it is (tools::run makes a result valid)
        const bool space = breaks(cp) || cp == ' ';
        if (!hidden(cp) && !(space && (s.empty() || s.back() == ' '))) {
            if (space) s += ' ';
            else s.append(raw, i, len);
        }
        i += len;
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
