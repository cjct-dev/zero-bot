// Timing for guided breathing exercises: which phase a session is in at a given moment and how
// full the breath is (the breathing scene's sun or moon grows with it). No drawing here, so it can be
// tested without a screen.
#pragma once
#include <string>
#include <vector>

namespace breath {

struct Step {
    const char *label;     // what to do: "Breathe in", "Hold", ...
    int ms;                // how long
    float from, to;        // how full the lungs are at the start and end, 0 to 1
};

struct Pattern {
    const char *id;
    const char *title;
    std::vector<Step> steps;
    int cycle_ms() const;
};

// "box" (in 4, hold 4, out 4, hold 4), "calm" (in 4, out 6: six breaths a minute) or "sigh"
// (two breaths in, one long breath out). Anything else gives box.
const Pattern &pattern(const std::string &id);

// Five minutes is the default: analyses of breathing practice find sessions shorter than that
// less effective, and no clear gain from longer ones.
constexpr int kDefaultMinutes = 5;
int clamp_minutes(double minutes);     // 1 to 20, default when not a sensible number

struct Moment {
    int step = 0;              // index into the pattern's steps
    const char *label = "";
    float level = 0;           // how full the lungs are, eased so it never jerks
    int seconds_left = 0;      // in this step, counting down to 1
    long remaining_ms = 0;     // in the whole session
    bool finished = false;
};

// A session runs for whole breaths: it ends at the end of the cycle in which the time runs out.
long session_ms(const Pattern &p, int minutes);
Moment at(const Pattern &p, long elapsed_ms, long total_ms);

}  // namespace breath
