// fetch_json: every read a tool makes from a web service (web.h).
#include "agent/tools/web.h"
#include "agent/tools/common.h"

#include "agent/experiment.h"
#include "net/http.h"

namespace tools {

// Every caller reads (a forecast, a rate, a price, a place, a search), so a request that failed on
// the way is sent once more after half a second, inside the same 20 seconds.
json fetch_json(const std::string &url, std::string &err, Context &ctx, const std::vector<std::string> &headers,
                const std::string &post_body) {
    http::Request req;
    req.url = url;
    req.headers = headers;
    req.timeout_s = 20;
    req.cancel = ctx.cancel;
    // The test sets' stand-in web (ZB_EXP_WEB, experiment builds only) is reached directly: no proxy,
    // and a redirect is not followed off this computer.
    req.local = experiment::stand_in(url);
    if (!post_body.empty()) { req.method = "POST"; req.body = post_body; }
    http::Retry retry;
    retry.attempts = 2;
    retry.budget_ms = req.timeout_s * 1000;
    const http::Response res = http::send_retrying(req, retry);
    if (!res.ok()) {
        if (res.error == "no connection") err = "no internet connection";
        else if (!res.error.empty()) err = res.error;
        else {
            // An error page is HTML, which says nothing the model can use; a JSON or plain answer may.
            const std::string body = util::trim(res.body);
            err = "the service answered with an error (HTTP " + std::to_string(res.status) + ")";
            if (!body.empty() && body[0] != '<') err += ": " + clean_text(util::truncate_utf8(body, 160));
        }
        return json();
    }
    json j = util::parse_untrusted_json(res.body);
    if (j.is_discarded()) { err = "unexpected reply from the service"; return json(); }
    return j;
}

}  // namespace tools
