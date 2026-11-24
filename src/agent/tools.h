// The tools the agent can call. Each takes JSON arguments and returns text for the model. Each is one
// file in src/agent/tools/ (its function and its spec) and one line in tools/registry.cpp.
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

// "say", for a tool whose effect is fully known from the request (a timer set, a button pressed): the
// model supplies the confirmation up front, and when the call works that is spoken straight away
// instead of paying for a second model call just to say "done".
struct Say {
    bool taken = false;   // the tool takes "say" (its schema gets the parameter)
    std::string note;     // added to the parameter's description, e.g. "Omit for status."
    // Whether a call that worked is confirmed with its say (unset: every one). False where the result
    // holds what the model has still to read: the track that started, the battery level.
    std::function<bool(const json &args, const std::string &result)> when;
};

struct Tool {
    std::string name;
    std::string label;          // short present-tense text for the screen, e.g. "Checking the weather"
    std::string description;
    json parameters;            // JSON schema
    std::function<std::string(const json &args, Context &ctx)> run;
    Say say;
    // Decided for each request (unset: always offered, as described above). `offered`: whether the tool
    // is offered at all (web_search needs a search key). `spec`: a description and parameters made from
    // the user's data, {"description": ..., "parameters": ...}, or null for nothing to offer (smart_home,
    // radio); such a schema brings its own "say", and the tool checks its own arguments.
    std::function<bool()> offered;
    std::function<json()> spec;
};

const std::vector<Tool> &all();
json schema();                                   // OpenAI "tools" array
const Tool *find(const std::string &name);

// The sentence the model asked to have spoken once this call succeeded ("say"), or "" when
// the call failed, waits for the user's yes, or its result has to be read by the model first.
std::string confirmation(const std::string &name, const std::string &raw_args, const std::string &result);

// Whether a result reports a failure ("ERROR: ...", or a JSON object holding "error": result::failed).
// A "CONFIRM: ..." is not one: nothing was done, but only because the user's answer is awaited.
bool failed(const std::string &result);

// Runs a tool by name with the model's raw argument text. Never throws. The arguments are checked
// against the tool's schema first (types, with yes/no and capitals taken as meant), and every
// failure comes back in one shape, "ERROR: what was wrong. What to do", naming the argument and
// what was sent.
std::string run(const std::string &name, const std::string &raw_args, Context &ctx);

}  // namespace tools
