// The user's yes: the conversation as far as it needs it, the questions waiting for an answer, and what
// counts as putting a question and as a plain yes. See consent.h.
#include "agent/consent.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include "core/util.h"

namespace consent {

namespace {

constexpr unsigned long kAnswerWithinMs = 3 * 60 * 1000;   // a yes must come within this
constexpr size_t kMaxPending = 8;

// An action that needed the user's yes. It goes ahead only when the identical action is asked for again in
// the very next user message, that message reads as a yes, and a reply that put the question went to the
// user in between.
struct Pending {
    Question q;
    unsigned long at_ms = 0;
    unsigned long turn = 0;      // the user message during which it was asked for
    bool asked = false;          // a reply that put the question went to the user after it
};

std::mutex g_mtx;                // guards everything in this group
unsigned long g_turn = 0;        // user messages so far
std::string g_last_said;         // the newest user message
// Text from outside (a web page, what a photo shows) the model has read. It stays in what the model sees
// until the conversation is cleared or grows past it, and could be steering the model for all that time,
// so every action that asks needs the user's yes until then: not only in the turn it was read in, and not
// only an action already put as a question.
bool g_untrusted = false;             // read since the conversation was last cleared ...
unsigned long g_untrusted_turn = 0;   // ... most recently during this user message
unsigned long g_context_from = 0;     // the oldest user message the model still sees
std::vector<Pending> g_pending;

// Whether text from outside is still in what the model sees. The caller holds g_mtx.
bool steered_by_outside() { return g_untrusted && g_untrusted_turn >= g_context_from; }

// A short answer that agrees and says nothing else: "yes", "yes please", "sure, go ahead", "yes, the bed
// heater". Every word has to belong to an agreement, to the politeness around one, or to what the question
// was about (`names`: the thing's name, the word for its kind, a group's words, the words of a memory); a
// message that also says anything else is a new request ("please set a timer", "ok, what time is it"), and
// one with a word of refusal in it ("leave it off please", "yes, no wait"), or of the opposite of what was
// asked ("off" to a question about switching on), is not a yes. An answer that is not recognised only means
// the question is asked again.
bool sounds_like_yes(const std::string &text, const std::set<std::string> &names, const std::string &action) {
    const std::vector<std::string> words = words_of(text);
    if (words.empty() || words.size() > 10) return false;
    static const std::set<std::string> kRefusal = {
        "no", "nope", "nah", "not", "don", "dont", "never", "cancel", "stop", "wait", "later", "leave", "keep", "hold", "maybe",
        "nein", "nicht", "non", "pas", "não", "nao", "nee", "niet", "nie", "ne", "nem", "nunca", "jamais", "nada"};
    const bool switching_off = action == "off";
    for (const auto &w : words)
        if (kRefusal.count(w) || w == (switching_off ? "on" : "off")) return false;
    // Words that agree, alone or together; "please" answers a question as a yes only when it is
    // all that was said.
    static const char *const kAgreeing[] = {"go ahead", "go on", "go for it", "do it", "do that", "please do", "of course",
                                            "that s right", "that s fine", "that s ok", "that s okay", "all right", "sounds good",
                                            "sure thing", "you bet", "uh huh", "mm hmm", "d accord", "bien sûr", "bien sur",
                                            "vas y", "por supuesto", "mach das", "mach es", "na klar", "va bene", "pode ser",
                                            "isso mesmo"};
    static const char *const kPlease[] = {"por favor", "s il vous plaît", "s il vous plait", "s il te plaît", "s il te plait"};
    static const std::set<std::string> kYes = {"yes", "yeah", "yea", "yep", "yup", "aye", "sure", "ok", "okay", "okey", "alright",
                                               "fine", "affirmative", "absolutely", "definitely", "certainly", "correct", "confirm",
                                               "confirmed", "mhm", "si", "sí", "oui", "ja", "jawohl", "genau", "klar", "sim",
                                               "tak", "da", "vale", "claro", "certo", "dale"};
    // Allowed around an agreement, never one by themselves ("right" also gives directions).
    static const std::set<std::string> kAround = {"thanks", "thank", "you", "then", "just", "oh", "um", "uh", "er", "erm", "ah",
                                                  "well", "now", "right", "mate", "bitte", "danke", "merci", "gracias", "obrigado",
                                                  "obrigada", "grazie", "the", "it", "them", "that", "one", "all", "of", "turn",
                                                  "switch", "set", "on", "off"};
    bool agreed = false, please = false, other = false;
    std::string joined = " ";
    for (const auto &w : words) joined += w + " ";
    auto take = [&](const char *phrase) {
        bool found = false;
        for (size_t at; (at = joined.find(" " + std::string(phrase) + " ")) != std::string::npos; found = true)
            joined.replace(at, strlen(phrase) + 2, " ");
        return found;
    };
    for (const char *phrase : kAgreeing) agreed = take(phrase) || agreed;
    for (const char *phrase : {"turn it", "switch it", "turn them", "switch them"})
        agreed = take((std::string(phrase) + (switching_off ? " off" : " on")).c_str()) || agreed;
    for (const char *phrase : kPlease) please = take(phrase) || please;
    for (const auto &w : util::split(joined, ' ')) {
        if (w.empty()) continue;
        if (kYes.count(w)) agreed = true;
        else if (w == "please") please = true;
        else if (kAround.count(w) || names.count(w)) other = true;
        else return false;   // it says something else as well
    }
    return agreed || (please && !other);
}

// Whether a reply put the question: it asks something, and it names what the question is about (see Naming).
bool puts_question(const std::string &reply, const Naming &naming) {
    if (reply.find_first_of("?") == std::string::npos && reply.find("？") == std::string::npos &&
        reply.find("؟") == std::string::npos)
        return false;
    const std::vector<std::string> words = words_of(reply);
    const std::set<std::string> said(words.begin(), words.end());
    return std::any_of(naming.ways.begin(), naming.ways.end(), [&](const std::vector<std::set<std::string>> &way) {
        return !way.empty() && std::all_of(way.begin(), way.end(), [&](const std::set<std::string> &choice) {
            return std::any_of(choice.begin(), choice.end(), [&](const std::string &w) { return said.count(w) > 0; });
        });
    });
}

// What the model reads when the action waits: why, and the question to put; or that the answer was no yes.
std::string confirm_text(const Question &q, bool not_agreed) {
    const std::string then = "\" Only if they answer yes, " + q.again + ".";
    if (not_agreed)
        return "CONFIRM: nothing has been " + q.undone + ", because the user's answer was not a clear yes. If they still want "
               "it, ask: \"" + q.question + then;
    return "CONFIRM: nothing has been " + q.undone + " yet, because " + q.reason + ". Ask the user: \"" + q.question + then;
}

}  // namespace

std::string ask_first(const Question &asked, unsigned long now_ms) {
    std::lock_guard<std::mutex> lk(g_mtx);
    Question q = asked;
    bool not_agreed = false;
    for (auto it = g_pending.begin(); it != g_pending.end(); ++it) {
        if (it->q.signature != q.signature) continue;
        // Within one turn the rules give the current answer (the user may have just switched the
        // question off in settings); a question from the turn before stays open whatever they say.
        if (q.reason.empty() && it->turn < g_turn) q.reason = it->q.reason;
        const bool answered = it->asked && g_turn == it->turn + 1;
        const bool ok = answered && sounds_like_yes(g_last_said, it->q.naming.words, it->q.action) &&
                        now_ms - it->at_ms <= kAnswerWithinMs;
        not_agreed = answered && !ok;
        g_pending.erase(it);
        if (ok) return "";
        break;
    }
    if (q.reason.empty()) return "";
    g_pending.push_back({q, now_ms, g_turn, false});
    if (g_pending.size() > kMaxPending) g_pending.erase(g_pending.begin());
    return confirm_text(q, not_agreed);
}

std::string outside_reason(const std::string &rule) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!steered_by_outside()) return "";
    return (g_untrusted_turn == g_turn ? "this request involved text from outside (a web page or a photo), and "
                                       : "this conversation includes text from outside (a web page or a photo), and ") +
           rule;
}

std::vector<std::string> words_of(const std::string &text) {
    std::vector<std::string> out;
    for (const auto &w : util::split(util::plain(text), ' '))
        if (!w.empty()) out.push_back(w);
    return out;
}

Naming read_back(const std::string &text, const std::set<std::string> &answer_words, const std::string &user_name) {
    // Words a reading back changes or leaves out: "User is allergic to shellfish" is read back as "you are
    // allergic to shellfish", "My sister's name is Anna" as "your sister's name is Anna".
    static const std::set<std::string> kChanged = {
        "the", "a", "an", "of", "and", "to", "in", "on", "at", "for", "with", "by", "that", "this", "is", "are", "am", "was",
        "were", "be", "been", "has", "have", "had", "do", "does", "did", "i", "me", "my", "mine", "you", "your", "yours", "he",
        "him", "his", "she", "her", "hers", "they", "them", "their", "we", "us", "our", "it", "its", "user", "users", "owner",
        "s", "m", "re", "ve", "ll", "d"};
    const std::vector<std::string> all = words_of(text), name = words_of(user_name);
    std::vector<std::string> says;
    for (const auto &w : all)
        if (!kChanged.count(w) && std::find(name.begin(), name.end(), w) == name.end()) says.push_back(w);
    if (says.empty()) says = all;
    Naming n;
    std::vector<std::set<std::string>> way;
    for (const auto &w : says) {
        std::set<std::string> forms = {w, w + "s"};
        if (w.size() > 3 && w.back() == 's') forms.insert(w.substr(0, w.size() - 1));
        if (w.size() > 4 && w.compare(w.size() - 2, 2, "es") == 0) forms.insert(w.substr(0, w.size() - 2));
        way.push_back(forms);
    }
    n.ways.push_back(way);
    n.words.insert(all.begin(), all.end());
    n.words.insert(answer_words.begin(), answer_words.end());
    return n;
}

std::string read_back_question(const std::string &lead, const std::string &text) {
    std::string said = text;
    while (!said.empty() && strchr(".!?;:", said.back())) said.pop_back();
    return lead + said + "?";
}

// ---------------------------------------------------------------- the conversation
void user_spoke(const std::string &text) {
    std::lock_guard<std::mutex> lk(g_mtx);
    ++g_turn;
    g_last_said = text;
    // A question is answered by the next message or not at all.
    g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(), [](const Pending &p) { return p.turn + 1 < g_turn; }),
                    g_pending.end());
}

void assistant_replied(const std::string &reply) {
    std::lock_guard<std::mutex> lk(g_mtx);
    // Only a reply that puts an action's question counts as asking it: "Done. Anything else?" or an empty
    // answer leaves it unasked, and asking about one action does not ask about another.
    for (auto &p : g_pending)
        if (p.turn == g_turn && puts_question(reply, p.q.naming)) p.asked = true;
}

void saw_untrusted_text() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_untrusted = true;
    g_untrusted_turn = g_turn;
}

void conversation_reset() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_untrusted = false;
    g_pending.clear();   // a yes in the new conversation answers nothing asked in the old one
}

void context_kept(size_t user_messages) {
    std::lock_guard<std::mutex> lk(g_mtx);
    // The messages kept are the newest ones: the oldest of them is this user message.
    if (user_messages <= g_turn) g_context_from = std::max(g_context_from, g_turn + 1 - user_messages);
}

namespace testing {
void reset() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_turn = 0;
    g_last_said.clear();
    g_untrusted = false;
    g_untrusted_turn = 0;
    g_context_from = 0;
    g_pending.clear();
}
}  // namespace testing

}  // namespace consent
