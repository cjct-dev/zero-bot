// The calculator tool: arithmetic, worked out exactly by agent/calc.
#include "agent/tools/common.h"

#include "agent/calc.h"

namespace tools {

namespace {

std::string t_calculator(const json &a, Context &) {
    const std::string expr = str_arg(a, "expression");
    if (expr.empty()) return fail("expression is required");
    double value;
    std::string err;
    if (!calc::evaluate(expr, value, err)) return fail(err + ". For anything beyond arithmetic use run_python.");
    return calc::format(value);
}

}  // namespace

Tool make_calculator() {
    return {"calculator", "Calculating",
            "Evaluate arithmetic exactly. Use it for every sum beyond single-digit arithmetic: tips, splits, "
            "percentages, unit conversions. Not for dates (use date_info). Supports + - * / ^ %, brackets, sqrt, sin, cos, tan, ln, log, exp, abs, round, "
            "min, max, factorial (5!), pi and e. '15% of 80' is written 0.15*80.",
            obj({{"expression", S("The expression, e.g. \"(84.50 + 12) * 1.125 / 4\"")}}, {"expression"}), t_calculator};
}

}  // namespace tools
