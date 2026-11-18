#include "ui/screen_idle.h"

namespace screen {

const std::vector<int> &off_choices() {
    static const std::vector<int> choices = {0, 1, 2, 5, 10, 15};
    return choices;
}

int step_off_choice(int minutes, int dir) {
    const std::vector<int> &c = off_choices();
    const int n = static_cast<int>(c.size());
    if (minutes < 0) minutes = 0;   // read as never
    int at = -1;
    for (int i = 0; i < n; ++i)
        if (c[static_cast<size_t>(i)] == minutes) at = i;
    if (at < 0) {
        // Not a choice: the next larger one going up, the next smaller one going down.
        if (dir > 0) {
            for (int v : c)
                if (v > minutes) return v;
            return c.front();
        }
        for (int i = n - 1; i >= 0; --i)
            if (c[static_cast<size_t>(i)] < minutes) return c[static_cast<size_t>(i)];
        return c.back();
    }
    return c[static_cast<size_t>(((at + (dir > 0 ? 1 : -1)) % n + n) % n)];
}

std::string off_label(int minutes) {
    if (minutes <= 0) return "Never";
    return std::to_string(minutes) + (minutes == 1 ? " minute" : " minutes");
}

Light Idle::use(unsigned long now) {
    const Light was = light_;
    light_ = Light::Lit;
    if (now > last_use_) last_use_ = now;   // a use stamped a moment earlier on another path is not a step back
    return was;
}

Light Idle::update(unsigned long now, bool busy, int off_after_min) {
    if (busy) {
        use(now);
        return light_;
    }
    // A use stamped a moment after the time this tick read is no idle time at all, not a very long one.
    const unsigned long idle = now > last_use_ ? now - last_use_ : 0;
    Light want = Light::Lit;
    if (off_after_min > 0 && idle >= static_cast<unsigned long>(off_after_min) * 60000UL) want = Light::Off;
    else if (idle >= kDimAfterMs) want = Light::Dimmed;
    if (static_cast<int>(want) > static_cast<int>(light_)) light_ = want;
    return light_;
}

}  // namespace screen
