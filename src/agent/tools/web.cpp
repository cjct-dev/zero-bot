// fetch_json: every read a tool or a search service makes from a web service (web.h).
#include "agent/tools/web.h"
#include "agent/tools/common.h"

#include "agent/experiment.h"
#include "net/http.h"

namespace tools {

// Every caller reads (a forecast, a rate, a price, a place, a search), so a request that failed on
// the way may be sent again after half a second, inside the same deadline.
json fetch_json(const std::string &url, std::string &err, Context &ctx, const Fetch &how, Fetched *got) {
    http::Request req;
    req.url = url;
    req.headers = how.headers;
    req.keep_headers = how.keep_headers;
    req.timeout_ms = how.deadline_ms;
    req.cancel = ctx.cancel;
    // The test sets' stand-in web (ZB_EXP_WEB, experiment builds only) is reached directly: no proxy,
    // and a redirect is not followed off this computer.
    req.local = experiment::stand_in(url);
    if (!how.post_body.empty()) { req.method = "POST"; req.body = how.post_body; }
    http::Retry retry;
    retry.attempts = how.attempts;
    retry.budget_ms = how.deadline_ms;
    const http::Response res = http::send_retrying(req, retry);
    Fetched seen;
    seen.status = res.status;
    seen.sent = res.sent;
    seen.timed_out = res.error == "timed out";
    seen.headers = res.headers;
    json j;
    if (!res.ok()) {
        if (res.error == "no connection") err = "no internet connection";
        else if (!res.error.empty()) err = res.error;
        else {
            // An error page is HTML, which says nothing the model can use; a JSON or plain answer may.
            const std::string body = util::trim(res.body);
            err = "the service answered with an error (HTTP " + std::to_string(res.status) + ")";
            if (!body.empty() && body[0] != '<') err += ": " + clean_text(util::truncate_utf8(body, 160));
        }
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
