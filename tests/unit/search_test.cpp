// Unit tests for web search's layer (src/agent/search/search.cpp, usage.cpp) against stand-in services: the
// registry (the services built in, and a test's own in their place), what each outcome a service reports means
// (outcome_of), when search counts as set up (configured), what the model reads of an answer, the same whatever
// the service, the failover, the allowance file (counts, figures, periods, an unset clock, spent allowances after
// a restart, the daily cap), the cache, and what the settings page says of each service. No service built into the
// app is ever asked: the stand-ins replace them before the first search, and every proxy variable points at a
// closed port on this computer besides.
// usage: search_test STATE_DIR (a folder for the settings file; made if missing)
// Each check prints one line on failure; the exit status is non-zero when any fails.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "agent/search/provider.h"
#include "agent/search/search.h"
#include "agent/search/usage.h"
#include "core/settings.h"
#include "core/util.h"

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
unsigned long g_tick = 0;        // how far it moves at each reading (a thread that runs on while it reads twice)
unsigned long test_clock() {
    const unsigned long now = g_now;
    g_now += g_tick;
    return now;
}
constexpr time_t kNov18 = 1795003200;   // Wednesday 18 November 2026, 12:00 in London
time_t g_wall = kNov18;                 // its wall clock (search::testing::set_wall_clock)
time_t test_wall() { return g_wall; }

// A search service that answers what the test tells it to, and notes what it was asked.
class Stand : public search::Provider {
public:
    explicit Stand(const char *kind) : kind_(kind) {}
    const char *kind() const override { return kind_; }
    Reply run(const std::string &query, const SearchService &service, tools::Fetch how, tools::Context &) const override {
        asked.push_back({query, service.key, how.deadline_ms, how.attempts});
        g_now += takes_ms;
        if (cancel_with) *cancel_with = true;
        while_pending();
        return replies_.empty() ? Reply() : replies_[std::min(used_++, replies_.size() - 1)];
    }
    bool tells_allowance() const override { return tells; }
    Reply allowance(const SearchService &, tools::Context &) const override {
        ++allowance_asked;
        while_pending();
        return allowance_reply;
    }
    void will(std::vector<Reply> replies) {   // one a search, in turn; the last again after that
        replies_ = std::move(replies);
        used_ = 0;
    }
    mutable std::vector<Asked> asked;
    unsigned long takes_ms = 0;                // how long each search takes, on the test's clock
    std::atomic<bool> *cancel_with = nullptr;  // set while it searches: the user pressed ESC
    bool tells = false;                        // it can be asked what is left of its allowance
    Reply allowance_reply;                     // and answers this
    mutable int allowance_asked = 0;
    // Done once while the next search or allowance asked of it is under way (its answer not in yet): what the user or
    // another thread does meanwhile.
    mutable std::function<void()> meanwhile;

private:
    void while_pending() const {
        if (!meanwhile) return;
        const std::function<void()> once = std::move(meanwhile);
        meanwhile = nullptr;
        once();
    }
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

Reply failure(Outcome outcome, long status = 0) {
    Reply r;
    r.outcome = outcome;
    r.status = status;
    return r;
}

void set_services(std::vector<SearchService> list) {
    Settings::update([&](Settings &s) { s.search = list; });
}

std::string search_for(const std::string &query) {
    tools::Context ctx;
    return search::run(query, ctx);
}

// A web service on this computer, on a port the system picks, for the checks of what is sent for real: it answers each
// request with `reply` (the raw bytes of an HTTP answer, or only the start of one), keeps the connection open `hold_ms`
// more, then closes it. It notes the requests it was sent and whether an answer could not be written whole (the other
// side had closed the connection first).
class Peer {
public:
    explicit Peer(std::string reply, int hold_ms = 0) : reply_(std::move(reply)), hold_ms_(hold_ms) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof(a);
        if (bind(fd_, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0 || listen(fd_, 16) != 0 ||
            getsockname(fd_, reinterpret_cast<sockaddr *>(&a), &len) != 0)
            perror("peer");
        port_ = ntohs(a.sin_port);
        loop_ = std::thread([this] { serve(); });
    }
    ~Peer() {
        stop_ = true;
        loop_.join();
        close(fd_);
    }
    std::string url(const std::string &path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
    int requests() const { return requests_; }
    std::string last() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return last_;
    }
    // Whether an answer was cut off by the other side, once the connections are over (two seconds at most).
    bool cut() const {
        for (int i = 0; i < 200 && busy_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return cut_;
    }

private:
    void serve() {
        while (!stop_) {
            pollfd p{fd_, POLLIN, 0};
            if (poll(&p, 1, 50) <= 0) continue;
            const int c = accept(fd_, nullptr, nullptr);
            if (c < 0) continue;
            busy_ = true;
            std::string req;
            char buf[4096];
            while (req.find("\r\n\r\n") == std::string::npos) {
                const ssize_t n = recv(c, buf, sizeof(buf), 0);
                if (n <= 0) break;
                req.append(buf, static_cast<size_t>(n));
            }
            ++requests_;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                last_ = req;
            }
            for (size_t sent = 0; sent < reply_.size();) {
                const ssize_t n = send(c, reply_.data() + sent, reply_.size() - sent, MSG_NOSIGNAL);
                if (n <= 0) { cut_ = true; break; }
                sent += static_cast<size_t>(n);
            }
            for (int waited = 0; waited < hold_ms_ && !stop_; waited += 10) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            close(c);
            busy_ = false;
        }
    }
    const std::string reply_;
    const int hold_ms_;
    int fd_ = -1, port_ = 0;
    std::thread loop_;
    std::atomic<bool> stop_{false}, busy_{false}, cut_{false};
    std::atomic<int> requests_{0};
    mutable std::mutex mtx_;
    std::string last_;
};

std::string http_answer(const std::string &status, const std::string &body, const std::string &headers = "") {
    return "HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n" + headers + "\r\n" + body;
}

// What a provider's request comes to when sent for real, the way every provider sends it (search::send).
json send_for_real(const std::string &url, const tools::Fetch &how, tools::Fetched &got, std::string &err) {
    tools::Context ctx;
    return search::send(url, how, ctx, got, err);
}

// A search service that sends its request for real, with what the layer gives it and its key in a header of its own (as
// Brave's X-Subscription-Token), to an address on this computer; and its allowance the same way.
class Sender : public search::Provider {
public:
    const char *kind() const override { return "sender"; }
    Reply run(const std::string &, const SearchService &service, tools::Fetch how, tools::Context &) const override {
        how.headers = {"X-Subscription-Token: " + service.key};
        Reply reply;
        tools::Fetched got;
        std::string err;
        send_for_real(address, how, got, err);
        reply.outcome = search::outcome_of(got, err);
        return reply;
    }
    bool tells_allowance() const override { return true; }
    Reply allowance(const SearchService &service, tools::Context &) const override {
        tools::Fetch how;
        how.headers = {"X-Subscription-Token: " + service.key};
        Reply reply;
        tools::Fetched got;
        std::string err;
        send_for_real(address + "/usage", how, got, err);
        reply.outcome = search::outcome_of(got, err);
        return reply;
    }
    std::string address;
};

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
    check(of(402, true, false, false, "HTTP 402") == Outcome::Spent, "402: the allowance spent");
    // 403 is what a firewall, a proxy or a country block answers too: a failure, not a key to stop sending.
    for (long status : {302L, 400L, 403L, 404L, 408L, 422L, 429L, 500L, 502L, 503L})
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
    check(!set_up({{"alpha", "k1", false}, {"alpha", "k2"}}), "only the first entry of a kind counts: switched off, a second is not used");
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

    alpha.will({failure(Outcome::Failing)});
    out = search_for("louvre closing hours");
    check(out.rfind("ERROR: web search is not available right now (every search service failed or has used its allowance), so "
                    "this could not be looked up. Tell the user that; if you answer from memory, say it may be out of date", 0) == 0,
          "a failure is one ERROR sentence, whatever the service said (" + out + ")");
}

// ---------------------------------------------------------------- failover
std::string usage_path() { return util::data_dir() + "/search_usage.json"; }

// Three stand-ins, set up afresh for each case: each answers "From <kind>." unless told otherwise. Nothing is
// remembered from the case before (as on a first start): no health, no cache, no allowance file.
struct Trio {
    Stand &a, &b, &c;
    void reset(std::vector<SearchService> list) {
        search::testing::forget();
        unlink(usage_path().c_str());
        g_wall = kNov18;
        set_services(std::move(list));
        for (Stand *s : {&a, &b, &c}) {
            s->asked.clear();
            s->will({answer("From " + std::string(s->kind()) + ".", {{"Title", "https://example.com/", "Text."}})});
            s->takes_ms = 0;
            s->tells = false;
            s->allowance_asked = 0;
        }
    }
};

const std::vector<SearchService> kAB = {{"alpha", "key-a"}, {"beta", "key-b"}};
const std::vector<SearchService> kABC = {{"alpha", "key-a"}, {"beta", "key-b"}, {"gamma", "key-c"}};
const std::string kAllFailed = "ERROR: web search is not available right now (every search service failed or has used its allowance)";
const std::string kNothingFound = "{\"results\":[]}";

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
    t.reset(kAB);
    t.a.will({failure(Outcome::Spent, 432), answer("From alpha.", {})});
    search_for("one");
    t.b.asked.clear();
    search_for("two");
    check(t.a.asked.size() == 1 && t.b.asked.size() == 1 && t.b.asked[0].deadline_ms == 8000 && t.b.asked[0].attempts == 2,
          "the only service that may be asked (the other spent) is given the whole 8 s and may resend");
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
        t.a.will({failure(outcome)});
        t.a.takes_ms = 5000;
        const std::string out = search_for("q");
        check(from(out, "beta"), std::string(what) + ": the next service answers (" + out + ")");
        check(t.b.asked.size() == 1 && t.b.asked[0].deadline_ms == 3000 && t.b.asked[0].attempts == 1,
              std::string(what) + ": the next service gets what is left of the 8 s, sent once (another was set up)");
    }
    t.reset(kAB);
    t.a.will({failure(Outcome::Slow)});
    t.a.takes_ms = 7500;
    const std::string out = search_for("q");
    check(t.b.asked.empty() && out.rfind(kAllFailed, 0) == 0, "under a second left of the 8: no other service is asked (" + out + ")");
}

void test_rest(Trio &t) {
    t.reset(kAB);
    t.a.will({failure(Outcome::Failing, 503)});
    search_for("one");
    search_for("two");
    check(t.a.asked.size() == 2 && t.b.asked.size() == 2, "a failing first service is asked on each of two searches");
    std::string out = search_for("three");
    check(t.a.asked.size() == 2 && from(out, "beta"), "after two failures in a row it rests: the next search goes to the second");
    t.b.will({failure(Outcome::Unreachable)});
    search_for("four");
    check(t.a.asked.size() == 3 && t.b.asked.size() == 4, "a resting service is still asked when every other has failed");
    t.b.will({answer("From beta.", {})});
    g_now += 2 * 60 * 1000;
    t.a.will({answer("From alpha.", {})});
    out = search_for("five");
    check(from(out, "alpha") && t.b.asked.size() == 4, "after two minutes the order is the user's again: the first answers");
    t.a.will({failure(Outcome::Failing, 503), answer("From alpha.", {})});
    search_for("six");
    search_for("seven");
    check(t.a.asked.size() == 6, "an answer ends the count: one failure since is no rest");
}

void test_refused_key(Trio &t) {
    t.reset(kAB);
    t.a.will({failure(Outcome::KeyRefused, 401)});
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
    t.a.will({failure(Outcome::KeyRefused, 401), answer("From alpha.", {})});
    search_for("five");
    Settings::update([](Settings &s) { s.search[0].key = "key-a2"; });   // another key put in its place by hand
    out = search_for("six");
    check(t.a.asked.size() == 4 && t.a.asked[3].key == "key-a2" && from(out, "alpha"), "another key in its place is not refused");

    t.reset({{"alpha", "key-a"}});
    t.a.will({failure(Outcome::KeyRefused, 401)});
    out = search_for("one");
    check(out.rfind("ERROR: the search service refused the key set up on this device (a new key can be typed in settings), so "
                    "this could not be looked up. Tell the user that", 0) == 0,
          "its key refused, the only service: the model is told to tell the user (" + out + ")");
    out = search_for("two");
    check(t.a.asked.size() == 1 && out.rfind("ERROR: the search service refused the key", 0) == 0, "and so again, without asking");
    t.reset(kAB);
    t.a.will({failure(Outcome::KeyRefused, 401)});
    t.b.will({failure(Outcome::KeyRefused, 422)});
    out = search_for("one");
    check(out.rfind("ERROR: the search services refused the keys set up on this device", 0) == 0, "both refused (" + out + ")");
    t.reset(kAB);
    t.a.will({failure(Outcome::KeyRefused, 401)});
    t.b.will({failure(Outcome::Failing, 500)});
    out = search_for("one");
    check(out.rfind(kAllFailed, 0) == 0, "a key refused and another service failing: the sentence for every failure (" + out + ")");
}

void test_spent(Trio &t) {
    t.reset(kAB);
    t.a.will({failure(Outcome::Spent, 432)});
    std::string out = search_for("one");
    check(from(out, "beta"), "a spent allowance: the next service answers");
    g_now += 5 * 3600 * 1000UL;
    search_for("two");
    check(t.a.asked.size() == 1, "a spent service that did not say when it comes back is passed over for hours");
    g_now += 3600 * 1000UL + 1;
    search_for("three");
    check(t.a.asked.size() == 2, "and is asked again after six");

    t.reset(kAB);
    Reply spent = failure(Outcome::Spent, 429);
    spent.reset_s = 60;
    t.a.will({spent, answer("From alpha.", {})});
    search_for("one");
    search_for("two");
    g_now += 61 * 1000;
    out = search_for("three");
    check(t.a.asked.size() == 2 && from(out, "alpha"), "one that said when its allowance comes back is asked again then");

    t.reset(kAB);
    Reply last = answer("From alpha.", {});
    last.left = 0;
    last.reset_s = 3600;
    t.a.will({last});
    out = search_for("one");
    check(from(out, "alpha"), "an answer that says nothing is left is used");
    out = search_for("two");
    check(t.a.asked.size() == 1 && from(out, "beta"), "and the service is passed over until its allowance comes back");

    t.reset({{"alpha", "key-a"}});
    t.a.will({failure(Outcome::Spent, 432)});
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
    t.b.will({failure(Outcome::Failing, 500)});
    out = search_for("q");
    check(out == "{\"results\":[]}" && t.c.asked.empty(), "nothing found, then a failure: what was found stands");
}

void test_same_shape(Trio &t) {
    const Reply same = answer("A summary.", {{"T", "https://example.com/x", "Words."}, {"", "https://example.com/y", " "},
                                             {"U", "javascript:alert(1)", "More."}});
    t.reset({{"alpha", "key-a"}});
    t.a.will({same});
    const std::string from_a = search_for("q");
    t.reset({{"beta", "key-b"}});
    t.b.will({same});
    const std::string from_b = search_for("q");
    check(from_a == from_b && from_a.find("alpha") == std::string::npos, "the model reads the same whichever service answered, and no name");
    check(from_a.find("javascript:") == std::string::npos && from_a.find("https://example.com/x") != std::string::npos,
          "only a web page's address is passed on as a source (" + from_a + ")");
    check(from_a.find("example.com/y") == std::string::npos && from_a.find("More.") != std::string::npos,
          "a result with neither title nor text is left out");
    t.reset({{"alpha", "key-a"}});
    t.a.will({answer(std::string(5000, 'S'), {})});
    check(search_for("q").size() < 1100, "a summary is capped");
}

void test_cancelled(Trio &t) {
    t.reset(kAB);
    std::atomic<bool> cancel{false};
    t.a.cancel_with = &cancel;
    t.a.will({failure(Outcome::Unreachable)});
    tools::Context ctx;
    ctx.cancel = &cancel;
    search::run("q", ctx);
    search::run("q", ctx);
    t.a.cancel_with = nullptr;
    check(t.b.asked.empty() && t.a.asked.size() == 1, "cancelled: no other service is asked, and nothing once cancelled");
    t.a.will({failure(Outcome::Failing, 503)});
    search_for("one");
    search_for("two");
    check(t.a.asked.size() == 3, "a cancelled search is no failure of the service: one failure since is no rest");
    // ESC just after the service answered with an error: the model reads that it was cancelled, never the answer's words.
    t.reset(kAB);
    cancel = false;
    t.a.cancel_with = &cancel;
    t.a.will({failure(Outcome::Failing)});
    const std::string out = search::run("r", ctx);
    t.a.cancel_with = nullptr;
    check(out == "ERROR: cancelled", "cancelled after an error answer: \"cancelled\", nothing of the answer (" + out + ")");
}

// ---------------------------------------------------------------- the allowance file
namespace usage = search::usage;

const std::string kIdA = search::id_of({"alpha", "key-a"}), kIdB = search::id_of({"beta", "key-b"});

json usage_file() { return util::load_json(usage_path(), json::object()); }

void test_counts(Trio &t) {
    t.reset(kAB);
    search_for("one");
    search_for("two");
    check(usage::of(kIdA).used == 2 && usage::of(kIdB).used == 0 && usage::today() == 2,
          "two searches answered: counted for the service that answered, and for the day");
    t.a.will({failure(Outcome::Failing, 503)});
    search_for("three");
    check(usage::of(kIdA).used == 2 && usage::of(kIdB).used == 1 && usage::today() == 3,
          "a search that failed over is one search of the day, and counts only for the service that answered");
    Reply said = answer("From alpha.", {{"Title", "https://example.com/", "Text."}});
    said.left = 812;
    said.limit = 1000;
    t.a.will({said});
    search_for("four");
    const usage::Figures f = usage::of(kIdA);
    check(f.left == 812 && f.limit == 1000 && f.said_at == kNov18, "what the service said is left is kept, with when it said it");

    const json file = usage_file();
    check(util::jget(file, "today", 0) == 4 && file.contains("services") && file["services"].contains(kIdA) &&
              util::jget(file["services"][kIdA], "used", 0) == 3 && util::jget(file["services"][kIdA], "left", 0) == 812,
          "the counts and the figure are in the file (" + file.dump() + ")");
    const std::string text = file.dump();
    check(text.find("key-a") == std::string::npos && text.find("four") == std::string::npos && text.find("From") == std::string::npos,
          "the file holds no key, no query and no result");
    struct stat st{};
    check(stat(usage_path().c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "the file can be read by its owner only");

    search::testing::forget();   // a restart: the file is read again
    check(usage::of(kIdA).used == 3 && usage::of(kIdA).left == 812 && usage::today() == 4, "a later run reads the counts back");
    set_services({{"alpha", "key-a2"}, {"beta", "key-b"}});
    search_for("five");
    check(usage::of(search::id_of({"alpha", "key-a2"})).used == 1 && !usage_file()["services"].contains(kIdA),
          "another key is another allowance: counted afresh, the old key's figures dropped from the file");
}

void test_periods(Trio &t) {
    t.reset(kAB);
    search_for("one");
    g_wall = kNov18 + 86400;
    check(usage::today() == 0 && usage::of(kIdA).used == 1, "a new day: the day's count starts again, the month's goes on");
    g_wall = kNov18 + 13 * 86400;   // 1 December
    check(usage::of(kIdA).used == 0, "a new month: the month's count starts again");
    search_for("two");
    g_wall = 1000;   // 1970: the clock has not been set (a board without a battery-backed clock, before its time sync)
    search::testing::forget();
    search_for("three");
    check(usage::of(kIdA).used == 2 && usage::today() == 2, "the clock not set: nothing rolls over, the counts go on");
    Reply said = answer("From alpha.", {{"Title", "https://example.com/", "Text."}});
    said.left = 5;
    t.a.will({said});
    search_for("four");
    check(usage::of(kIdA).said_at == 0 && usage::of(kIdA).left == 5, "a figure said while the clock is not set is kept undated");
    g_wall = kNov18 + 13 * 86400 + 3600;
    check(usage::of(kIdA).used == 3, "the clock set again in the same month: the count it held goes on");

    t.reset(kAB);
    g_wall = 1000;
    search_for("one");
    g_wall = kNov18;
    check(usage::of(kIdA).used == 1 && usage::today() == 1, "counted before the clock was ever set: the first month and day it reads");
}

void test_spent_kept(Trio &t) {
    t.reset(kAB);
    Reply spent = failure(Outcome::Spent, 432);
    spent.reset_s = 3 * 86400;
    t.a.will({spent, answer("From alpha.", {{"Title", "https://example.com/", "Text."}})});
    search_for("one");
    search::testing::forget();   // a restart
    std::string out = search_for("two");
    check(t.a.asked.size() == 1 && from(out, "beta"), "a spent allowance is passed over by a later run too (" + out + ")");
    const time_t until = usage::of(kIdA).spent_until;
    check(until >= kNov18 + 3 * 86400 - 1 && until <= kNov18 + 3 * 86400 + 1, "until the end the service gave");
    search::testing::forget();
    g_wall = 1000;   // a later run, before the clock is set
    search_for("three");
    check(t.a.asked.size() == 1, "while the clock is not set, the end the file holds stands");
    g_wall = kNov18 + 3 * 86400 + 60;
    out = search_for("four");
    check(t.a.asked.size() == 2 && from(out, "alpha"), "once the clock says the end has passed, the service is asked again");
}

void test_daily_cap(Trio &t) {
    t.reset(kAB);
    search_for("kept for later");
    util::save_json(usage_path(), {{"version", 1}, {"day", util::format_time(kNov18, "%Y-%m-%d")}, {"today", usage::kDailyCap}});
    search::testing::forget();   // as a run would find the file: the cache gone too
    std::string out = search_for("one more");
    check(t.a.asked.size() == 1 && t.b.asked.empty() &&
              out.rfind("ERROR: web search has reached this device's limit of 100 searches a day (it starts again tomorrow), so "
                        "this could not be looked up. Tell the user that", 0) == 0,
          "past the day's cap: one sentence for the model, and no service is asked (" + out + ")");
    g_wall = kNov18 + 86400;
    out = search_for("one more");
    check(from(out, "alpha"), "the next day, search works again");
}

// ---------------------------------------------------------------- the cache
void test_cache(Trio &t) {
    t.reset(kAB);
    const std::string first = search_for("Louvre  opening hours");
    std::string out = search_for(" louvre opening\tHOURS ");
    check(out == first && t.a.asked.size() == 1, "the same query in other capitals and spacing is answered from the cache");
    check(usage::today() == 1 && usage::of(kIdA).used == 1, "an answer from the cache is no search: not counted");
    g_now += 10 * 60 * 1000;
    search_for("louvre opening hours");
    check(t.a.asked.size() == 2, "after ten minutes the service is asked again");

    t.reset(kAB);
    t.a.will({failure(Outcome::Failing, 503)});
    t.b.will({failure(Outcome::Failing, 503)});
    search_for("q");
    search_for("q");
    check(t.a.asked.size() == 2 && t.b.asked.size() == 2, "a failure is not kept");
    t.reset({{"alpha", "key-a"}});
    t.a.will({answer("", {})});
    search_for("q");
    search_for("q");
    check(t.a.asked.size() == 2, "an answer that found nothing is not kept");
    t.a.will({answer("", {{"", "https://example.com/", " "}})});
    search_for("r");
    search_for("r");
    check(t.a.asked.size() == 4, "nor one whose results all said nothing");

    t.reset(kAB);
    search_for("q");
    set_services({{"beta", "key-b"}, {"alpha", "key-a"}});
    out = search_for("q");
    check(from(out, "beta"), "the services set up changed (another order): the cache is emptied");
    set_services({{"beta", "key-b"}, {"alpha", "key-a", false}});
    search_for("q");
    check(t.b.asked.size() == 2, "one switched off: emptied again");

    t.reset(kAB);
    for (int i = 0; i <= 32; ++i) search_for("query " + std::to_string(i));
    search_for("query 1");
    check(t.a.asked.size() == 33, "32 answers are kept: the newest");
    search_for("query 0");
    check(t.a.asked.size() == 34, "the oldest went first");
}

// ---------------------------------------------------------------- what the settings page says
void test_states(Trio &t) {
    t.reset(kAB);
    const SearchService a{"alpha", "key-a"};
    check(search::describe(a) == "ready", "a service not asked yet in this run: ready");
    search_for("one");
    check(search::describe(a) == "working", "it answered: working");
    t.a.will({failure(Outcome::Failing, 503)});
    search_for("two");
    check(search::describe(a) == "failed last time", "one failure: failed last time");
    search_for("three");
    check(search::describe(a) == "resting after failures", "two in a row: resting after failures");
    check(search::describe({"alpha", "key-a", false}) == "off" && search::describe({"alpha", ""}) == "no key", "off, no key");
    SearchService marked = a;
    marked.refused = search::key_print("key-a");
    check(search::describe(marked) == "key refused: type it again" && search::state_of(marked) == search::State::KeyRefused,
          "its key refused: type it again");
    for (const long reset_s : {3 * 86400L, 2 * 3600L}) {
        t.reset(kAB);
        Reply spent = failure(Outcome::Spent, 429);
        spent.reset_s = reset_s;
        t.a.will({spent});
        search_for("one");
        const std::string want = "allowance used until " + util::format_time(kNov18 + reset_s, reset_s > 86400 ? "%-d %B" : "%H:%M");
        check(search::describe(a) == want, "spent: " + want + " (" + search::describe(a) + ")");
    }
}

void test_ask_allowance(Trio &t) {
    t.reset(kAB);
    tools::Context ctx;
    check(search::ask_allowance("alpha", ctx) == "This service says what is left only with its searches." && t.a.allowance_asked == 0,
          "a service that cannot be asked is not");
    t.a.tells = true;
    t.a.allowance_reply.outcome = Outcome::Ok;
    t.a.allowance_reply.left = 850;
    t.a.allowance_reply.limit = 1000;
    check(search::ask_allowance("alpha", ctx).empty() && usage::of(kIdA).left == 850 && usage::of(kIdA).limit == 1000,
          "asked what is left: the figure is kept");
    check(util::jget(usage_file()["services"][kIdA], "left", 0) == 850 && usage::today() == 0 && usage::of(kIdA).used == 0,
          "in the file, and asking is no search");
    check(search::ask_allowance("alpha", ctx).rfind("Asked a moment ago", 0) == 0 && t.a.allowance_asked == 1, "at most once a minute");
    g_now += 61 * 1000;
    t.a.allowance_reply.left = 0;
    search::ask_allowance("alpha", ctx);
    check(usage::of(kIdA).spent && t.a.allowance_asked == 2, "nothing left: the service is passed over");
    g_now += 61 * 1000;
    t.a.allowance_reply.left = 40;
    search::ask_allowance("alpha", ctx);
    check(!usage::of(kIdA).spent, "something left again (a plan changed): asked again");
    g_now += 61 * 1000;
    t.a.allowance_reply = failure(Outcome::Failing, 500);
    const std::string why = search::ask_allowance("alpha", ctx);
    check(why == "The service did not say: it answered with an error (HTTP 500).", "an error, in the app's own words (" + why + ")");
    g_now += 61 * 1000;
    t.a.allowance_reply = failure(Outcome::KeyRefused, 401);
    check(search::ask_allowance("alpha", ctx).find("refused this key") != std::string::npos &&
              Settings::get().search[0].refused == search::key_print("key-a"),
          "a key refused when asked is marked refused, as a search's would be");
    g_now += 61 * 1000;
    search::ask_allowance("alpha", ctx);
    check(t.a.allowance_asked == 5, "and not sent again");
}


// ---------------------------------------------------------------- review fixes (search, A)
// What is sent for real: a redirect is not followed (the key would go with it), a refusal stands whatever happened to its
// body, an answer longer than any service sends is cut off.
void test_sent_for_real(Sender &sender) {
    search::testing::forget();
    Peer elsewhere(http_answer("200 OK", "{}"));
    Peer service(http_answer("302 Found", "", "Location: " + elsewhere.url("/landed") + "\r\n"));
    set_services({{"sender", "secret-token-0001"}});
    sender.address = service.url("/search");
    std::string out = search_for("where does it go");
    check(service.requests() == 1 && service.last().find("secret-token-0001") != std::string::npos && elsewhere.requests() == 0,
          "a search's redirect is not followed: its key goes nowhere else (" + std::to_string(elsewhere.requests()) + " there)");
    check(out.rfind(kAllFailed, 0) == 0, "and the search failed (" + out + ")");
    tools::Context ctx;
    search::ask_allowance("sender", ctx);
    check(service.requests() == 2 && elsewhere.requests() == 0, "nor an allowance lookup's");

    // A refusal is the service's answer, whatever then happened to its body: not sent again, and a refusal, not "no
    // answer" (a body cut short) or "too slow" (a body that stalls).
    const std::string refusal = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 200\r\n"
                                "Connection: close\r\n\r\n{\"detail\": ";
    {
        Peer peer(refusal);
        tools::Fetch how;
        tools::Fetched got;
        std::string err;
        send_for_real(peer.url("/search"), how, got, err);
        check(peer.requests() == 1 && search::outcome_of(got, err) == Outcome::KeyRefused,
              "a 401 whose body was cut short is a refusal, sent once (" + std::to_string(peer.requests()) + " sent: " + err + ")");
    }
    {
        Peer peer(refusal, 3000);
        tools::Fetch how;
        how.deadline_ms = 1000;
        tools::Fetched got;
        std::string err;
        send_for_real(peer.url("/search"), how, got, err);
        check(search::outcome_of(got, err) == Outcome::KeyRefused, "a 401 whose body stalls past the deadline is a refusal (" + err + ")");
    }
    {
        Peer peer(refusal);
        search::testing::forget();
        set_services({{"sender", "secret-token-0001"}});
        sender.address = peer.url("/search");
        search_for("refused");
        check(peer.requests() == 1 && !Settings::get().search[0].refused.empty(),
              "through the layer, the only service: marked refused, sent once (" + std::to_string(peer.requests()) + ")");
    }

    // An answer is read up to half a megabyte: one of 32 MB is cut off, not read to its end, and is no answer.
    {
        Peer peer(http_answer("200 OK", "{\"results\": [], \"pad\": \"" + std::string(32 << 20, 'a') + "\"}"));
        search::testing::forget();
        set_services({{"sender", "secret-token-0001"}});
        sender.address = peer.url("/search");
        out = search_for("a long answer");
        check(peer.cut() && out.rfind(kAllFailed, 0) == 0, "an answer of 32 MB is cut off and read as no answer (" + out.substr(0, 80) + ")");
    }
    {
        Peer peer(http_answer("200 OK", "{\"results\": [], \"pad\": \"" + std::string(400 << 10, 'a') + "\"}"));
        search::testing::forget();
        sender.address = peer.url("/search");
        out = search_for("a long answer read whole");
        check(!peer.cut() && out == kNothingFound, "one of 400 kB is read whole (" + out.substr(0, 80) + ")");
    }
}

// The keys set up are taken out of what a service answered before the model reads it or the cache keeps it.
void test_keys_taken_out(Trio &t) {
    t.reset({{"alpha", "sk-alpha-secret-1"}, {"beta", "sk-beta-secret-22"}});
    Settings::update([](Settings &s) { s.api_key = "sk-ai-service-key-3"; });
    const std::string echo = "keys: sk-alpha-secret-1, sk-beta-secret-22, sk-ai-service-key-3";
    t.a.will({answer("Summary " + echo, {{"Title " + echo, "https://example.com/?k=sk-alpha-secret-1", "Text " + echo}})});
    const std::string out = search_for("q");
    check(out.find("secret") == std::string::npos && out.find("sk-ai-service") == std::string::npos &&
              out.find("[removed]") != std::string::npos && out.find("Summary keys") != std::string::npos,
          "a key a service echoed is taken out of the summary, the title, the address and the text (" + out + ")");
    check(search_for("q") == out && t.a.asked.size() == 1, "and the cache keeps it taken out");
    Settings::update([](Settings &s) { s.api_key.clear(); });
}

// What a service said, before it is used: a result that says nothing is no result, so an answer of only such is nothing
// found (the next service is asked), and an answer without results is not kept.
void test_unusable_hits(Trio &t) {
    t.reset(kAB);
    t.a.will({answer("", {{"", "https://example.com/1", " "}, {"\x01\x02", "https://example.com/2", ""}})});
    std::string out = search_for("q");
    check(from(out, "beta"), "an answer whose results all say nothing found nothing: the next service is asked (" + out + ")");
    t.reset(kAB);
    t.a.will({answer("  ", {})});
    out = search_for("q");
    check(from(out, "beta"), "a blank summary is no summary (" + out + ")");
    t.reset({{"alpha", "key-a"}});
    t.a.will({answer("Only a summary.", {})});
    search_for("q");
    search_for("q");
    check(t.a.asked.size() == 2, "an answer without results is not kept");
}

// The settings may change while a service's answer is awaited: the search sends only what they still set up, and a
// refusal of a key typed again meanwhile marks nothing.
void test_settings_meanwhile(Trio &t) {
    const std::vector<std::pair<const char *, std::function<void(Settings &)>>> kChanges = {
        {"refused meanwhile (the settings page's lookup)", [](Settings &s) { s.search[1].refused = search::key_print("key-b"); }},
        {"switched off meanwhile", [](Settings &s) { s.search[1].enabled = false; }},
        {"another key put in meanwhile", [](Settings &s) { s.search[1].key = "key-b2"; }},
        {"removed meanwhile", [](Settings &s) { s.search.pop_back(); }}};
    for (const auto &[what, change] : kChanges) {
        t.reset(kAB);
        t.a.will({failure(Outcome::Failing, 503)});
        t.a.meanwhile = [change = change] { Settings::update(change); };
        search_for("q");
        check(t.a.asked.size() == 1 && t.b.asked.empty(), std::string(what) + ": the second service is not sent");
    }
    t.reset(kAB);
    t.a.will({failure(Outcome::Failing, 503)});
    t.a.meanwhile = [] { search::usage::note(search::id_of({"beta", "key-b"}), failure(Outcome::Spent, 432)); };
    search_for("q");
    check(t.b.asked.empty(), "its allowance found spent meanwhile (another search): the second service is not sent");

    t.reset({{"alpha", "key-a"}, {"alpha", "key-a"}, {"beta", "key-b"}});
    t.a.will({failure(Outcome::KeyRefused, 401)});
    std::string out = search_for("q");
    check(t.a.asked.size() == 1 && from(out, "beta"), "one service twice in the list: a refused key is sent once");
    t.reset({{"alpha", "key-a", false}, {"alpha", "key-a2"}, {"beta", "key-b"}});
    out = search_for("q");
    check(t.a.asked.empty() && from(out, "beta"), "a second entry of a kind is not used (" + out + ")");

    t.reset({{"alpha", "key-a"}});
    t.a.will({failure(Outcome::KeyRefused, 401)});
    t.a.meanwhile = [] { Settings::update([](Settings &s) { s.set_search_key("alpha", "key-a"); }); };
    search_for("q");
    check(Settings::get().search[0].refused.empty(), "a refusal of a key typed again meanwhile (unchanged) marks nothing");
    t.reset({{"alpha", "key-a"}});
    t.a.tells = true;
    t.a.allowance_reply = failure(Outcome::KeyRefused, 401);
    t.a.meanwhile = [] { Settings::update([](Settings &s) { s.set_search_key("alpha", "key-a"); }); };
    tools::Context ctx;
    search::ask_allowance("alpha", ctx);
    check(Settings::get().search[0].refused.empty(), "nor does an allowance lookup's");
}

// The allowance: a lookup's late answer does not undo what a search learnt since; an end learnt before the clock was set
// is kept by a later run; the time left is read from one reading of the clock.
void test_allowance_late_and_early(Trio &t) {
    t.reset(kAB);
    t.a.tells = true;
    t.a.allowance_reply.outcome = Outcome::Ok;
    t.a.allowance_reply.left = 1;
    t.a.allowance_reply.limit = 1000;
    t.a.meanwhile = [&t] {
        t.a.will({failure(Outcome::Spent, 432)});
        search_for("spent meanwhile");
    };
    tools::Context ctx;
    search::ask_allowance("alpha", ctx);
    check(search::usage::of(kIdA).spent, "a lookup answered after a search found the allowance spent does not clear the mark");

    t.reset(kAB);
    g_wall = 1000;   // the clock not set yet
    Reply spent = failure(Outcome::Spent, 432);
    spent.reset_s = 3600;
    t.a.will({spent, answer("From alpha.", {{"Title", "https://example.com/", "Text."}})});
    search_for("one");
    search::testing::forget();   // a restart, the clock still not set
    std::string out = search_for("two");
    check(t.a.asked.size() == 1 && from(out, "beta"), "spent, learnt before the clock was set: a later run passes it over (" + out + ")");
    g_wall = kNov18;
    search_for("three");
    check(t.a.asked.size() == 1, "and so once the clock is set, until its hour is up");
    g_now += 3600 * 1000UL + 1;
    out = search_for("four");
    check(t.a.asked.size() == 2 && from(out, "alpha"), "then it is asked again (" + out + ")");

    t.reset(kAB);
    spent.reset_s = 60;
    t.a.will({spent});
    search_for("one");
    g_now += 60 * 1000 - 5;
    g_tick = 2000;   // the clock moves on two seconds between two readings (the thread was held up)
    const search::usage::Figures f = search::usage::of(kIdA);
    g_tick = 0;
    check(f.spent_until <= g_wall + 1 && (f.spent_until != 0) == f.spent,
          "the time left is read once: no end far in the future (" + std::to_string(f.spent_until - g_wall) + " s)");
}

// The counts when the clock steps back: they stay in the period they were counted in.
void test_clock_back(Trio &t) {
    t.reset(kAB);
    g_wall = kNov18 + 86400;   // 19 November
    search_for("one");
    g_wall = kNov18;           // a day back
    check(search::usage::today() == 1, "the clock a day back: the day's count stands (" + std::to_string(search::usage::today()) + ")");
    search_for("two");
    g_wall = kNov18 + 86400;
    check(search::usage::today() == 2, "and counts on in the day it was counted in");
    g_wall = kNov18 + 2 * 86400;
    check(search::usage::today() == 0, "the next day it starts again");
    t.reset(kAB);
    g_wall = kNov18 + 13 * 86400;   // 1 December
    search_for("one");
    g_wall = kNov18 + 12 * 86400;   // 30 November
    check(search::usage::of(kIdA).used == 1, "the month's count stands too");
    t.reset(kAB);
    g_wall = kNov18 + 400 * 86400;   // a clock set a year ahead, then put right
    search_for("one");
    g_wall = kNov18;
    check(search::usage::today() == 0, "a clock put right after running far ahead: the counts start again at once");

    t.reset(kAB);
    std::atomic<bool> cancel{true};
    tools::Context ctx;
    ctx.cancel = &cancel;
    search::run("q", ctx);
    check(search::usage::today() == 0 && t.a.asked.empty(), "a search cancelled before anything was sent is not counted");
}

// The cache answers only for the settings it was filled under: a change to the services, even one taken back, and a
// refusal empty it, and an answer that came while they changed is not kept.
void test_cache_and_settings(Trio &t) {
    t.reset(kAB);
    search_for("q");
    set_services({{"alpha", "key-a", false}, {"beta", "key-b", false}});
    search_for("q");
    set_services(kAB);
    search_for("q");
    check(t.a.asked.size() == 2, "switched off and on again: not answered from the cache");
    t.reset(kAB);
    search_for("q");
    Settings::update([](Settings &s) { s.search[1].refused = search::key_print("key-b"); });
    search_for("q");
    check(t.a.asked.size() == 2, "a key refused since: not answered from the cache");
    t.reset(kAB);
    t.a.meanwhile = [] {
        Settings::update([](Settings &s) { s.search[1].enabled = false; });
        Settings::update([](Settings &s) { s.search[1].enabled = true; });
    };
    search_for("q");
    search_for("q");
    check(t.a.asked.size() == 2, "an answer that came while the settings changed is not kept");
}

}  // namespace

int main(int argc, char **argv) {
    const std::string dir = argc > 1 ? argv[1] : "/tmp/zb-search-test";
    mkdir(dir.c_str(), 0700);
    setenv("XDG_CONFIG_HOME", (dir + "/config").c_str(), 1);
    setenv("XDG_DATA_HOME", (dir + "/data").c_str(), 1);
    setenv("XDG_CACHE_HOME", (dir + "/cache").c_str(), 1);
    unsetenv("ZB_SEARCH_KEY");
    // Nothing here reaches a real service: should anything be sent elsewhere than this computer's own peers, it goes to a
    // closed port on this computer.
    for (const char *proxy : {"http_proxy", "https_proxy", "HTTPS_PROXY", "ALL_PROXY", "all_proxy"}) setenv(proxy, "http://127.0.0.1:9", 1);
    setenv("no_proxy", "127.0.0.1", 1);   // this computer's own peers (Peer) are reached directly
    unsetenv("NO_PROXY");
    Settings::load();

    test_builtin_registry();
    test_outcomes();

    search::testing::clear_providers();
    search::testing::set_clock(test_clock);
    search::testing::set_wall_clock(test_wall);
    auto *alpha = new Stand("alpha"), *beta = new Stand("beta"), *gamma = new Stand("gamma");
    auto *sender = new Sender();
    search::register_provider(std::unique_ptr<search::Provider>(alpha), "Alpha");
    search::register_provider(std::unique_ptr<search::Provider>(beta), "Beta");
    search::register_provider(std::unique_ptr<search::Provider>(gamma), "Gamma");
    search::register_provider(std::unique_ptr<search::Provider>(sender), "Sender");
    check(search::registry().size() == 4 && search::provider_for("tavily") == nullptr,
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
    test_counts(t);
    test_periods(t);
    test_spent_kept(t);
    test_daily_cap(t);
    test_cache(t);
    test_states(t);
    test_ask_allowance(t);
    test_sent_for_real(*sender);
    test_keys_taken_out(t);
    test_unusable_hits(t);
    test_settings_meanwhile(t);
    test_allowance_late_and_early(t);
    test_clock_back(t);
    test_cache_and_settings(t);

    printf("search: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
