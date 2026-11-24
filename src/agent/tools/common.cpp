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
// the soft hyphen, the zero-width space, the word joiner and the invisible operators, the byte-order mark, the
// bidirectional marks, embeddings, overrides and isolates, the variation selectors other than the text and emoji
// styles (an ideographic one is kept after an ideograph: kept_after), and Unicode's tag characters (U+E0000 to
// U+E007F), which can spell out a whole sentence no screen shows. The zero-width joiner and non-joiner are not
// hidden: emoji and Persian, among others, need them (kept_after says where).
bool hidden(uint32_t cp) {
    return cp == 0x00AD || cp == 0x061C || cp == 0x200B || cp == 0x200E || cp == 0x200F || (cp >= 0x202A && cp <= 0x202E) ||
           (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0x2066 && cp <= 0x2069) || (cp >= 0xFE00 && cp <= 0xFE0D) || cp == 0xFEFF ||
           (cp >= 0xE0000 && cp <= 0xE007F) || (cp >= 0xE0100 && cp <= 0xE01EF);
}

// The text and emoji styles (VS15, VS16): one is kept after a character, as an emoji or a symbol needs.
bool style(uint32_t cp) { return cp == 0xFE0E || cp == 0xFE0F; }
// The ideographic variation selectors: one is kept after a CJK ideograph, for a name's own form of it.
bool variant(uint32_t cp) { return cp >= 0xE0100 && cp <= 0xE01EF; }
bool ideograph(uint32_t cp) {
    return (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0x20000 && cp <= 0x2FA1F) || (cp >= 0x30000 && cp <= 0x323AF);
}
// The zero-width non-joiner and joiner: kept between two characters that show, as Persian and emoji need them.
bool joiner(uint32_t cp) { return cp == 0x200C || cp == 0x200D; }

// Control characters (C0, DEL, C1) and the line and paragraph separators: each becomes a space.
bool breaks(uint32_t cp) { return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0x2028 || cp == 0x2029; }

// The flags of England, Scotland and Wales, the only tag sequences there are: a black flag, the tag letters of
// the region and a cancel tag. Their length when one begins at s[i], else 0.
size_t flag_at(const std::string &s, size_t i) {
    const unsigned char *p = reinterpret_cast<const unsigned char *>(s.data());
    auto next = [&](size_t &at, uint32_t &cp) {
        const size_t len = at < s.size() ? util::utf8_at(p, s.size(), at, cp) : 0;
        at += len;
        return len > 0;
    };
    size_t at = i;
    uint32_t cp = 0;
    if (!next(at, cp) || cp != 0x1F3F4) return 0;
    const size_t after_flag = at;
    for (const char *region : {"gbeng", "gbsct", "gbwls"}) {
        at = after_flag;
        bool same = true;
        for (const char *r = region; *r && same; ++r) same = next(at, cp) && cp == 0xE0000u + static_cast<unsigned char>(*r);
        if (same && next(at, cp) && cp == 0xE007F) return at - i;
    }
    return 0;
}

// What clean_text kept last, which decides what may follow it.
enum class Kept { Nothing, Character, Ideograph, Style, Joiner };

bool shows(uint32_t cp) { return !breaks(cp) && cp != ' ' && !hidden(cp) && !joiner(cp) && !style(cp); }

// Whether the next character clean_text keeps after s[i] shows (hidden ones, which it drops, are passed over).
bool shows_next(const std::string &s, size_t i) {
    const unsigned char *p = reinterpret_cast<const unsigned char *>(s.data());
    for (uint32_t cp = 0; i < s.size();) {
        const size_t len = util::utf8_at(p, s.size(), i, cp);
        if (!len) return false;
        if (!hidden(cp)) return shows(cp);
        i += len;
    }
    return false;
}

// Whether `cp` is kept after what was kept last: whatever shows, a style after a character that shows, an ideographic
// variant after an ideograph, a joiner between two characters that show (`next_shows`); nothing hidden. So no run of
// joiners and styles that show as nothing gets through: at most one style or variant after a character, and a joiner
// only between it and the next.
bool kept_after(uint32_t cp, Kept last, bool next_shows) {
    const bool after_shown = last == Kept::Character || last == Kept::Ideograph;
    if (style(cp)) return after_shown;
    if (variant(cp)) return last == Kept::Ideograph;
    if (joiner(cp)) return (after_shown || last == Kept::Style) && next_shows;
    return !hidden(cp);
}

Kept kind_of(uint32_t cp) {
    if (style(cp) || variant(cp)) return Kept::Style;
    if (joiner(cp)) return Kept::Joiner;
    return ideograph(cp) ? Kept::Ideograph : Kept::Character;
}

}  // namespace

// Text that is kept and shown again (titles, notes, reminders, memories): control characters, line
// breaks included, become single spaces, and characters that show as nothing are removed. Memories go
// into the instructions of every later request, where a line break could pass for a heading or an entry
// of its own, and hidden characters for words the user never saw. What ordinary text needs stays (kept_after):
// a joiner between two characters, one text or emoji style after a character, one ideographic variant after an
// ideograph, and the three flags made of tags.
std::string clean_text(const std::string &raw) {
    const unsigned char *p = reinterpret_cast<const unsigned char *>(raw.data());
    std::string s;
    Kept last = Kept::Nothing;
    for (size_t i = 0; i < raw.size();) {
        if (const size_t flag = flag_at(raw, i)) {
            s.append(raw, i, flag);
            i += flag;
            last = Kept::Character;
            continue;
        }
        uint32_t cp = 0;
        const size_t len = util::utf8_at(p, raw.size(), i, cp);
        if (!len) {   // not UTF-8: kept as it is (tools::run makes a result valid)
            s += raw[i++];
            last = Kept::Nothing;
            continue;
        }
        if (breaks(cp) || cp == ' ') {
            if (!s.empty() && s.back() != ' ') s += ' ';
            last = Kept::Nothing;
        } else if (kept_after(cp, last, joiner(cp) && shows_next(raw, i + len))) {
            s.append(raw, i, len);
            last = kind_of(cp);
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
