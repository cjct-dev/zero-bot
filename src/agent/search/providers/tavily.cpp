// Tavily (tavily.com): POST /search with the key as a bearer token; a basic search (one credit) with the
// service's own summary ("answer") and five results. GET /usage, which costs no credit, says what the key and the
// account's plan have used of their limits this month (the key's limit null when it has none).
#include "agent/search/provider.h"

#include <algorithm>

#include "agent/experiment.h"

namespace search {

namespace {

// What is left of one of /usage's limits ({"usage": N, "limit": M}, under the fields named): -1 when it has none.
void left_of(const json &part, const char *used_field, const char *limit_field, Reply &out) {
    if (!part.is_object()) return;
    const long limit = util::jget(part, limit_field, -1L), used = util::jget(part, used_field, -1L);
    if (limit < 0 || used < 0) return;
    const long left = std::max(0L, limit - used);
    if (out.left < 0 || left < out.left) {
        out.left = left;
        out.limit = limit;
    }
}

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

    bool tells_allowance() const override { return true; }

    // The smaller of what the key and the plan have left: a search stops at whichever runs out first.
    Reply allowance(const SearchService &service, tools::Context &ctx) const override {
        tools::Fetch how;
        how.headers = {"Authorization: Bearer " + service.key};
        how.deadline_ms = 8000;
        Reply reply;
        tools::Fetched got;
        const json j = tools::fetch_json(experiment::web("https://api.tavily.com/usage"), reply.error, ctx, how, &got);
        reply.outcome = outcome_of(got, reply.error);
        if (reply.outcome != Outcome::Ok || !j.is_object()) return reply;
        left_of(j.contains("key") ? j["key"] : json(), "usage", "limit", reply);
        left_of(j.contains("account") ? j["account"] : json(), "plan_usage", "plan_limit", reply);
        if (reply.left < 0) reply.outcome = Outcome::Garbled;
        return reply;
    }
};

}  // namespace

}  // namespace search

std::unique_ptr<search::Provider> make_tavily_provider() { return std::make_unique<search::Tavily>(); }
