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
    bool in_conversation = false;   // called from a live request, whose spoken reply is still to come
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

// Runs a tool by name with the model's raw argument text. Never throws.
std::string run(const std::string &name, const std::string &raw_args, Context &ctx);

}  // namespace tools
