// The web_search tool: a web search: a short summary and source snippets.
#include "agent/tools/common.h"

#include "agent/experiment.h"
#include "agent/tools/web.h"
#include "core/settings.h"
#include "smarthome/home.h"

namespace tools {

namespace {

std::string t_web_search(const json &a, Context &ctx) {
    const std::string query = str_arg(a, "query");
    if (query.empty()) return fail("query is required");
    const std::string key = Settings::get().search_key;
    if (key.empty())
        return fail("web search is not set up on this device (a search key can be added in settings), so this could "
                    "not be looked up. Tell the user that; if you answer from memory, say it may be out of date");
    const json body = {{"query", query}, {"search_depth", "basic"}, {"include_answer", true}, {"max_results", 5}};
    std::string err;
    const json j = fetch_json(experiment::web("https://api.tavily.com/search"), err, ctx,
                              {"Content-Type: application/json", "Authorization: Bearer " + key}, body.dump());
    if (!err.empty()) return fail(err);
    json out = json::object();
    const std::string answer = jget(j, "answer", std::string());
    if (!answer.empty()) out["summary"] = answer;
    json results = json::array();
    if (j.contains("results") && j["results"].is_array()) {
        for (const json &r : j["results"]) {
            results.push_back({{"title", util::truncate_utf8(clean_text(jget(r, "title", std::string())), 200)},
                               {"source", util::truncate_utf8(jget(r, "url", std::string()), 300)},
                               {"text", util::truncate_utf8(jget(r, "content", std::string()), 500)}});
            if (results.size() >= 4) break;
        }
    }
    out["results"] = results;
    home::saw_untrusted_text();   // nor may a web page
    return out.dump(-1, ' ', false, json::error_handler_t::replace);
}

}  // namespace

Tool make_web_search() {
    return {"web_search", "Searching the web",
            "Search the web. Always use it for details that change or are easy to misremember: opening hours and "
            "days, ticket prices, timetables, travel rules, news, sports results, who currently holds a post, "
            "anything recent. Not needed for settled general knowledge. Returns a short summary and source snippets.",
            obj({{"query", S("A specific search query including place names and dates where relevant")}}, {"query"}),
            t_web_search};
}

}  // namespace tools
