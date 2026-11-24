// The user's yes (AGENTS.md section 4). Some actions go ahead only at the user's own word: a smart-home
// change the safety rules hold back (home::execute), and, while text from outside (a web page, what a photo
// shows) is still in what the model sees, every smart-home change and every memory saved or forgotten
// (memory_save, memory_delete). Such an action is remembered as a question. It goes ahead only when the
// identical action is asked for again in the very next user message, that message reads as a short, plain
// yes, and a reply that put the question (it asks, and names what the question is about) went to the user in
// between. Nothing the model passes can stand in for this. The request loop reports the conversation here.
#pragma once
#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace consent {

// How a reply or an answer may refer to what a question is about. A reply names it when, for one of
// `ways`, it has a word from every set: {"heater"} for a "Fan heater" when no other thing's name has that
// word, {all, every, ...} and {light, lights, ...} for all the lights, every word of a memory that says
// something. An answer may use any of `words` besides agreeing ("yes, the heating", "yes, remember it").
struct Naming {
    std::vector<std::vector<std::set<std::string>>> ways;
    std::set<std::string> words;
    std::set<std::string> read;     // every word of what a Memory question reads back (never taken as its action's)
};

// The kinds of question, each with its own open questions, so that one kind never pushes out another's.
//   Home: a smart-home change. A reply puts it when it asks something and names the target anywhere in it.
//   Memory: a memory saved, replaced or forgotten (read_back; Question::action "save", "replace" or "forget"). A
//   reply puts it only in a sentence that ends in a question mark and has every word of the memory that says
//   something, a word for its action ("remember", "save", "note", "keep"; "replace", "change", "update", "swap";
//   "forget", "delete", "remove", "erase") and none for another, besides the memory's own words, so "You're
//   vegetarian, right?" does not ask to forget "Vegetarian"; and only when the memory has a word that no other
//   memory asked about in the same request has: "Shall I forget that your partner is vegetarian?" does not also
//   ask about forgetting "Vegetarian". Sentences end where the reply ends them, never where the memory does.
enum class Pool { Home, Memory };

// An action that may need the user's yes, and how its question is put.
struct Question {
    std::string signature;   // the action exactly: only the same signature asked for again is the same action
    std::string reason;      // why it needs a yes now; "" when it needs none (an open question about it still does)
    std::string question;    // what the user is asked, e.g. "Turn on Bed heater?"
    std::string action;      // "on" or "off" for a switch: the opposite word in an answer is no yes
    Naming naming;
    std::string undone;      // what has not happened: "changed" (nothing has been changed yet)
    std::string again;       // what the model does after a yes: "call smart_home again with the same request"
    Pool pool = Pool::Home;
};

// "" when the action may go ahead: it needs no yes, or it was put to the user as a question in the reply to
// their previous message, within three minutes of `now_ms`, and they have just said yes. Otherwise the action
// is remembered and the CONFIRM text for the model is returned. An action once put as a question keeps
// needing the yes in the next message even when what made it need one has passed, so "no" stops it.
std::string ask_first(const Question &q, unsigned long now_ms);

// Why an action needs a yes because text from outside is still in what the model sees, ending in `rule`
// ("devices change only at the user's own word"); "" when no such text is in view.
std::string outside_reason(const std::string &rule);

// The words of a text as questions and answers are compared: letters and digits, lower case (util::plain).
std::vector<std::string> words_of(const std::string &text);

// How a reply that reads `texts` back to the user names them (a Memory question): it has every word of them
// that says something (not "the", "my", "is", the user's own name or another word a reading back changes:
// "Sam's desk" is read back to Sam as "your desk"), as written or with an "s" or "es" more or less ("prefer"
// for "prefers"). An answer may use the texts' words and `answer_words`.
Naming read_back(const std::vector<std::string> &texts, const std::set<std::string> &answer_words, const std::string &user_name);
// A text as a question reads it back: without its closing punctuation ("Shall I remember this: " + it + "?").
std::string read_back_text(const std::string &text);

// ---- the conversation, as the request loop reports it
void user_spoke(const std::string &text);         // a user message: an open question is answered by this or not at all
void assistant_replied(const std::string &reply); // a reply that went to the user: it may put an open question
// A tool has just handed the model text from outside (a web page, a photo). From now on every action that
// asks outside_reason() needs the user's yes, for as long as the model can still see that text: until the
// conversation is cleared, or has grown so long that the message it came with is dropped.
void saw_untrusted_text();
// The conversation was cleared: nothing read in it is seen by the model any more, and no question asked in it
// is still open.
void conversation_reset();
// How many of the user's messages (the newest) the model is still shown, reported each time the loop
// shortens the conversation; text from outside read before the oldest of them is gone.
void context_kept(size_t user_messages);

namespace testing {
void reset();   // forgets the conversation: its messages, the text from outside, the open questions
}

}  // namespace consent
