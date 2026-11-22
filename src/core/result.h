// The one shape every failure a tool or the smart-home layer reports to the model has:
// "ERROR: what was wrong. What to do" (AGENTS.md section 4, failed tool calls). Header-only.
#pragma once
#include <string>

#include "nlohmann/json.hpp"

namespace result {

// A failure, phrased as an instruction about what to do next: exactly "ERROR: " + msg.
inline std::string fail(const std::string &msg) { return "ERROR: " + msg; }

// Whether a tool's result is a failure: it starts with "ERROR" (with or without the ": "), or it is a
// JSON object holding "error", whatever its value (tools::run turns one with a text error into
// "ERROR: ..." first, so this catches the rest: {"error": 5}, {"error": ""}). Text that begins
// {"error" counts too, JSON or not. Only text that begins with "{" and names "error" is parsed.
inline bool failed(const std::string &text) {
    if (text.compare(0, 5, "ERROR") == 0 || text.compare(0, 8, "{\"error\"") == 0) return true;
    if (text.empty() || text[0] != '{' || text.find("\"error\"") == std::string::npos) return false;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    return j.is_object() && j.contains("error");
}

// A failure's words without the "ERROR: " in front, to quote or show; any other text unchanged.
inline std::string words(const std::string &text) {
    return text.compare(0, 7, "ERROR: ") == 0 ? text.substr(7) : text;
}

}  // namespace result
