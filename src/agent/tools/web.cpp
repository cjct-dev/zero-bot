// fetch_json: every read a tool or a search service makes from a web service (web.h).
#include "agent/tools/web.h"
#include "agent/tools/common.h"

#include <atomic>

#include "agent/experiment.h"
#include "net/http.h"

namespace tools {

namespace {

// What came back besides the JSON: an error answer's body as far as the caller reads one.
Fetched seen_in(const http::Response &res, size_t max_bytes) {
    Fetched seen;
    seen.status = res.status;
    seen.sent = res.sent;
    seen.timed_out = res.error == "timed out";
    seen.headers = res.headers;
    if (res.status >= 300) seen.body = max_bytes ? res.body.substr(0, max_bytes) : res.body;
    return seen;
}

// Why a read failed, in a sentence: the status the service answered with, whatever then happened to its body (with
// its words, unless they are an error page's HTML, which says nothing the model can use), else what went wrong on
// the way.
std::string failure_of(const http::Response &res) {
    if (res.status >= 300 || res.error.empty()) {
        const std::string body = util::trim(res.body);
        std::string err = "the service answered with an error (HTTP " + std::to_string(res.status) + ")";
        if (!body.empty() && body[0] != '<') err += ": " + clean_text(util::truncate_utf8(body, 160));
        return err;
    }
    return res.error == "no connection" ? "no internet connection" : res.error;
}

// Whether a failed send may be made again: as any read may (http::transient), except that an answer with a status
// of its own is judged by that status, whatever happened to its body after it (a 401 cut short is a refusal, not a
// connection dropped), and that one whose meaning the caller already knows is never sent again (Fetch::settled).
bool again(const http::Response &res, const Fetch &how) {
    if (res.ok()) return false;
    if (how.settled && how.settled(seen_in(res, how.max_bytes), failure_of(res))) return false;
    if (res.status < 300 || res.error == "timed out") return http::transient(res);
    http::Response answer = res;
    answer.error.clear();
    return http::transient(answer);
}

}  // namespace

// Every caller reads (a forecast, a rate, a price, a place, a search), so a request that failed on
// the way may be sent again after half a second, inside the same deadline.
json fetch_json(const std::string &url, std::string &err, Context &ctx, const Fetch &how, Fetched *got) {
    http::Request req;
    req.url = url;
    req.headers = how.headers;
    req.keep_headers = how.keep_headers;
    req.timeout_ms = how.deadline_ms;
    req.cancel = ctx.cancel;
    req.no_redirect = how.no_redirect;
    // The test sets' stand-in web (ZB_EXP_WEB, experiment builds only) is reached directly: no proxy,
    // and a redirect is not followed off this computer.
    req.local = experiment::stand_in(url);
    if (!how.post_body.empty()) { req.method = "POST"; req.body = how.post_body; }
    // With a limit, a success's body is read here, and the transfer ends once it passes the limit.
    std::string body;
    std::atomic<bool> too_long{false};
    if (how.max_bytes) {
        req.on_data = [&](const char *data, size_t n) {
            if (too_long || body.size() + n > how.max_bytes) too_long = true;
            else body.append(data, n);
        };
        req.cancel = &too_long;
        req.cancel_too = ctx.cancel;   // ESC ends it as ever
    }
    http::Retry retry;
    retry.attempts = how.attempts;
    retry.budget_ms = how.deadline_ms;
    retry.again = [&how](const http::Response &res) { return again(res, how); };
    retry.before_resend = [&body] { body.clear(); };
    http::Response res = http::send_retrying(req, retry);
    if (how.max_bytes && res.ok()) res.body = std::move(body);
    Fetched seen = seen_in(res, how.max_bytes);
    json j;
    if (too_long) {
        err = "the answer was too long to read";
        seen.garbled = true;
    } else if (!res.ok()) {
        err = failure_of(res);
    } else {
        j = util::parse_untrusted_json(res.body);
        if (j.is_discarded()) {
            err = "unexpected reply from the service";
            seen.garbled = true;
            j = json();
        }
    }
    if (got) *got = std::move(seen);
    return j;
}

}  // namespace tools
