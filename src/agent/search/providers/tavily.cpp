// Tavily (tavily.com): POST /search with the key as a bearer token; a basic search (one credit) with the
// service's own summary ("answer") and five results.
#include "agent/search/provider.h"

#include "agent/experiment.h"

namespace search {

namespace {

class Tavily : public Provider {
public:
    const char *kind() const override { return "tavily"; }

    Reply run(const std::string &query, const SearchService &service, tools::Fetch how, tools::Context &ctx) const override {
        how.headers = {"Content-Type: application/json", "Authorization: Bearer " + service.key};
        how.post_body = json{{"query", query}, {"search_depth", "basic"}, {"include_answer", true}, {"max_results", 5}}.dump();
        Reply reply;
        tools::Fetched got;
        const json j = tools::fetch_json(experiment::web("https://api.tavily.com/search"), reply.error, ctx, how, &got);
        reply.outcome = outcome_of(got, reply.error);
        if (got.status == 432 || got.status == 433) reply.outcome = Outcome::Spent;   // the plan's or the key's limit
        if (reply.outcome != Outcome::Ok) return reply;
        reply.summary = util::jget(j, "answer", std::string());
        if (j.contains("results") && j["results"].is_array())
            for (const json &r : j["results"])
                reply.hits.push_back({util::jget(r, "title", std::string()), util::jget(r, "url", std::string()),
                                      util::jget(r, "content", std::string())});
        if (reply.summary.empty() && reply.hits.empty()) reply.outcome = Outcome::Empty;
        return reply;
    }
};

}  // namespace

}  // namespace search

std::unique_ptr<search::Provider> make_tavily_provider() { return std::make_unique<search::Tavily>(); }
