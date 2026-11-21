// What the request loop (agent.cpp) checks a reply for before it is spoken, and the notes it sends
// the model with a call (AGENTS.md section 4): the second look, the note asking for a tool, the
// claim guards and the notes that say what went wrong. Every note is for one model call only and
// never kept in the conversation.
#pragma once
#include <string>

namespace guards {

extern const char *const kSecondLook;   // the second look's question, after a draft written without tools
extern const char *const kDecideNote;   // rides along with the newest request in fast mode
extern const char *const kLastRound;    // with the last call a request may make, which is offered no tools

// A claim guard: a draft that says what no tool call backs is withdrawn before it is spoken, once
// per request, and the model is told why (note()). The loop tries them in the order of kClaims.
struct Claim {
    enum Scope {
        NoTool,          // no tool has run for the request
        NothingWorked,   // tools ran, and every call failed or waits for the user's yes
    };
    const char *name;                         // as the experiments' trace records it
    Scope scope;
    bool as_it_streams;                       // also tried on each sentence as the draft streams in
    bool (*test)(const std::string &text);
    const char *why;                          // what the note says after quoting the draft
    std::string note(const std::string &draft) const;
};
extern const Claim kClaims[3];

// The note after an empty reply: the user has heard nothing.
std::string empty_reply_note(bool failures);

// True when a draft tells the user that something has been done. Said in a turn where no tool
// ran, that is a false claim, and it is cheaper to catch here than to pay for the model's
// reasoning on every call. Covers English and the commonest phrasings in a few other languages.
bool claims_action(const std::string &raw);
// True when a draft says something is not in the calendar or reminders. Only meaningful in a
// turn where nothing was looked up.
bool claims_not_saved(const std::string &raw);
// "Done", or something said to be playing, after tool calls that all failed or wait for the
// user's yes (guards.cpp has the cases).
bool claims_done(const std::string &raw);

}  // namespace guards
