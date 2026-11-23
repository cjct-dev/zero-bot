// Unit tests for web search's layer (src/agent/search/search.cpp) against stand-in services: the registry
// (the services built in, and a test's own in their place), what each outcome a service reports means
// (outcome_of), when search counts as set up (configured), and what the model reads of an answer, the same
// whatever the service. No service built into the app is ever asked: the stand-ins replace them before the
// first search, and every proxy variable points at a closed port on this computer besides.
// usage: search_test STATE_DIR (a folder for the settings file; made if missing)
// Each check prints one line on failure; the exit status is non-zero when any fails.
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "agent/search/provider.h"
#include "agent/search/search.h"
#include "core/settings.h"

namespace {

int g_passed = 0, g_failed = 0;

void check(bool ok, const std::string &what) {
    if (ok) ++g_passed;
    else { ++g_failed; printf("FAIL %s\n", what.c_str()); }
}

using search::Hit;
using search::Outcome;
using search::Reply;

// What a stand-in service was asked.
struct Asked {
    std::string query, key;
    long deadline_ms;
    int attempts;
};

// A search service that answers what the test tells it to, and notes what it was asked.
class Stand : public search::Provider {
public:
    explicit Stand(const char *kind) : kind_(kind) {}
    const char *kind() const override { return kind_; }
    Reply run(const std::string &query, const SearchService &service, tools::Fetch how, tools::Context &) const override {
        asked.push_back({query, service.key, how.deadline_ms, how.attempts});
        return next;
    }
    mutable std::vector<Asked> asked;
    Reply next;

private:
    const char *kind_;
};

Reply answer(const std::string &summary, std::vector<Hit> hits) {
    Reply r;
    r.outcome = hits.empty() && summary.empty() ? Outcome::Empty : Outcome::Ok;
    r.summary = summary;
    r.hits = std::move(hits);
    return r;
}

Reply failure(Outcome outcome, const std::string &error) {
    Reply r;
    r.outcome = outcome;
    r.error = error;
    return r;
}

void set_services(std::vector<SearchService> list) {
    Settings::update([&](Settings &s) { s.search = list; });
}

std::string search_for(const std::string &query) {
    tools::Context ctx;
    return search::run(query, ctx);
}

// ---------------------------------------------------------------- the services built in
void test_builtin_registry() {
    const auto &list = search::registry();
    check(!list.empty(), "at least one service is built in");
    std::set<std::string> kinds;
    for (const search::Registered &r : list) {
        const std::string kind = r.provider->kind();
        check(kinds.insert(kind).second, "each built-in kind is registered once: " + kind);
        check(!r.name.empty(), "a built-in service has a name: " + kind);
        check(search::provider_for(kind) == r.provider.get(), "provider_for finds " + kind);
    }
    check(!list.empty() && std::string(list.front().provider->kind()) == "tavily",
          "Tavily is first: the one key row and the setup guide ask for its key");
    check(search::provider_for("later") == nullptr, "a kind this version does not know has no provider");
}

// ---------------------------------------------------------------- what a reply meant
void test_outcomes() {
    auto of = [](long status, bool sent, bool timed_out, bool garbled, const std::string &err) {
        tools::Fetched got;
        got.status = status;
        got.sent = sent;
        got.timed_out = timed_out;
        got.garbled = garbled;
        return search::outcome_of(got, err);
    };
    check(of(200, true, false, false, "") == Outcome::Ok, "an answer read: Ok");
    check(of(200, true, false, true, "unexpected reply from the service") == Outcome::Garbled, "a success that is not JSON: Garbled");
    check(of(0, true, true, false, "timed out") == Outcome::Slow, "sent and no answer in time: Slow");
    check(of(0, false, true, false, "timed out") == Outcome::Unreachable, "a connection never made in time: Unreachable");
    check(of(0, false, false, false, "no internet connection") == Outcome::Unreachable, "no connection: Unreachable");
    check(of(200, true, false, false, "transfer closed") == Outcome::Unreachable, "a success cut off on the way: Unreachable");
    check(of(401, true, false, false, "HTTP 401") == Outcome::KeyRefused, "401: the key refused");
    check(of(403, true, false, false, "HTTP 403") == Outcome::KeyRefused, "403: the key refused");
    check(of(402, true, false, false, "HTTP 402") == Outcome::Spent, "402: the allowance spent");
    for (long status : {302L, 400L, 404L, 408L, 422L, 429L, 500L, 502L, 503L})
        check(of(status, true, false, false, "HTTP " + std::to_string(status)) == Outcome::Failing,
              "HTTP " + std::to_string(status) + ": Failing");
}

// ---------------------------------------------------------------- set up or not
void test_configured() {
    const auto set_up = [](std::vector<SearchService> list) {
        Settings s;
        s.search = std::move(list);
        return search::configured(s);
    };
    check(!set_up({}), "no service: not set up");
    check(set_up({{"alpha", "k"}}), "a known kind with a key: set up");
    check(!set_up({{"alpha", ""}}), "without a key: not set up");
    check(!set_up({{"alpha", "k", false}}), "switched off: not set up");
    check(!set_up({{"later", "k"}}), "only a kind this version does not know: not set up");
    check(set_up({{"later", "k"}, {"beta", "k"}}), "a known kind after an unknown one: set up");
}

// ---------------------------------------------------------------- what the model reads
void test_shape(Stand &alpha) {
    set_services({});
    std::string out = search_for("anything");
    check(out.rfind("ERROR: web search is not set up", 0) == 0, "no service set up: the model is told so (" + out + ")");

    set_services({{"alpha", "key-a"}});
    std::vector<Hit> hits;
    hits.push_back({"\x1b[2JBell\x07 and escape " + std::string(1000, 'T'), "https://example.com/" + std::string(1000, 'u'),
                    std::string(5000, 'C')});
    for (int i = 0; i < 6; ++i) hits.push_back({"Title " + std::to_string(i), "https://example.com/" + std::to_string(i), "Text."});
    alpha.next = answer("The summary.", hits);
    alpha.asked.clear();
    out = search_for("louvre opening hours");
    check(alpha.asked.size() == 1 && alpha.asked[0].query == "louvre opening hours" && alpha.asked[0].key == "key-a",
          "the service set up is asked, with the query and its key");
    const json j = json::parse(out, nullptr, false);
    check(j.is_object() && j.value("summary", "") == "The summary.", "the summary is passed on (" + out.substr(0, 200) + ")");
    const json results = j.is_object() ? j.value("results", json::array()) : json::array();
    check(results.size() == 4, "four results at most");
    if (!results.empty()) {
        const std::string title = results[0].value("title", ""), source = results[0].value("source", ""), text = results[0].value("text", "");
        // Each cap is in bytes, with "…" (3 more) where the text was cut.
        check(title.size() <= 203 && title.find('\x1b') == std::string::npos && title.find('\x07') == std::string::npos,
              "a title is capped and cleaned of control characters");
        check(source.size() <= 303 && text.size() <= 503, "a source and a text are capped");
    }

    alpha.next = answer("", {});
    out = search_for("nothing at all");
    check(out == "{\"results\":[]}", "nothing found: an empty list, not an error (" + out + ")");

    alpha.next = failure(Outcome::Failing, "the service answered with an error (HTTP 503)");
    out = search_for("louvre opening hours");
    check(out.rfind("ERROR: ", 0) == 0, "a failure is one ERROR sentence (" + out + ")");
}

}  // namespace

int main(int argc, char **argv) {
    const std::string dir = argc > 1 ? argv[1] : "/tmp/zb-search-test";
    mkdir(dir.c_str(), 0700);
    setenv("XDG_CONFIG_HOME", (dir + "/config").c_str(), 1);
    setenv("XDG_DATA_HOME", (dir + "/data").c_str(), 1);
    setenv("XDG_CACHE_HOME", (dir + "/cache").c_str(), 1);
    unsetenv("ZB_SEARCH_KEY");
    // Nothing here reaches a real service: should anything be sent, it goes to a closed port on this computer.
    for (const char *proxy : {"http_proxy", "https_proxy", "HTTPS_PROXY", "ALL_PROXY", "all_proxy"}) setenv(proxy, "http://127.0.0.1:9", 1);
    unsetenv("no_proxy");
    unsetenv("NO_PROXY");
    Settings::load();

    test_builtin_registry();
    test_outcomes();

    search::testing::clear_providers();
    auto *alpha = new Stand("alpha");
    search::register_provider(std::unique_ptr<search::Provider>(alpha), "Alpha");
    search::register_provider(std::make_unique<Stand>("beta"), "Beta");
    check(search::registry().size() == 2 && search::provider_for("tavily") == nullptr,
          "a test's stand-ins replace the services built in");
    test_configured();
    test_shape(*alpha);

    printf("search: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
