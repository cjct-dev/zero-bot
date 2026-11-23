// Brave Search (brave.com/search/api): GET /res/v1/web/search with the key in X-Subscription-Token; five web results,
// each a title, an address and a description, and no summary. The descriptions are HTML: the words searched for in
// <strong>, some characters as entities (&#x27;). Every reply says what the allowance is, what is left of it and when
// it comes back, per second then per month: X-RateLimit-Limit "50, 1000", X-RateLimit-Remaining "49, 812",
// X-RateLimit-Reset "1, 1419704" (seconds). Its error codes come in the body: an invalid key is 422
// SUBSCRIPTION_TOKEN_INVALID (as a user reported it; a guide says 401, which outcome_of takes too), a spent month 429
// QUOTA_LIMITED, too many at once 429 RATE_LIMITED. Its terms allow results to be stored only transiently: nothing of a
// reply is written anywhere (the cache is in memory, for ten minutes; the allowance file keeps only the figures).
#include "agent/search/provider.h"

#include <algorithm>
#include <cctype>
#include <map>

#include "agent/experiment.h"
#include "net/http.h"

namespace search {

namespace {

// The month's figure of one of the rate-limit headers (its last number); -1 when there is none.
long monthly(const std::map<std::string, std::string> &headers, const char *name) {
    const auto it = headers.find(name);
    if (it == headers.end()) return -1;
    const size_t comma = it->second.rfind(',');
    double v = 0;
    if (!util::to_number(util::trim(it->second.substr(comma == std::string::npos ? 0 : comma + 1)), v) || v < 0) return -1;
    return static_cast<long>(std::min(v, 1e9));
}

// The code of an error Brave answered with ({"type": "ErrorResponse", "error": {"code": ...}}); "" for any other body.
std::string error_code(const std::string &body) {
    const json j = util::parse_untrusted_json(body);
    if (!j.is_object() || !j.contains("error") || !j["error"].is_object()) return "";
    return util::jget(j["error"], "code", std::string());
}

// What a named entity stands for (those Brave writes); null for any other name.
const char *named(const std::string &name) {
    static const std::pair<const char *, const char *> kNamed[] = {{"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""},
                                                                   {"apos", "'"}, {"nbsp", " "}};
    for (const auto &[n, text] : kNamed)
        if (name == n) return text;
    return nullptr;
}

// The character a numbered entity's name ("#39", "#x27") stands for; 0 when it is none.
uint32_t numbered(const std::string &name) {
    const bool hex = name.size() > 2 && name[0] == '#' && (name[1] == 'x' || name[1] == 'X');
    const std::string digits = name.size() > 1 && name[0] == '#' ? name.substr(hex ? 2 : 1) : "";
    if (digits.empty() || digits.size() > 7) return 0;
    for (const char c : digits)
        if (!(hex ? isxdigit(static_cast<unsigned char>(c)) : isdigit(static_cast<unsigned char>(c)))) return 0;
    const unsigned long cp = std::stoul(digits, nullptr, hex ? 16 : 10);
    return cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) ? 0 : static_cast<uint32_t>(cp);
}

// The entity at text[i] ('&') as the character it stands for, appended to `out`; the index of its last byte. A '&'
// that begins no entity known here is kept as it is.
size_t entity(const std::string &text, size_t i, std::string &out) {
    const size_t end = text.find(';', i);
    const std::string name = end == std::string::npos || end - i > 10 ? "" : text.substr(i + 1, end - i - 1);
    if (const char *c = named(name)) {
        out += c;
        return end;
    }
    if (const uint32_t cp = numbered(name)) {
        util::append_utf8(out, cp);
        return end;
    }
    out += '&';
    return i;
}

// Brave's HTML as text: tags left out (its own '<' arrives as &lt;), entities decoded.
std::string plain(const std::string &html) {
    std::string out;
    for (size_t i = 0; i < html.size(); ++i) {
        if (html[i] == '<') {
            const size_t end = html.find('>', i);
            if (end == std::string::npos) break;
            i = end;
        } else if (html[i] == '&') {
            i = entity(html, i, out);
        } else {
            out += html[i];
        }
    }
    return out;
}

class Brave : public Provider {
public:
    const char *kind() const override { return "brave"; }

    Reply run(const std::string &query, const SearchService &service, tools::Fetch how, tools::Context &ctx) const override {
        how.headers = {"Accept: application/json", "X-Subscription-Token: " + service.key};
        how.keep_headers = {"X-RateLimit-Limit", "X-RateLimit-Remaining", "X-RateLimit-Reset"};
        Reply reply;
        tools::Fetched got;
        const json j = tools::fetch_json(
            experiment::web("https://api.search.brave.com/res/v1/web/search?q=" + http::url_encode(query) + "&count=5"), reply.error,
            ctx, how, &got);
        reply.outcome = outcome_of(got, reply.error);
        reply.left = monthly(got.headers, "x-ratelimit-remaining");
        reply.limit = monthly(got.headers, "x-ratelimit-limit");
        reply.reset_s = std::max(0L, monthly(got.headers, "x-ratelimit-reset"));
        const std::string code = reply.outcome == Outcome::Failing ? error_code(got.body) : "";
        if (code == "SUBSCRIPTION_TOKEN_INVALID") reply.outcome = Outcome::KeyRefused;
        else if (code == "QUOTA_LIMITED" || (got.status == 429 && reply.left == 0)) reply.outcome = Outcome::Spent;
        if (reply.outcome != Outcome::Ok) return reply;
        const json web = j.is_object() && j.contains("web") ? j["web"] : json();
        if (web.is_object() && web.contains("results") && web["results"].is_array())
            for (const json &r : web["results"])
                reply.hits.push_back({plain(util::jget(r, "title", std::string())), util::jget(r, "url", std::string()),
                                      plain(util::jget(r, "description", std::string()))});
        if (reply.hits.empty()) reply.outcome = Outcome::Empty;
        return reply;
    }
};

}  // namespace

}  // namespace search

std::unique_ptr<search::Provider> make_brave_provider() { return std::make_unique<search::Brave>(); }
