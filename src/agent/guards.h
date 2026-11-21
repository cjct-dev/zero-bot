// What the request loop (agent.cpp) checks a reply for before it is spoken, and the notes it sends
// the model with it (AGENTS.md section 4): the second look, the note asking for a tool, the claim
// guards.
#pragma once
#include <string>

namespace guards {

extern const char *const kSecondLook;   // the second look's question, after a draft written without tools
extern const char *const kDecideNote;   // rides along with the newest request in fast mode

bool claims_not_saved(const std::string &raw);
bool claims_action(const std::string &raw);
bool claims_done(const std::string &raw);

}  // namespace guards
