// What every tool file includes. A tool is one file here, src/agent/tools/<name>.cpp, holding its function
// and its spec (`Tool make_<name>()`: name, label, description, schema), and one line in registry.cpp.
// The runner (run.cpp) checks each argument's type against the schema before the tool runs, so a
// tool keeps only its own rules (ranges, dates, which names exist). Definitions in common.cpp.
#pragma once
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "agent/tools.h"

namespace tools {

using util::jget;

// ---- the environment
bool dry_run();   // ZB_DRY_RUN: nothing is played, sent or switched

// ---- arguments (the model's JSON, already checked against the schema's types)
std::string str_arg(const json &a, const char *key);    // text, trimmed; a number as its digits; "" when absent
bool given(const json &a, const char *key);             // present and not null
bool num_arg(const json &a, const char *key, double &out);   // a finite number, or text that starts with one
int clamp_int(double v, int lo, int hi);                // a finite value in [lo, hi], as an int
// An id from an earlier result; `where` says how to find it ("use calendar_list to find it").
bool id_arg(const json &a, const char *key, int &id, std::string &err, const std::string &where);
bool bool_arg(const json &a, const char *key, bool fallback);
std::string clean_text(const std::string &raw);         // control characters as single spaces, trimmed
// The error for a date or time that is malformed or not real, with what was sent.
std::string bad_time(const std::string &field, const std::string &sent, bool date_only = false);

// ---- results
std::string fail(const std::string &msg);   // "ERROR: " + msg, phrased as what to do next

// ---- the schema: an object of named properties, and a property of each type
json obj(std::initializer_list<std::pair<const std::string, json>> props, std::vector<std::string> required = {});
json S(const std::string &desc);
json N(const std::string &desc);
json B(const std::string &desc);
json E(const std::string &desc, std::vector<std::string> values);

// The tool takes "say" (tools.h): `note` is added to the parameter's description, and `when`, if given,
// picks the calls that worked which are confirmed with it. The parameter itself is added by the registry.
inline Say takes_say(std::string note = "", std::function<bool(const json &args, const std::string &result)> when = nullptr) {
    return {true, std::move(note), std::move(when)};
}

}  // namespace tools
