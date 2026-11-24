// Reading from a web service. Every tool and search service that reads from the network does it through
// fetch_json, which retries what is safe to send again, and every address it is given goes through
// experiment::web() first (the test sets' stand-in web; tests/sim/tools_cli.py checks both). Definition in
// web.cpp.
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "agent/tools.h"

namespace tools {

struct Fetched;

// How to read. The defaults are a tool's everyday read: a GET, sent once more after a failure on the way,
// all inside 20 seconds.
struct Fetch {
    std::vector<std::string> headers;          // "Name: value"
    std::string post_body;                     // when set, POSTed
    long deadline_ms = 20000;                  // the whole read, a resend included
    int attempts = 2;                          // sends at most
    std::vector<std::string> keep_headers;     // reply headers to hand back in Fetched, by name
    // A redirect is answered as the failure it is, never followed: for a request that carries a key in a header
    // (libcurl hands a header of the caller's own to whatever host a redirect names).
    bool no_redirect = false;
    // An answer longer than this, decompressed, is cut off where it passes it (the transfer ends there) and read
    // as garbled; 0: no limit of the caller's own.
    size_t max_bytes = 0;
    // A failed answer whose meaning the caller already knows and a resend cannot change (a refused key, a spent
    // allowance): it is not sent again, whatever its status would otherwise allow. Given what the answer came
    // to and its `err`.
    std::function<bool(const Fetched &, const std::string &)> settled;
};

// What came back besides the JSON, for a caller that tells failures apart (a search service: a refused
// key, a spent allowance, a slow or garbled answer).
struct Fetched {
    long status = 0;                           // the last answer's HTTP status, kept when its body then failed; 0 when none came
    bool sent = false;                         // the request went out (http::Response::sent)
    bool timed_out = false;                    // the deadline passed before the answer was in
    bool garbled = false;                      // a success whose body is not JSON this app reads, or is too long
    std::map<std::string, std::string> headers;   // those Fetch::keep_headers asked for, by lower-case name
    std::string body;                          // an error answer's body, as it came (a service's own error code)
};

// The JSON a read of `url` answered; null with `err` set when it failed. `got`, when given, says more.
// An answer with a status of its own (3xx, 4xx, 5xx) is what the service said, whatever then happened to its
// body: it is read by that status (a 401 cut short is still a refusal) and sent again only when the status
// allows (http::transient: 408, 429, 5xx but 501 and 505).
json fetch_json(const std::string &url, std::string &err, Context &ctx, const Fetch &how = {}, Fetched *got = nullptr);

}  // namespace tools
