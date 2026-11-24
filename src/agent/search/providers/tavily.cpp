// Tavily (tavily.com): POST /search with the key as a bearer token; a basic search (one credit) with the
// service's own summary ("answer") and five results. GET /usage, which costs no credit, says what the key and the
// account's plan have used of their limits this month (the key's limit null when it has none).
#include "agent/search/provider.h"

#include <algorithm>

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

// The summary and results of a successful answer, up to the first kMaxHits results that say something. Garbled when
// the answer is not Tavily's shape (an object, its "answer" text and its "results" a list, where they are given).
void read_answer(const json &j, Reply &reply) {
    const json none;
    const json &answer = j.is_object() && j.contains("answer") ? j["answer"] : none;
    const json &results = j.is_object() && j.contains("results") ? j["results"] : none;
    if (!j.is_object() || !(answer.is_null() || answer.is_string()) || !(results.is_null() || results.is_array())) {
        reply.outcome = Outcome::Garbled;
        return;
    }
    if (answer.is_string()) reply.summary = answer.get<std::string>();
    size_t saying = 0;
    if (results.is_array())
        for (const json &r : results) {
            if (saying >= kMaxHits) break;
            Hit hit{util::jget(r, "title", std::string()), util::jget(r, "url", std::string()), util::jget(r, "content", std::string())};
            saying += says_something(hit);
            reply.hits.push_back(std::move(hit));
        }
    if (reply.summary.empty() && reply.hits.empty()) reply.outcome = Outcome::Empty;
}

class Tavily : public Provider {
public:
    const char *kind() const override { return "tavily"; }

    Reply run(const std::string &query, const SearchService &service, tools::Fetch how, tools::Context &ctx) const override {
        how.headers = {"Content-Type: application/json", "Authorization: Bearer " + service.key};
        how.post_body = json{{"query", query}, {"search_depth", "basic"}, {"include_answer", true}, {"max_results", 5}}.dump();
        Reply reply;
        tools::Fetched got;
        std::string err;
        const json j = send("https://api.tavily.com/search", how, ctx, got, err);
        reply.outcome = outcome_of(got, err);
        reply.status = got.status;
        if (got.status == 432 || got.status == 433) reply.outcome = Outcome::Spent;   // the plan's or the key's limit
        if (reply.outcome == Outcome::Ok) read_answer(j, reply);
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
        std::string err;
        const json j = send("https://api.tavily.com/usage", how, ctx, got, err);
        reply.outcome = outcome_of(got, err);
        reply.status = got.status;
        if (reply.outcome != Outcome::Ok) return reply;
        if (j.is_object()) {
            left_of(j.contains("key") ? j["key"] : json(), "usage", "limit", reply);
            left_of(j.contains("account") ? j["account"] : json(), "plan_usage", "plan_limit", reply);
        }
        if (reply.left < 0) reply.outcome = Outcome::Garbled;
        return reply;
    }
};

}  // namespace

}  // namespace search

std::unique_ptr<search::Provider> make_tavily_provider() { return std::make_unique<search::Tavily>(); }
