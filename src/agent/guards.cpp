#include "agent/guards.h"

#include <regex>

namespace guards {

// Appended after a reply that was drafted without tools (fast mode only). The second look reuses
// the whole prompt, so the server answers it from its prefix cache in a fraction of a normal
// call, and asked this pointedly the model catches what it missed in passing.
const char *const kSecondLook =
    "(Automatic check, not from the user.) CHECK BEFORE SPEAKING. The reply above is only a draft: the user has not heard it, and it was written without "
    "calling any tool. Call the right tool now, instead of replying, if the draft does any of these:\n"
    "- says or implies something was done, saved, set, added, moved, cancelled, played, stopped, switched or "
    "changed. Nothing has been; call the tool that does it.\n"
    "- states from memory something a tool provides: weather, a share price, an exchange rate or converted amount, "
    "the time in another place, opening hours or days, prices, news, results, anything recent or changeable.\n"
    "- gives the result of counting days or finding the weekday of a date (date_info), or of arithmetic that is more "
    "than one easy step (calculator).\n"
    "- is about something the user is looking at or holding, including saying you cannot see it (look).\n"
    "- says what is or is not in the user's calendar, reminders, timers or music library, or how long a timer has "
    "left, without having checked.\n"
    "If none of these apply, reply with the single word OK and the draft will be spoken as it is. That is the right "
    "outcome for conversation, settled general knowledge, translations, the current local time, facts you "
    "remember about the user, a question asking for a missing detail, or honestly saying you cannot do something "
    "none of your tools does.";

// Rides along with the newest request (fast mode only). The same rule is in the system prompt,
// but with reasoning off the model heeds what it read last.
const char *const kDecideNote =
    "(Automatic note, not from the user: if this asks you to do something, or needs live, local or changeable "
    "information, a date or a sum worked out, or the camera, call the tool now instead of replying. Reply in words "
    "only when no tool applies.)";

// True when a draft tells the user that something has been done. Said in a turn where no tool
// ran, that is a false claim, and it is cheaper to catch here than to pay for the model's
// reasoning on every call. Covers English and the commonest phrasings in a few other languages.
// True when a draft says something is not in the calendar or reminders. Only meaningful in a
// turn where nothing was looked up.
bool claims_not_saved(const std::string &raw) {
    std::string text = raw;
    for (size_t at; (at = text.find("\xE2\x80\x99")) != std::string::npos;) text.replace(at, 3, "'");
    static const std::regex re(
        "\\b(don'?t|do not|doesn'?t) (have|see|know|seem to have) [^.!?]{0,60}"
        "(birthday|anniversary|appointment|calendar|reminder|event|saved|on record|booked|scheduled)"
        "|\\b(no|nothing|isn'?t|not) [^.!?]{0,40}\\b(in|on) your (calendar|diary|reminders)",
        std::regex::icase | std::regex::optimize);
    return std::regex_search(text, re);
}

bool claims_action(const std::string &raw) {
    std::string text = raw;
    for (size_t at; (at = text.find("\xE2\x80\x99")) != std::string::npos;) text.replace(at, 3, "'");  // typographic apostrophe
    static const std::regex re(
        "\\b(i'?ve|i have|i just|i) (now |just |already )?(added|saved|set|scheduled|put|noted|cancell?ed|deleted|removed|"
        "moved|sent|booked|turned|changed|updated|switched|stopped|started|reserved|ordered|called|texted|messaged)\\b"
        "|\\b(is|it'?s|that'?s) (now |already )?(on|in|added to|saved (in|to)) your (calendar|diary|memory)"
        "|\\b(timer|reminder|alarm|event|message|text|volume|brightness) (is|has been|was) (now |already )?"
        "(set|saved|added|cancell?ed|sent|scheduled|deleted|turned|changed)"
        "|\\bnow playing\\b|\\bi'?ll (remember|keep that in mind|remind you)|\\bi will (remember|remind you)"
        "|^\\W*(done|all set|all done|got it|noted|saved|sorted|sent)\\b"
        "|^\\W*(listo|hecho|fertig|erledigt|fait|c'est fait|feito|pronto|fatto)\\b"
        "|\\b(he|ich habe|j'ai|ho) [^.!?]{0,40}(añadido|guardado|puesto|programado|gestellt|gespeichert|eingetragen|"
        "hinzugefügt|ajouté|enregistré|programmé|réglé|aggiunto|salvato|impostato)",
        std::regex::icase | std::regex::optimize);
    return std::regex_search(text, re);
}

// "Done" said after tool calls that all failed or wait for the user's yes. An offer put as a
// question is not that claim, and is often the right answer then: "Shall I put it in for next
// year?" after a date in the past, "Shall I set the heater to 28 degrees?" for a change that waits
// for a yes. "Done, anything else?" still claims.
//
// Something said to be on: "Here is Closed FM.", "Playing Jazz FM", "It's playing now", the radio
// and music tools' own confirmations, after the call that would have started it failed. Judged
// sentence by sentence, and not in one that says no or asks: "Nothing is playing right now" (the
// honest answer to "skip this song" with no music on), "Closed FM isn't playing", "Here's the
// problem", "The speaker is playing at 20 %; shall I set it to 90 %?" (a change that waits for a yes).
bool claims_done(const std::string &raw) {
    static const std::regex offer("\\b(shall|should|can|could|may|will|would|do) i\\b", std::regex::icase | std::regex::optimize);
    static const std::regex on_air(
        "^\\W*(here('s| is| comes| you go| we go)\\b(?![^:]*\\b(problem|issue|trouble|thing|what|why|how|error|sorry|list)\\b)"
        "|playing\\b)|\\b(is|are|i'?m|it'?s) (now |already )?playing\\b",
        std::regex::icase | std::regex::optimize);
    static const std::regex says_no("\\b(no|not|nothing|none|nobody|never|neither|nor|cannot|unable|failed|sorry)\\b|n't\\b",
                                    std::regex::icase | std::regex::optimize);
    std::string text = std::regex_replace(raw, offer, "$1");
    for (size_t at; (at = text.find("\xE2\x80\x99")) != std::string::npos;) text.replace(at, 3, "'");  // typographic apostrophe
    if (claims_action(text)) return true;
    for (size_t from = 0; from < text.size();) {
        const size_t end = text.find_first_of(".!?", from);
        const std::string sentence = text.substr(from, end == std::string::npos ? std::string::npos : end - from);
        const bool question = end != std::string::npos && text[end] == '?';
        if (!question && std::regex_search(sentence, on_air) && !std::regex_search(sentence, says_no)) return true;
        if (end == std::string::npos) break;
        from = end + 1;
    }
    return false;
}

}  // namespace guards
