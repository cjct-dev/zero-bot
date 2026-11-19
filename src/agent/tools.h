// The tools the agent can call. Each takes JSON arguments and returns text for the model.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "core/util.h"

namespace tools {

struct Context {
    std::atomic<bool> *cancel = nullptr;
    std::function<void(const std::string &jpeg_path)> on_image;   // a photo was taken
    std::function<void(const std::string &pattern, int minutes)> on_breathe;   // start a breathing exercise on screen
    bool in_conversation = false;   // called from a live request, whose spoken reply is still to come
    // The calls that worked so far in this request, by name ("timer:start" for a tool with an
    // action), added by run(); the agent empties it when a new request begins. A confirmation is
    // judged against what the request has done (a cancel that says "at 10 instead" after the new
    // reminder was set).
    std::vector<std::string> worked;
    // Set by a tool that refused only for what its "say" claims, before doing anything: the same
    // call may be made again at once with another say, or after the step it lacked.
    bool say_refused = false;
};

struct Tool {
    std::string name;
    std::string label;          // short present-tense text for the screen, e.g. "Checking the weather"
    std::string description;
    json parameters;            // JSON schema
    std::function<std::string(const json &args, Context &ctx)> run;
};

const std::vector<Tool> &all();
json schema();                                   // OpenAI "tools" array
const Tool *find(const std::string &name);

// The sentence the model asked to have spoken once this call succeeded ("say"), or "" when
// the call failed or its result has to be read by the model first.
std::string confirmation(const std::string &name, const std::string &raw_args, const std::string &result);

// Whether a result reports a failure ("ERROR: ..."). A "CONFIRM: ..." is not one: nothing was
// done, but only because the user's answer is awaited.
bool failed(const std::string &result);

// Runs a tool by name with the model's raw argument text. Never throws. The arguments are checked
// against the tool's schema first (types, with yes/no and capitals taken as meant), and every
// failure comes back in one shape, "ERROR: what was wrong. What to do", naming the argument and
// what was sent.
std::string run(const std::string &name, const std::string &raw_args, Context &ctx);

}  // namespace tools
