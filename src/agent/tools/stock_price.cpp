// The stock_price tool: the latest price of a share, index, fund or cryptocurrency.
#include "agent/tools/common.h"

#include <algorithm>
#include <cmath>

#include "agent/experiment.h"
#include "agent/tools/web.h"
#include "net/http.h"

namespace tools {

namespace {

std::string t_stock(const json &a, Context &ctx) {
    std::string symbol = util::truncate_utf8(str_arg(a, "symbol"), 80, "");
    if (symbol.empty()) return fail("symbol (a ticker or company name) is required");
    const std::vector<std::string> hdr = {"Accept: application/json"};
    std::string err, name;
    // Anything that does not look like a ticker is looked up by name first.
    const bool tickerish = symbol.size() <= 8 && symbol.find(' ') == std::string::npos &&
                           std::all_of(symbol.begin(), symbol.end(), [](unsigned char c) { return isupper(c) || isdigit(c) || c == '.' || c == '-' || c == '^' || c == '='; });
    if (!tickerish) {
        const json s = fetch_json(experiment::web("https://query1.finance.yahoo.com/v1/finance/search?quotesCount=3&newsCount=0&q=" +
                                                  http::url_encode(symbol)), err, ctx, hdr);
        if (!err.empty()) return fail(err);
        if (!s.contains("quotes") || !s["quotes"].is_array() || s["quotes"].empty())
            return fail("no listed company or fund found for '" + symbol + "'");
        name = jget(s["quotes"][0], "longname", jget(s["quotes"][0], "shortname", std::string()));
        symbol = jget(s["quotes"][0], "symbol", std::string());
        if (symbol.empty()) return fail("no ticker found for that name");
    }
    const json j = fetch_json(experiment::web("https://query1.finance.yahoo.com/v8/finance/chart/" + http::url_encode(symbol) +
                                              "?range=5d&interval=1d"), err, ctx, hdr);
    if (!err.empty()) return fail(err);
    if (!j.contains("chart") || !j["chart"].is_object() || !j["chart"].contains("result") || !j["chart"]["result"].is_array() ||
        j["chart"]["result"].empty() || !j["chart"]["result"][0].is_object() || !j["chart"]["result"][0].contains("meta") ||
        !j["chart"]["result"][0]["meta"].is_object())
        return fail("no price data for '" + symbol + "'; check the ticker");
    const json &m = j["chart"]["result"][0]["meta"];
    const double price = jget(m, "regularMarketPrice", 0.0);
    const double prev = jget(m, "chartPreviousClose", jget(m, "previousClose", 0.0));
    json out = {{"symbol", util::truncate_utf8(clean_text(jget(m, "symbol", symbol)), 20)},
                {"name", util::truncate_utf8(clean_text(jget(m, "longName", jget(m, "shortName", name))), 100)},
                {"price", price}, {"currency", jget(m, "currency", std::string())},
                {"exchange", util::truncate_utf8(clean_text(jget(m, "fullExchangeName", jget(m, "exchangeName", std::string()))), 60)},
                {"day_high", jget(m, "regularMarketDayHigh", 0.0)}, {"day_low", jget(m, "regularMarketDayLow", 0.0)},
                {"year_high", jget(m, "fiftyTwoWeekHigh", 0.0)}, {"year_low", jget(m, "fiftyTwoWeekLow", 0.0)}};
    const long long when = jget<long long>(m, "regularMarketTime", 0);
    if (when) out["as_of"] = util::format_time(static_cast<time_t>(when), "%Y-%m-%d %H:%M");
    if (m.contains("regularMarketChangePercent") && m["regularMarketChangePercent"].is_number())
        out["change_percent_today"] = std::round(m["regularMarketChangePercent"].get<double>() * 100) / 100;
    else if (prev > 0)
        out["change_percent_5_days"] = std::round((price - prev) / prev * 10000) / 100;
    if (jget(m, "currency", std::string()) == "GBp") out["note"] = "price is in pence";
    return out.dump();
}

}  // namespace

Tool make_stock_price() {
    return {"stock_price", "Checking the price",
            "Latest price for a share, index, fund or cryptocurrency, with the day's change and range.",
            obj({{"symbol", S("A ticker (AAPL, RR.L, ^FTSE, BTC-USD) or a company name (\"Rolls-Royce\")")}}, {"symbol"}),
            t_stock};
}

}  // namespace tools
