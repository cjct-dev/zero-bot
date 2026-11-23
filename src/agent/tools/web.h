// Reading from a web service. Every tool and search service that reads from the network does it through
// fetch_json, which retries what is safe to send again, and every address it is given goes through
// experiment::web() first (the test sets' stand-in web; tests/sim/tools_cli.py checks both). Definition in
// web.cpp.
#pragma once
#include <map>
#include <string>
#include <vector>

#include "agent/tools.h"

namespace tools {

// How to read. The defaults are a tool's everyday read: a GET, sent once more after a failure on the way,
// all inside 20 seconds.
struct Fetch {
    std::vector<std::string> headers;          // "Name: value"
    std::string post_body;                     // when set, POSTed
    long deadline_ms = 20000;                  // the whole read, a resend included
    int attempts = 2;                          // sends at most
    std::vector<std::string> keep_headers;     // reply headers to hand back in Fetched, by name
};

// What came back besides the JSON, for a caller that tells failures apart (a search service: a refused
// key, a spent allowance, a slow or garbled answer).
struct Fetched {
    long status = 0;                           // the last answer's HTTP status; 0 when none came
    bool sent = false;                         // the request went out (http::Response::sent)
    bool timed_out = false;                    // the deadline passed before the answer was in
    bool garbled = false;                      // a success whose body is not JSON this app reads
    std::map<std::string, std::string> headers;   // those Fetch::keep_headers asked for, by lower-case name
};

// The JSON a read of `url` answered; null with `err` set when it failed. `got`, when given, says more.
json fetch_json(const std::string &url, std::string &err, Context &ctx, const Fetch &how = {}, Fetched *got = nullptr);

}  // namespace tools
