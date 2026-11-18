#include "core/breath.h"

#include <algorithm>
#include <cmath>

namespace breath {

int Pattern::cycle_ms() const {
    int total = 0;
    for (const Step &s : steps) total += s.ms;
    return total;
}

const Pattern &pattern(const std::string &id) {
    static const Pattern box = {"box", "Box breathing",
                                {{"Breathe in", 4000, 0.f, 1.f}, {"Hold", 4000, 1.f, 1.f},
                                 {"Breathe out", 4000, 1.f, 0.f}, {"Hold", 4000, 0.f, 0.f}}};
    static const Pattern calm = {"calm", "Slow breathing", {{"Breathe in", 4000, 0.f, 1.f}, {"Breathe out", 6000, 1.f, 0.f}}};
    static const Pattern sigh = {"sigh", "Calming sighs",
                                 {{"Breathe in", 2500, 0.f, 0.75f}, {"Sip in a little more", 1200, 0.75f, 1.f},
                                  {"Long breath out", 6500, 1.f, 0.f}}};
    if (id == "calm" || id == "slow" || id == "coherent") return calm;
    if (id == "sigh" || id == "sighing" || id == "cyclic") return sigh;
    return box;
}

int clamp_minutes(double minutes) {
    if (!(minutes >= 0.5)) return kDefaultMinutes;   // also catches NaN
    if (minutes > 20) return 20;                      // before rounding: lround(1e300) is not a number of minutes
    return std::max(1, std::min(20, static_cast<int>(std::lround(minutes))));
}

long session_ms(const Pattern &p, int minutes) {
    const long cycle = p.cycle_ms();
    const long wanted = static_cast<long>(minutes) * 60000L;
    return (wanted + cycle - 1) / cycle * cycle;
}

Moment at(const Pattern &p, long elapsed_ms, long total_ms) {
    Moment m;
    if (elapsed_ms >= total_ms) {
        m.finished = true;
        m.label = "Well done";
        m.level = p.steps.back().to;
        return m;
    }
    m.remaining_ms = total_ms - elapsed_ms;
    long t = elapsed_ms % p.cycle_ms();
    for (size_t i = 0; i < p.steps.size(); ++i) {
        const Step &s = p.steps[i];
        if (t < s.ms || i + 1 == p.steps.size()) {
            const float x = std::min(1.f, static_cast<float>(t) / static_cast<float>(s.ms));
            const float eased = x * x * (3.f - 2.f * x);        // gentle at both ends, like a real breath
            m.step = static_cast<int>(i);
            m.label = s.label;
            m.level = s.from + (s.to - s.from) * eased;
            m.seconds_left = static_cast<int>((s.ms - t + 999) / 1000);
            return m;
        }
        t -= s.ms;
    }
    return m;
}

}  // namespace breath
