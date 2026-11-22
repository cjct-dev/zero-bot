// Reading from a web service. Every tool that reads from the network does it through fetch_json, which
// retries what is safe to send again, and every address it is given goes through experiment::web()
// first (the test sets' stand-in web; tests/sim/tools_cli.py checks both). Definition in web.cpp.
#pragma once
#include <string>
#include <vector>

#include "agent/tools.h"

namespace tools {

// The JSON a GET (or, with a body, a POST) to `url` answered; null with `err` set when it failed.
json fetch_json(const std::string &url, std::string &err, Context &ctx, const std::vector<std::string> &headers = {},
                const std::string &post_body = "");

}  // namespace tools
