// The convert_currency tool: an amount converted at a current exchange rate.
#include "agent/tools/common.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "agent/experiment.h"
#include "agent/tools/web.h"

namespace tools {

namespace {

std::string fmt_money(double v) {
    char buf[48];
    snprintf(buf, sizeof(buf), std::fabs(v) >= 100 ? "%.2f" : "%.4g", v);
    return buf;
}

std::string t_currency(const json &a, Context &ctx) {
    double amount = 1;
    if (given(a, "amount") && !num_arg(a, "amount", amount)) return fail("amount must be a number, such as 25 or 12.50");
    std::string from = str_arg(a, "from"), to = str_arg(a, "to");
    std::transform(from.begin(), from.end(), from.begin(), ::toupper);
    std::transform(to.begin(), to.end(), to.begin(), ::toupper);
    // The code goes into the service's address, so only three letters will do.
    auto code = [](const std::string &c) {
        return c.size() == 3 && std::all_of(c.begin(), c.end(), [](char x) { return x >= 'A' && x <= 'Z'; });
    };
    if (!code(from) || !code(to)) return fail("from and to must be 3-letter currency codes such as GBP, EUR, JPY");
    std::string err;
    const json j = fetch_json(experiment::web("https://open.er-api.com/v6/latest/" + from), err, ctx);
    if (!err.empty()) return fail(err);
    if (jget(j, "result", std::string()) != "success" || !j.contains("rates") || !j["rates"].is_object() ||
        !j["rates"].contains(to) || !j["rates"][to].is_number())
        return fail("no exchange rate available for " + from + " to " + to + "; check both are real currency codes");
    const double rate = j["rates"][to].get<double>();
    return json{{"amount", amount}, {"from", from}, {"to", to}, {"rate", rate},
                {"result", fmt_money(amount * rate) + " " + to},
                {"rate_date", jget(j, "time_last_update_utc", std::string()).substr(0, 16)}}.dump();
}

}  // namespace

Tool make_convert_currency() {
    return {"convert_currency", "Converting currency",
            "Convert an amount between currencies using a current exchange rate.",
            obj({{"amount", N("Amount to convert")}, {"from", S("3-letter code of the source currency, e.g. EUR")},
                 {"to", S("3-letter code of the target currency, e.g. GBP")}}, {"amount", "from", "to"}), t_currency};
}

}  // namespace tools
