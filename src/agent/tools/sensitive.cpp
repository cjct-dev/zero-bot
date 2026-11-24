// Card and ID numbers in text: see sensitive.h.
#include "agent/tools/sensitive.h"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <vector>

#include "agent/tools/common.h"

namespace tools {

namespace {

// A run of letters and digits (upper case), and what stands between it and the run before.
struct Group {
    std::string text;
    char sep = 0;      // ' ' or '-' when that one character alone joins it to the group before; 0 otherwise
    char before = 0;   // the character just before it ('+' in front of a phone number), 0 at the start
};

// How a character between groups reads: a space (Unicode's other spaces too), a dash (and its look-alikes),
// or itself ('?' beyond ASCII).
char gap_char(uint32_t cp) {
    if (cp == ' ' || cp == 0xA0 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F || cp == 0x3000) return ' ';
    if (cp == '-' || (cp >= 0x2010 && cp <= 0x2015) || cp == 0x2212 || cp == 0xFE63 || cp == 0xFF0D) return '-';
    return cp < 0x80 ? static_cast<char>(cp) : '?';
}

std::vector<Group> groups_of(const std::string &raw) {
    const std::string text = clean_text(raw);   // a hidden character inside a number does not split it
    const unsigned char *p = reinterpret_cast<const unsigned char *>(text.data());
    std::vector<Group> out;
    std::string gap;   // what stands since the last group, as gap_char reads it
    bool in_group = false;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = 0xFFFD;
        const size_t len = util::utf8_at(p, text.size(), i, cp);
        i += len ? len : 1;
        if (len && cp < 0x80 && isalnum(static_cast<int>(cp))) {
            if (!in_group) {
                Group g;
                g.sep = !out.empty() && (gap == " " || gap == "-") ? gap[0] : 0;
                g.before = gap.empty() ? 0 : gap.back();
                out.push_back(g);
                gap.clear();
                in_group = true;
            }
            out.back().text += static_cast<char>(toupper(static_cast<int>(cp)));
        } else {
            in_group = false;
            gap += gap_char(len ? cp : 0xFFFD);
        }
    }
    return out;
}

bool digits(const std::string &s) {
    for (const char c : s)
        if (!isdigit(static_cast<unsigned char>(c))) return false;
    return !s.empty();
}

// Whether groups first..last are one number written in parts: each joined to the one before by one
// separator, the same throughout.
bool joined(const std::vector<Group> &g, size_t first, size_t last) {
    for (size_t k = first + 1; k <= last; ++k)
        if (!g[k].sep || g[k].sep != g[first + 1].sep) return false;
    return true;
}

// ---------------------------------------------------------------- payment cards
bool luhn(const std::string &number) {
    int sum = 0;
    bool twice = false;
    for (auto it = number.rbegin(); it != number.rend(); ++it) {
        int d = *it - '0';
        if (twice && (d *= 2) > 9) d -= 9;
        sum += d;
        twice = !twice;
    }
    return sum % 10 == 0;
}

// The parts a card number is written in: run together, in fours (the last part shorter), or 4-6-5 and 4-6-4
// (American Express, Diners Club). An order number's 3-7-7 is none of them.
bool card_layout(const std::vector<size_t> &parts) {
    if (parts.size() == 1) return true;
    if (parts == std::vector<size_t>{4, 6, 5} || parts == std::vector<size_t>{4, 6, 4}) return true;
    for (size_t k = 0; k + 1 < parts.size(); ++k)
        if (parts[k] != 4) return false;
    return parts.back() <= 4;
}

// A card number begins at group i; the number may be followed by more digits (the security code).
bool card_at(const std::vector<Group> &g, size_t i) {
    const std::string &first = g[i].text;
    if (!digits(first) || first[0] < '2' || first[0] > '6' || g[i].before == '+') return false;
    std::string number;
    std::vector<size_t> parts;
    for (size_t j = i; j < g.size() && digits(g[j].text) && joined(g, i, j); ++j) {
        number += g[j].text;
        parts.push_back(g[j].text.size());
        if (number.size() > 19) break;
        if (number.size() >= 13 && card_layout(parts) && luhn(number)) return true;
    }
    return false;
}

// ---------------------------------------------------------------- IBANs
// The countries whose banks issue IBANs: the registry's, and those that use the format outside it.
bool iban_country(const std::string &code) {
    static const char kCodes[] =
        " AD AE AL AO AT AZ BA BE BF BG BH BI BJ BR BY CF CG CH CI CM CR CV CY CZ DE DJ DK DO DZ EE EG ES FI FK FO FR GA GB GE GI GL GQ"
        " GR GT GW HN HR HU IE IL IQ IR IS IT JO KM KW KZ LB LC LI LT LU LV LY MA MC MD ME MG MK ML MN MR MT MU MZ NE NI NL NO OM PK PL"
        " PS PT QA RO RS RU SA SC SD SE SI SK SM SN SO ST SV TD TG TL TN TR UA VA VG XK YE ";
    return code.size() == 2 && strstr(kCodes, (" " + code + " ").c_str()) != nullptr;
}

// The ISO 13616 check: the first four characters moved to the end, letters as 10 to 35, the whole mod 97 is 1.
bool mod97(const std::string &iban) {
    const std::string moved = iban.substr(4) + iban.substr(0, 4);
    unsigned r = 0;
    for (const char c : moved)
        r = isdigit(static_cast<unsigned char>(c)) ? (r * 10 + static_cast<unsigned>(c - '0')) % 97
                                                   : (r * 100 + static_cast<unsigned>(c - 'A' + 10)) % 97;
    return r == 1;
}

// An IBAN begins at group i: run together, or in fours with a shorter last part (a word may follow it).
bool iban_at(const std::vector<Group> &g, size_t i) {
    const std::string &t = g[i].text;
    if (t.size() < 4 || !isalpha(static_cast<unsigned char>(t[0])) || !isalpha(static_cast<unsigned char>(t[1])) ||
        !isdigit(static_cast<unsigned char>(t[2])) || !isdigit(static_cast<unsigned char>(t[3])) || !iban_country(t.substr(0, 2)))
        return false;
    std::string iban;
    for (size_t j = i; j < g.size() && (j == i || (joined(g, i, j) && g[j - 1].text.size() == 4)); ++j) {
        iban += g[j].text;
        if (iban.size() > 34) break;
        if (iban.size() >= 15 && mod97(iban)) return true;
    }
    return false;
}

// ---------------------------------------------------------------- UK National Insurance and US Social Security numbers
// Two prefix letters (never D, F, I, Q, U or V first, nor those or O second, nor BG, GB, KN, NK, NT, TN or ZZ),
// six digits and A, B, C or D.
bool nino(const std::string &s) {
    if (s.size() != 9 || !strchr("ABCEGHJKLMNOPRSTWXYZ", s[0]) || !strchr("ABCEGHJKLMNPRSTWXYZ", s[1])) return false;
    for (const char *never : {"BG", "GB", "KN", "NK", "NT", "TN", "ZZ"})
        if (s.compare(0, 2, never) == 0) return false;
    return digits(s.substr(2, 6)) && s[8] >= 'A' && s[8] <= 'D';
}

bool nino_at(const std::vector<Group> &g, size_t i) {
    std::string s;
    for (size_t j = i; j < g.size() && joined(g, i, j); ++j) {
        s += g[j].text;
        if (s.size() >= 9) return nino(s);
    }
    return false;
}

// ddd-dd-dddd on its own (not part of a longer number joined by dashes), with an area, group and serial the
// Social Security Administration issues.
bool ssn_at(const std::vector<Group> &g, size_t i) {
    if (i + 2 >= g.size()) return false;
    const std::string &area = g[i].text, &group = g[i + 1].text, &serial = g[i + 2].text;
    if (area.size() != 3 || group.size() != 2 || serial.size() != 4 || !digits(area + group + serial)) return false;
    if (g[i + 1].sep != '-' || g[i + 2].sep != '-') return false;
    if (g[i].sep == '-' && digits(g[i - 1].text)) return false;
    if (i + 3 < g.size() && g[i + 3].sep == '-' && digits(g[i + 3].text)) return false;
    return area != "000" && area != "666" && area[0] != '9' && group != "00" && serial != "0000";
}

}  // namespace

std::string sensitive_number(const std::string &text) {
    const std::vector<Group> g = groups_of(text);
    for (size_t i = 0; i < g.size(); ++i) {
        if (card_at(g, i)) return "a payment card number";
        if (iban_at(g, i)) return "an IBAN (a bank account number)";
        if (nino_at(g, i)) return "a UK National Insurance number";
        if (ssn_at(g, i)) return "a US Social Security number";
    }
    return "";
}

}  // namespace tools
