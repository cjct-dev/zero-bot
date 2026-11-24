// The one shape every failure a tool or the smart-home layer reports to the model has:
// "ERROR: what was wrong. What to do" (AGENTS.md section 4, failed tool calls), and the one shape of a
// result that waits for the user's yes, "CONFIRM: ..." (agent/consent.h). Header-only.
#pragma once
#include <string>

#include "nlohmann/json.hpp"

namespace result {

// A failure, phrased as an instruction about what to do next: exactly "ERROR: " + msg.
inline std::string fail(const std::string &msg) { return "ERROR: " + msg; }

// Whether a tool's result is a failure: it starts with "ERROR" (with or without the ": "), or it is a
// JSON object holding "error" as JSON reads the key (a letter of it escaped too), whatever its value
// ({"error": 5}, {"error": ""}). Text that begins {"error" counts too, JSON or not. Text that begins with
// "{" is parsed, once: `decoded`, when given, gets what that gave (a discarded value for text that is not
// JSON), so a caller can shape the failure without parsing it again. tools::run gives every failure a
// tool returns the "ERROR: ..." shape before passing it on.
inline bool failed(const std::string &text, nlohmann::json *decoded = nullptr) {
    if (text.compare(0, 5, "ERROR") == 0) return true;
    if (text.empty() || text[0] != '{') return false;
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    const bool failure = j.is_object() ? j.contains("error") : text.compare(0, 8, "{\"error\"") == 0;
    if (decoded) *decoded = std::move(j);
    return failure;
}

// Whether a result waits for the user's yes ("CONFIRM: ..."): nothing was done, and nothing failed; the
// model has a question to put to the user first.
inline bool awaits_yes(const std::string &text) { return text.compare(0, 7, "CONFIRM") == 0; }

// A failure's words without the "ERROR: " in front, to quote or show; any other text unchanged.
inline std::string words(const std::string &text) {
    return text.compare(0, 7, "ERROR: ") == 0 ? text.substr(7) : text;
}

}  // namespace result
