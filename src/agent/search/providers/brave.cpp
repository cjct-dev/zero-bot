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

#include "net/http.h"

namespace search {

namespace {

// The figures of one of the rate-limit headers, one per window ("50, 1000"): whole numbers written as such, none
// when the header is missing or any figure is not one.
std::vector<long> windows(const std::map<std::string, std::string> &headers, const char *name) {
    const auto it = headers.find(name);
    if (it == headers.end()) return {};
    std::vector<long> out;
    for (const std::string &part : util::split(it->second, ',')) {
        const std::string figure = util::trim(part);
        if (figure.empty() || figure.size() > 9 || figure.find_first_not_of("0123456789") != std::string::npos) return {};
        out.push_back(std::stol(figure));
    }
    return out;
}

// What the month's window says, into `reply`: left, of how much, and in how many seconds it comes back. The month is
// the last window, so a figure is taken only from a header with as many windows as the limit header and that has two
// or more (one alone could be the second's: a 0 there would switch search off for hours); anything else says nothing.
void monthly(const std::map<std::string, std::string> &headers, Reply &reply) {
    const std::vector<long> limit = windows(headers, "x-ratelimit-limit");
    if (limit.size() < 2) return;
    const std::vector<long> left = windows(headers, "x-ratelimit-remaining"), reset = windows(headers, "x-ratelimit-reset");
    if (left.size() == limit.size()) {
        reply.left = left.back();
        reply.limit = limit.back();
    }
    if (reset.size() == limit.size()) reply.reset_s = reset.back();
}

// The code of an error Brave answered with ({"type": "ErrorResponse", "error": {"code": ...}}); "" for any other body.
std::string error_code(const std::string &body) {
    const json j = util::parse_untrusted_json(body);
    if (!j.is_object() || !j.contains("error") || !j["error"].is_object()) return "";
    return util::jget(j["error"], "code", std::string());
}

// What an answer meant: what it means from any service (outcome_of), and Brave's own words on top: the key refused, a
// 422 with SUBSCRIPTION_TOKEN_INVALID; the month spent, a 429 with QUOTA_LIMITED or whose month's window is at 0. The
// code alone means nothing in another answer (a 503 page that happens to hold it).
Outcome meaning(const tools::Fetched &got, const std::string &err) {
    const Outcome outcome = outcome_of(got, err);
    if (outcome != Outcome::Failing) return outcome;
    const std::string code = error_code(got.body);
    if (got.status == 422 && code == "SUBSCRIPTION_TOKEN_INVALID") return Outcome::KeyRefused;
    if (got.status != 429) return outcome;
    Reply figures;
    monthly(got.headers, figures);
    return code == "QUOTA_LIMITED" || figures.left == 0 ? Outcome::Spent : outcome;
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
// that begins no entity known here is kept as it is. Its ';' is looked for only as far as the longest entity reaches
// ("&#x10FFFF;"), not to the end of the text: a text of '&' alone would otherwise take time that grows as its square.
size_t entity(const std::string &text, size_t i, std::string &out) {
    constexpr size_t kLongest = 10;   // from the '&' to the ';'
    const size_t semi = text.substr(i, kLongest + 1).find(';');
    const std::string name = semi == std::string::npos ? "" : text.substr(i + 1, semi - 1);
    if (const char *c = named(name)) {
        out += c;
        return i + semi;
    }
    if (const uint32_t cp = numbered(name)) {
        util::append_utf8(out, cp);
        return i + semi;
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

// The results of a successful answer, as text, up to the first kMaxHits that say something. Garbled when the answer
// is not Brave's shape (an object, its "web" an object, their "results" a list); one without web results (no "web",
// as Brave sends it, or none in it) found nothing.
void read_results(const json &j, Reply &reply) {
    const json none;
    const json &web = j.is_object() && j.contains("web") ? j["web"] : none;
    const json &results = web.is_object() && web.contains("results") ? web["results"] : none;
    if (!j.is_object() || !(web.is_null() || web.is_object()) || !(results.is_null() || results.is_array())) {
        reply.outcome = Outcome::Garbled;
        return;
    }
    size_t saying = 0;
    if (results.is_array())
        for (const json &r : results) {
            if (saying >= kMaxHits) break;
            Hit hit{plain(util::jget(r, "title", std::string())), util::jget(r, "url", std::string()),
                    plain(util::jget(r, "description", std::string()))};
            saying += says_something(hit);
            reply.hits.push_back(std::move(hit));
        }
    if (reply.hits.empty()) reply.outcome = Outcome::Empty;
}

class Brave : public Provider {
public:
    const char *kind() const override { return "brave"; }

    Reply run(const std::string &query, const SearchService &service, tools::Fetch how, tools::Context &ctx) const override {
        how.headers = {"Accept: application/json", "X-Subscription-Token: " + service.key};
        how.keep_headers = {"X-RateLimit-Limit", "X-RateLimit-Remaining", "X-RateLimit-Reset"};
        // A refused key or a spent month is never sent again, even by a service alone (a 429 may be either).
        how.settled = [](const tools::Fetched &got, const std::string &err) {
            const Outcome outcome = meaning(got, err);
            return outcome == Outcome::KeyRefused || outcome == Outcome::Spent;
        };
        Reply reply;
        tools::Fetched got;
        std::string err;
        const json j = send("https://api.search.brave.com/res/v1/web/search?q=" + http::url_encode(query) + "&count=5", how, ctx, got, err);
        reply.outcome = meaning(got, err);
        reply.status = got.status;
        monthly(got.headers, reply);
        if (reply.outcome == Outcome::Ok) read_results(j, reply);
        return reply;
    }
};

}  // namespace

}  // namespace search

std::unique_ptr<search::Provider> make_brave_provider() { return std::make_unique<search::Brave>(); }
