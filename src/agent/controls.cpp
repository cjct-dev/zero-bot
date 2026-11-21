// See controls.h. The switch is compiled into the cli build only (ZB_EXPERIMENTS).
#include "agent/controls.h"

#if ZB_EXPERIMENTS

#include <cstdio>
#include <cstdlib>

namespace controls {

namespace {

// The names ZB_EXP_OFF takes, in the order of Control.
const char *const kNames[] = {"second_look", "decide_note", "claim_guards", "say", "weekday_check", "cancel_claims",
                              "date_spelling"};
constexpr int kCount = DateSpelling + 1;
static_assert(sizeof kNames / sizeof *kNames == kCount, "a name for every control");

std::string known() {
    std::string out;
    for (const char *n : kNames) out += std::string(out.empty() ? "" : ", ") + n;
    return out + " or all";
}

// ZB_EXP_OFF as a set of bits, one per Control. Stops the run on a name it does not know.
unsigned read_off() {
    const char *v = getenv("ZB_EXP_OFF");
    if (!v) return 0;
    const std::string list = v;
    unsigned off = 0;
    for (size_t from = 0; from <= list.size();) {
        size_t end = list.find(',', from);
        if (end == std::string::npos) end = list.size();
        size_t a = from, b = end;
        while (a < b && (list[a] == ' ' || list[a] == '\t')) ++a;
        while (b > a && (list[b - 1] == ' ' || list[b - 1] == '\t')) --b;
        const std::string name = list.substr(a, b - a);
        from = end + 1;
        if (name.empty()) continue;
        if (name == "all") { off = (1u << kCount) - 1; continue; }
        int i = 0;
        while (i < kCount && name != kNames[i]) ++i;
        if (i == kCount) {
            fprintf(stderr, "ZB_EXP_OFF: %s is not a control this build knows (%s)\n", name.c_str(), known().c_str());
            exit(2);
        }
        off |= 1u << i;
    }
    return off;
}

unsigned off_set() {
    static const unsigned off = read_off();
    return off;
}

// Read as the program starts, so that a slip in a name stops any run, whatever its mode, before
// anything is sent.
[[maybe_unused]] const bool g_read_at_start = (off_set(), true);

}  // namespace

bool on(Control c) { return !(off_set() & (1u << c)); }

std::string off_names() {
    std::string out;
    for (int i = 0; i < kCount; ++i)
        if (off_set() & (1u << i)) out += std::string(out.empty() ? "" : ",") + kNames[i];
    return out;
}

}  // namespace controls

#endif
