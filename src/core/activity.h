// A private log of what the assistant did: each tool call with its arguments and result, so that a
// problem the owner reports ("it said it could not turn the TV on") can be diagnosed afterwards.
// It lives in ~/.cache/zero-bot/activity.log (mode 0600), at most about 256 kB plus one older file.
// What the user said is not written, nor anything secret: tool arguments and results never carry
// passwords or tokens (the smart-home results are names and states only).
#pragma once
#include <string>

namespace activity {

// A line marking the start of a request.
void request();
// One tool call: its name, arguments (JSON), result and how long it took.
void tool(const std::string &name, const std::string &args, const std::string &result, long ms);
// Where the log is.
std::string path();

}  // namespace activity
