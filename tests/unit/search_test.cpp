// Unit tests for web search's layer (src/agent/search/search.cpp) against stand-in services: the registry
// (the services built in, and a test's own in their place), what each outcome a service reports means
// (outcome_of), when search counts as set up (configured), and what the model reads of an answer, the same
// whatever the service. No service built into the app is ever asked: the stand-ins replace them before the
// first search, and every proxy variable points at a closed port on this computer besides.
// usage: search_test STATE_DIR (a folder for the settings file; made if missing)
// Each check prints one line on failure; the exit status is non-zero when any fails.
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
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

unsigned long g_now = 1000000;   // the layer's clock (search::testing::set_clock), in milliseconds
unsigned long test_clock() { return g_now; }

// A search service that answers what the test tells it to, and notes what it was asked.
class Stand : public search::Provider {
public:
    explicit Stand(const char *kind) : kind_(kind) {}
    const char *kind() const override { return kind_; }
    Reply run(const std::string &query, const SearchService &service, tools::Fetch how, tools::Context &) const override {
        asked.push_back({query, service.key, how.deadline_ms, how.attempts});
        g_now += takes_ms;
        if (cancel_with) *cancel_with = true;
        return replies_.empty() ? Reply() : replies_[std::min(used_++, replies_.size() - 1)];
    }
    void will(std::vector<Reply> replies) {   // one a search, in turn; the last again after that
        replies_ = std::move(replies);
        used_ = 0;
    }
    mutable std::vector<Asked> asked;
    unsigned long takes_ms = 0;                // how long each search takes, on the test's clock
    std::atomic<bool> *cancel_with = nullptr;  // set while it searches: the user pressed ESC

private:
    const char *kind_;
    std::vector<Reply> replies_;
    mutable size_t used_ = 0;
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
    alpha.will({answer("The summary.", hits)});
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

    alpha.will({answer("", {})});
    out = search_for("nothing at all");
    check(out == "{\"results\":[]}", "nothing found: an empty list, not an error (" + out + ")");

    alpha.will({failure(Outcome::Failing, "the service answered with an error (HTTP 503)")});
    out = search_for("louvre opening hours");
    check(out.rfind("ERROR: web search is not available right now (every search service failed or has used its allowance), so "
                    "this could not be looked up. Tell the user that; if you answer from memory, say it may be out of date", 0) == 0,
          "a failure is one ERROR sentence, whatever the service said (" + out + ")");
}

// ---------------------------------------------------------------- failover
// Three stand-ins, set up afresh for each case: each answers "From <kind>." unless told otherwise.
struct Trio {
    Stand &a, &b, &c;
    void reset(std::vector<SearchService> list) {
        search::testing::forget_health();
        set_services(std::move(list));
        for (Stand *s : {&a, &b, &c}) {
            s->asked.clear();
            s->will({answer("From " + std::string(s->kind()) + ".", {{"Title", "https://example.com/", "Text."}})});
            s->takes_ms = 0;
        }
    }
};

const std::vector<SearchService> kAB = {{"alpha", "key-a"}, {"beta", "key-b"}};
const std::vector<SearchService> kABC = {{"alpha", "key-a"}, {"beta", "key-b"}, {"gamma", "key-c"}};
const std::string kAllFailed = "ERROR: web search is not available right now (every search service failed or has used its allowance)";

bool from(const std::string &out, const char *kind) { return out.find("From " + std::string(kind) + ".") != std::string::npos; }

void test_order_and_deadlines(Trio &t) {
    t.reset(kAB);
    std::string out = search_for("q");
    check(from(out, "alpha") && t.b.asked.empty(), "the first service answers: the second is not asked");
    check(t.a.asked.size() == 1 && t.a.asked[0].deadline_ms == 5000 && t.a.asked[0].attempts == 1,
          "the first of two gets 5 s and one send");
    t.reset({{"alpha", "key-a"}});
    search_for("q");
    check(t.a.asked.size() == 1 && t.a.asked[0].deadline_ms == 8000 && t.a.asked[0].attempts == 2,
          "a service alone gets the whole 8 s and may resend once");
    t.reset({{"beta", "key-b"}, {"alpha", "key-a"}});
    out = search_for("q");
    check(from(out, "beta") && t.a.asked.empty(), "the user's order, not the registry's");
    t.reset({{"later", "key-l"}, {"alpha", "key-a", false}, {"beta", "key-b"}});
    out = search_for("q");
    check(from(out, "beta") && t.a.asked.empty(), "an unknown kind and a service switched off are passed over");
}

void test_each_failure_moves_on(Trio &t) {
    const std::pair<Outcome, const char *> kFailures[] = {
        {Outcome::Unreachable, "no answer"}, {Outcome::Slow, "too slow"}, {Outcome::Failing, "the service's trouble"},
        {Outcome::Garbled, "a garbled answer"}, {Outcome::KeyRefused, "a refused key"}, {Outcome::Spent, "a spent allowance"}};
    for (const auto &[outcome, what] : kFailures) {
        t.reset(kAB);
        t.a.will({failure(outcome, "it failed")});
        t.a.takes_ms = 5000;
        const std::string out = search_for("q");
        check(from(out, "beta"), std::string(what) + ": the next service answers (" + out + ")");
        check(t.b.asked.size() == 1 && t.b.asked[0].deadline_ms == 3000 && t.b.asked[0].attempts == 2,
              std::string(what) + ": the last service gets what is left of the 8 s, and may resend");
    }
    t.reset(kAB);
    t.a.will({failure(Outcome::Slow, "timed out")});
    t.a.takes_ms = 7500;
    const std::string out = search_for("q");
    check(t.b.asked.empty() && out.rfind(kAllFailed, 0) == 0, "under a second left of the 8: no other service is asked (" + out + ")");
}

void test_rest(Trio &t) {
    t.reset(kAB);
    t.a.will({failure(Outcome::Failing, "HTTP 503")});
    search_for("one");
    search_for("two");
    check(t.a.asked.size() == 2 && t.b.asked.size() == 2, "a failing first service is asked on each of two searches");
    std::string out = search_for("three");
    check(t.a.asked.size() == 2 && from(out, "beta"), "after two failures in a row it rests: the next search goes to the second");
    t.b.will({failure(Outcome::Unreachable, "no internet connection")});
    search_for("four");
    check(t.a.asked.size() == 3 && t.b.asked.size() == 4, "a resting service is still asked when every other has failed");
    t.b.will({answer("From beta.", {})});
    g_now += 2 * 60 * 1000;
    t.a.will({answer("From alpha.", {})});
    out = search_for("five");
    check(from(out, "alpha") && t.b.asked.size() == 4, "after two minutes the order is the user's again: the first answers");
    t.a.will({failure(Outcome::Failing, "HTTP 503"), answer("From alpha.", {})});
    search_for("six");
    search_for("seven");
    check(t.a.asked.size() == 6, "an answer ends the count: one failure since is no rest");
}

void test_refused_key(Trio &t) {
    t.reset(kAB);
    t.a.will({failure(Outcome::KeyRefused, "HTTP 401")});
    std::string out = search_for("one");
    check(from(out, "beta"), "a refused key: the next service answers");
    search_for("two");
    check(t.a.asked.size() == 1, "a refused key is not sent again in this run");
    Settings::load();   // what a later run reads from the file
    search_for("three");
    check(t.a.asked.size() == 1 && Settings::get().search[0].refused.size() == 16 &&
              Settings::get().search[0].refused.find("key-a") == std::string::npos,
          "nor by a later run: a print of the key (not the key) is in the settings file");
    t.a.will({answer("From alpha.", {})});
    Settings::update([](Settings &s) { s.set_search_key("alpha", "key-a"); });
    Settings::load();
    check(Settings::get().search[0].refused.empty(), "typing the key again clears the mark in the file too");
    out = search_for("four");
    check(t.a.asked.size() == 2 && from(out, "alpha"), "typed again, even unchanged, the key is sent again");
    t.a.will({failure(Outcome::KeyRefused, "HTTP 401"), answer("From alpha.", {})});
    search_for("five");
    Settings::update([](Settings &s) { s.search[0].key = "key-a2"; });   // another key put in its place by hand
    out = search_for("six");
    check(t.a.asked.size() == 4 && t.a.asked[3].key == "key-a2" && from(out, "alpha"), "another key in its place is not refused");

    t.reset({{"alpha", "key-a"}});
    t.a.will({failure(Outcome::KeyRefused, "HTTP 401")});
    out = search_for("one");
    check(out.rfind("ERROR: the search service refused the key set up on this device (a new key can be typed in settings), so "
                    "this could not be looked up. Tell the user that", 0) == 0,
          "its key refused, the only service: the model is told to tell the user (" + out + ")");
    out = search_for("two");
    check(t.a.asked.size() == 1 && out.rfind("ERROR: the search service refused the key", 0) == 0, "and so again, without asking");
    t.reset(kAB);
    t.a.will({failure(Outcome::KeyRefused, "HTTP 401")});
    t.b.will({failure(Outcome::KeyRefused, "HTTP 422")});
    out = search_for("one");
    check(out.rfind("ERROR: the search services refused the keys set up on this device", 0) == 0, "both refused (" + out + ")");
    t.reset(kAB);
    t.a.will({failure(Outcome::KeyRefused, "HTTP 401")});
    t.b.will({failure(Outcome::Failing, "HTTP 500")});
    out = search_for("one");
    check(out.rfind(kAllFailed, 0) == 0, "a key refused and another service failing: the sentence for every failure (" + out + ")");
}

void test_spent(Trio &t) {
    t.reset(kAB);
    t.a.will({failure(Outcome::Spent, "HTTP 432")});
    std::string out = search_for("one");
    check(from(out, "beta"), "a spent allowance: the next service answers");
    g_now += 5 * 3600 * 1000UL;
    search_for("two");
    check(t.a.asked.size() == 1, "a spent service that did not say when it comes back is passed over for hours");
    g_now += 3600 * 1000UL + 1;
    search_for("three");
    check(t.a.asked.size() == 2, "and is asked again after six");

    t.reset(kAB);
    Reply spent = failure(Outcome::Spent, "HTTP 429");
    spent.reset_s = 60;
    t.a.will({spent, answer("From alpha.", {})});
    search_for("one");
    search_for("two");
    g_now += 61 * 1000;
    out = search_for("three");
    check(t.a.asked.size() == 2 && from(out, "alpha"), "one that said when its allowance comes back is asked again then");

    t.reset(kAB);
    Reply last = answer("From alpha.", {});
    last.none_left = true;
    last.reset_s = 3600;
    t.a.will({last});
    out = search_for("one");
    check(from(out, "alpha"), "an answer that says nothing is left is used");
    out = search_for("two");
    check(t.a.asked.size() == 1 && from(out, "beta"), "and the service is passed over until its allowance comes back");

    t.reset({{"alpha", "key-a"}});
    t.a.will({failure(Outcome::Spent, "HTTP 432")});
    out = search_for("one");
    check(out.rfind(kAllFailed, 0) == 0, "every allowance spent: one sentence for the model (" + out + ")");
}

void test_nothing_found(Trio &t) {
    t.reset(kABC);
    t.a.will({answer("", {})});
    std::string out = search_for("q");
    check(from(out, "beta") && t.c.asked.empty(), "nothing found: the next service is asked, once");
    t.reset(kABC);
    t.a.will({answer("", {})});
    t.b.will({answer("", {})});
    out = search_for("q");
    check(out == "{\"results\":[]}" && t.c.asked.empty(), "nothing found twice: no third service, and nothing found it is");
    t.reset(kABC);
    t.a.will({answer("", {})});
    t.b.will({failure(Outcome::Failing, "HTTP 500")});
    out = search_for("q");
    check(out == "{\"results\":[]}" && t.c.asked.empty(), "nothing found, then a failure: what was found stands");
}

void test_same_shape(Trio &t) {
    const Reply same = answer("A summary.", {{"T", "https://example.com/x", "Words."}, {"U", "javascript:alert(1)", "More."}});
    t.reset({{"alpha", "key-a"}});
    t.a.will({same});
    const std::string from_a = search_for("q");
    t.reset({{"beta", "key-b"}});
    t.b.will({same});
    const std::string from_b = search_for("q");
    check(from_a == from_b && from_a.find("alpha") == std::string::npos, "the model reads the same whichever service answered, and no name");
    check(from_a.find("javascript:") == std::string::npos && from_a.find("https://example.com/x") != std::string::npos,
          "only a web page's address is passed on as a source (" + from_a + ")");
    t.reset({{"alpha", "key-a"}});
    t.a.will({answer(std::string(5000, 'S'), {})});
    check(search_for("q").size() < 1100, "a summary is capped");
}

void test_cancelled(Trio &t) {
    t.reset(kAB);
    std::atomic<bool> cancel{false};
    t.a.cancel_with = &cancel;
    t.a.will({failure(Outcome::Unreachable, "cancelled")});
    tools::Context ctx;
    ctx.cancel = &cancel;
    search::run("q", ctx);
    search::run("q", ctx);
    t.a.cancel_with = nullptr;
    check(t.b.asked.empty() && t.a.asked.size() == 1, "cancelled: no other service is asked, and nothing once cancelled");
    t.a.will({failure(Outcome::Failing, "HTTP 503")});
    search_for("one");
    search_for("two");
    check(t.a.asked.size() == 3, "a cancelled search is no failure of the service: one failure since is no rest");
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
    search::testing::set_clock(test_clock);
    auto *alpha = new Stand("alpha"), *beta = new Stand("beta"), *gamma = new Stand("gamma");
    search::register_provider(std::unique_ptr<search::Provider>(alpha), "Alpha");
    search::register_provider(std::unique_ptr<search::Provider>(beta), "Beta");
    search::register_provider(std::unique_ptr<search::Provider>(gamma), "Gamma");
    check(search::registry().size() == 3 && search::provider_for("tavily") == nullptr,
          "a test's stand-ins replace the services built in");
    test_configured();
    test_shape(*alpha);
    Trio t{*alpha, *beta, *gamma};
    test_order_and_deadlines(t);
    test_each_failure_moves_on(t);
    test_rest(t);
    test_refused_key(t);
    test_spent(t);
    test_nothing_found(t);
    test_same_shape(t);
    test_cancelled(t);

    printf("search: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
