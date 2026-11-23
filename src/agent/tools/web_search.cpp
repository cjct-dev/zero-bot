// The web_search tool: a web search: a short summary and source snippets. The search itself, by whichever
// service the user has set up, is agent/search's.
#include "agent/tools/common.h"

#include "agent/search/search.h"
#include "core/settings.h"

namespace tools {

namespace {

std::string t_web_search(const json &a, Context &ctx) {
    const std::string query = str_arg(a, "query");
    if (query.empty()) return fail("query is required");
    return search::run(query, ctx);
}

}  // namespace

Tool make_web_search() {
    Tool t{"web_search", "Searching the web",
           "Search the web. Always use it for details that change or are easy to misremember: opening hours and "
           "days, ticket prices, timetables, travel rules, news, sports results, who currently holds a post, "
           "anything recent. Not needed for settled general knowledge. Returns a short summary and source snippets.",
           obj({{"query", S("A specific search query including place names and dates where relevant")}}, {"query"}),
           t_web_search};
    // A tool that can only fail is a wasted round trip; the prompt says search is unavailable.
    t.offered = [] { return search::configured(Settings::get()); };
    return t;
}

}  // namespace tools
