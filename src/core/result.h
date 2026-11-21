// The one shape every failure a tool or the smart-home layer reports to the model has:
// "ERROR: what was wrong. What to do" (AGENTS.md section 4, failed tool calls). Header-only.
#pragma once
#include <string>

namespace result {

// A failure, phrased as an instruction about what to do next: exactly "ERROR: " + msg.
inline std::string fail(const std::string &msg) { return "ERROR: " + msg; }

// Whether a tool's result is a failure: it starts with "ERROR" (with or without the ": "), or is a
// JSON object whose first key is "error".
inline bool failed(const std::string &text) {
    return text.compare(0, 5, "ERROR") == 0 || text.compare(0, 9, "{\"error\"") == 0;
}

// A failure's words without the "ERROR: " in front, to quote or show; any other text unchanged.
inline std::string words(const std::string &text) {
    return text.compare(0, 7, "ERROR: ") == 0 ? text.substr(7) : text;
}

}  // namespace result
