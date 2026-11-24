// Card and ID numbers in text: see sensitive.h.
#include "agent/tools/sensitive.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <vector>

#include "agent/tools/common.h"

namespace tools {

namespace {

// A run of letters and digits (ASCII, upper case), and what stands between it and the run before.
struct Group {
    std::string text;
    std::string sep;   // " ", "-", ".", "/" or " - " when that alone joins it to the group before; "" otherwise
    char before = 0;   // the character just before it ('+' in front of a phone number), 0 at the start
};

// The digit zero of every run of ten decimal digits in Unicode (category Nd) beyond ASCII: Arabic-Indic,
// Devanagari, Thai, full-width, mathematical and the rest.
const uint32_t kZeros[] = {
    0x660,   0x6F0,   0x7C0,   0x966,   0x9E6,   0xA66,   0xAE6,   0xB66,   0xBE6,   0xC66,   0xCE6,   0xD66,   0xDE6,
    0xE50,   0xED0,   0xF20,   0x1040,  0x1090,  0x17E0,  0x1810,  0x1946,  0x19D0,  0x1A80,  0x1A90,  0x1B50,  0x1BB0,
    0x1C40,  0x1C50,  0xA620,  0xA8D0,  0xA900,  0xA9D0,  0xA9F0,  0xAA50,  0xABF0,  0xFF10,  0x104A0, 0x10D30, 0x11066,
    0x110F0, 0x11136, 0x111D0, 0x112F0, 0x11450, 0x114D0, 0x11650, 0x116C0, 0x11730, 0x118E0, 0x11950, 0x11C50, 0x11D50,
    0x11DA0, 0x11F50, 0x16A60, 0x16AC0, 0x16B50, 0x1D7CE, 0x1D7D8, 0x1D7E2, 0x1D7EC, 0x1D7F6, 0x1E140, 0x1E2F0, 0x1E4F0,
    0x1E950, 0x1FBF0};

// A letter or digit as ASCII, upper case: a decimal digit of any script, a full-width letter; 0 for anything else.
char ascii_of(uint32_t cp) {
    if (cp < 0x80) return isalnum(static_cast<int>(cp)) ? static_cast<char>(toupper(static_cast<int>(cp))) : 0;
    for (const uint32_t zero : kZeros)
        if (cp >= zero && cp < zero + 10) return static_cast<char>('0' + (cp - zero));
    if (cp >= 0xFF21 && cp <= 0xFF3A) return static_cast<char>('A' + (cp - 0xFF21));
    if (cp >= 0xFF41 && cp <= 0xFF5A) return static_cast<char>('A' + (cp - 0xFF41));
    return 0;
}

// How a character between groups reads: a space (Unicode's other spaces too), a dash (and its look-alikes), a
// full stop or a slash (full-width ones too), "" for what joins or styles a character and shows as nothing
// (clean_text keeps the joiners and the text and emoji styles), or itself ('?' beyond ASCII).
std::string gap_of(uint32_t cp) {
    if (cp == ' ' || cp == 0xA0 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F || cp == 0x3000) return " ";
    if (cp == '-' || (cp >= 0x2010 && cp <= 0x2015) || cp == 0x2212 || cp == 0xFE63 || cp == 0xFF0D) return "-";
    if (cp == 0xFF0E) return ".";
    if (cp == 0xFF0B) return "+";
    if (cp == 0xFF0F) return "/";
    if (cp == 0x200C || cp == 0x200D || cp == 0xFE0E || cp == 0xFE0F) return "";
    return std::string(1, cp < 0x80 ? static_cast<char>(cp) : '?');
}

std::vector<Group> groups_of(const std::string &raw) {
    const std::string text = clean_text(raw);   // a hidden character inside a number does not split it
    const unsigned char *p = reinterpret_cast<const unsigned char *>(text.data());
    std::vector<Group> out;
    std::string gap;   // what stands since the last group, as gap_of reads it
    bool in_group = false;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = 0xFFFD;
        const size_t len = util::utf8_at(p, text.size(), i, cp);
        i += len ? len : 1;
        const char c = len ? ascii_of(cp) : 0;
        if (c) {
            if (!in_group) {
                Group g;
                if (!out.empty() && (gap == " " || gap == "-" || gap == "." || gap == "/" || gap == " - ")) g.sep = gap;
                g.before = gap.empty() ? 0 : gap.back();
                out.push_back(g);
                gap.clear();
                in_group = true;
            }
            out.back().text += c;
        } else {
            const std::string between = gap_of(len ? cp : 0xFFFD);
            if (!between.empty()) in_group = false;
            gap += between;
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
        if (g[k].sep.empty() || g[k].sep != g[first + 1].sep) return false;
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

// The card networks: the prefixes each issues (lo to hi, as many digits as lo has) and the lengths, as bits.
struct Network {
    int lo, hi;
    unsigned lengths;
};
constexpr unsigned lengths(int from, int to) { return from > to ? 0 : (1u << from) | lengths(from + 1, to); }
const Network kNetworks[] = {
    {4, 4, lengths(13, 13) | lengths(16, 16) | lengths(19, 19)},                             // Visa
    {51, 55, lengths(16, 16)},      {2221, 2720, lengths(16, 16)},                            // Mastercard
    {34, 34, lengths(15, 15)},      {37, 37, lengths(15, 15)},                                // American Express
    {6011, 6011, lengths(16, 19)},  {644, 649, lengths(16, 19)},  {65, 65, lengths(16, 19)},  // Discover
    {3528, 3589, lengths(16, 19)},                                                            // JCB
    {300, 305, lengths(14, 19)},    {36, 36, lengths(14, 19)},    {38, 39, lengths(14, 19)},  // Diners Club
    {62, 62, lengths(16, 19)},                                                                // UnionPay
    {2200, 2204, lengths(16, 19)},                                                            // Mir
    {50, 50, lengths(12, 19)},      {56, 58, lengths(12, 19)},    {6759, 6759, lengths(12, 19)},   // Maestro
    {60, 60, lengths(16, 16)},      {65, 65, lengths(16, 16)},    {81, 82, lengths(16, 16)},
    {508, 508, lengths(16, 16)},                                                              // RuPay
    {5061, 5061, lengths(16, 19)},  {650002, 650027, lengths(16, 19)},                        // Verve
};

// Whether a number is one a card network issues: its prefix, and a length that network uses.
bool issued(const std::string &n) {
    return std::any_of(std::begin(kNetworks), std::end(kNetworks), [&](const Network &net) {
        const size_t digits = std::to_string(net.lo).size();
        const int prefix = std::stoi(n.substr(0, digits));
        return prefix >= net.lo && prefix <= net.hi && n.size() < 32 && (net.lengths >> n.size() & 1u);
    });
}

// A card number begins at group i; the number may be followed by more digits (the security code).
bool card_at(const std::vector<Group> &g, size_t i) {
    if (!digits(g[i].text) || g[i].before == '+') return false;
    std::string number;
    std::vector<size_t> parts;
    for (size_t j = i; j < g.size() && digits(g[j].text) && joined(g, i, j); ++j) {
        number += g[j].text;
        parts.push_back(g[j].text.size());
        if (number.size() > 19) break;
        if (number.size() >= 12 && card_layout(parts) && issued(number) && luhn(number)) return true;
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

// An IBAN begins at group i: run together, or in fours with a shorter last part (a word may follow it); the
// country's code and the check digits may stand apart ("GB 82 WEST 1234 ...").
bool iban_at(const std::vector<Group> &g, size_t i) {
    const std::string &t = g[i].text;
    const bool apart = t.size() == 2 && i + 1 < g.size() && g[i + 1].text.size() == 2 && digits(g[i + 1].text) &&
                       joined(g, i, i + 1);
    const std::string start = apart ? t + g[i + 1].text : t;
    if (start.size() < 4 || !isalpha(static_cast<unsigned char>(start[0])) || !isalpha(static_cast<unsigned char>(start[1])) ||
        !digits(start.substr(2, 2)) || !iban_country(start.substr(0, 2)))
        return false;
    std::string iban = start;
    const size_t first = apart ? i + 1 : i;   // the group the check digits end
    for (size_t j = first + 1; j < g.size() && joined(g, i, j) && (j == first + 1 ? start.size() == 4 : g[j - 1].text.size() == 4); ++j) {
        if (iban.size() >= 15 && mod97(iban)) return true;
        iban += g[j].text;
        if (iban.size() > 34) break;
    }
    return iban.size() >= 15 && iban.size() <= 34 && mod97(iban);
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
    if (g[i + 1].sep != "-" || g[i + 2].sep != "-") return false;
    if (g[i].sep == "-" && digits(g[i - 1].text)) return false;
    if (i + 3 < g.size() && g[i + 3].sep == "-" && digits(g[i + 3].text)) return false;
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
