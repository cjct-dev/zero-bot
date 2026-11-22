// The breathe tool: a guided breathing exercise on the screen.
#include "agent/tools/common.h"

#include "core/breath.h"

namespace tools {

namespace {

std::string t_breathe(const json &a, Context &ctx) {
    const std::string wanted = util::lower(str_arg(a, "pattern"));
    const breath::Pattern &p = breath::pattern(wanted);
    double minutes = breath::kDefaultMinutes;
    num_arg(a, "minutes", minutes);
    const int m = breath::clamp_minutes(minutes);
    if (ctx.on_breathe) ctx.on_breathe(p.id, m);
    return json{{"started", p.title}, {"minutes", m},
                {"note", "The exercise appears on screen as soon as you finish speaking, and the user follows the orb. "
                         "Say one short, calm sentence and nothing more."}}.dump();
}

}  // namespace

Tool make_breathe() {
    return {"breathe", "Starting a breathing exercise",
            "Show a guided breathing exercise on the screen: an orb grows and shrinks to pace the breath. Use it when "
            "the user says they are stressed, anxious, overwhelmed, cannot settle or sleep, or asks to meditate, "
            "breathe or calm down. Start it straight away; do not ask which kind or for how long unless they bring it "
            "up, and do not give advice instead.",
            obj({{"pattern", E("box: in, hold, out, hold for four seconds each (the default). calm: slow breathing, "
                               "in for four, out for six. sigh: two breaths in and one long breath out, good for "
                               "winding down", {"box", "calm", "sigh"})},
                 {"minutes", N("Length in minutes, 1 to 20. Leave out for the usual five")}}),
            t_breathe, takes_say()};
}

}  // namespace tools
