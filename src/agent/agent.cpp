#include "agent/agent.h"

#include <chrono>
#include <map>
#include <regex>
#include <unistd.h>

#include "agent/llm.h"
#include "agent/prompt.h"
#include "agent/speech.h"
#include "agent/tools.h"
#include "core/activity.h"
#include "core/settings.h"
#include "platform/audio.h"
#include "smarthome/home.h"

namespace {
constexpr int kMaxRounds = 8;            // model calls per request, including tool round-trips
constexpr size_t kMaxHistory = 40;       // messages kept as context
constexpr size_t kMaxToolResult = 6000;  // characters of tool output passed back to the model
constexpr size_t kMaxEarlier = 200;      // DIAGNOSTICS (temporary): trimmed messages kept for a saved conversation

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

// Adds an instruction for this one model call. It is attached to the newest user message where
// possible, because some chat templates reject a system message after the first.
void add_note(json &messages, const std::string &note) {
    json &last = messages.back();
    if (util::jget(last, "role", std::string()) == "user" && last["content"].is_string())
        last["content"] = last["content"].get<std::string>() + "\n\n" + note;
    else
        messages.push_back({{"role", "system"}, {"content", note}});
}

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
bool claims_done(const std::string &raw) {
    static const std::regex offer("\\b(shall|should|can|could|may|will|would|do) i\\b", std::regex::icase | std::regex::optimize);
    return claims_action(std::regex_replace(raw, offer, "$1"));
}

// What a tool call asked for, to recognise the same call again: the tool and its arguments in a
// fixed order, without "say" (a reworded confirmation is still the same call).
std::string call_key(const std::string &name, const std::string &raw_args) {
    json a = json::parse(util::trim(raw_args).empty() ? "{}" : raw_args, nullptr, false);
    if (!a.is_object()) return name + "\n" + raw_args;
    a.erase("say");
    return name + "\n" + a.dump(-1, ' ', false, json::error_handler_t::replace);
}

// A failure's words without the "ERROR: " in front, short enough to quote.
std::string error_words(const std::string &result) {
    std::string e = result.compare(0, 7, "ERROR: ") == 0 ? result.substr(7) : result;
    return util::truncate_utf8(e, 200, "...");
}

// Set while run_blocking() owns the conversation, so input submitted during it (steering)
// is absorbed by that run and does not start a second one on the worker thread.
std::atomic<bool> g_blocking{false};

}  // namespace

Agent &Agent::get() {
    static Agent a;
    return a;
}

void Agent::start(AgentEvents events) {
    ev_ = std::move(events);
    // An answer that is shown but cannot be heard says why, once, beside it.
    SpeechQueue::get().set_on_failure([this](const std::string &reason) {
        if (ev_.error) ev_.error("Could not speak the answer: " + reason);
    });
    thread_ = std::thread(&Agent::worker, this);
    thread_.detach();
}

void Agent::submit_text(const std::string &text) {
    const std::string t = util::trim(text);
    if (t.empty()) return;
    std::lock_guard<std::mutex> lk(mtx_);
    Input in;
    in.text = t;
    inputs_.push_back(std::move(in));
    cv_.notify_one();
}

void Agent::submit_audio(std::vector<int16_t> pcm) {
    std::lock_guard<std::mutex> lk(mtx_);
    Input in;
    in.audio = true;
    in.pcm = std::move(pcm);
    inputs_.push_back(std::move(in));
    cv_.notify_one();
}

void Agent::interrupt_speech() {
    SpeechQueue::get().stop();
    // Only the spoken answer is abandoned; a run that is still gathering information carries on
    // and will pick up what the user says next as steering.
    if (busy_ && state() == AgentState::Speaking) { interrupted_ = true; round_cancel_ = true; }
}

void Agent::cancel() {
    cancel_ = true;
    round_cancel_ = true;
    SpeechQueue::get().stop();
    std::lock_guard<std::mutex> lk(mtx_);
    inputs_.clear();
}

void Agent::reset_conversation() {
    cancel();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        history_ = json::array();
        earlier_ = json::array();   // DIAGNOSTICS (temporary)
        earlier_dropped_ = 0;
    }
    home::conversation_reset();   // a web page read in it no longer holds smart-home changes back
}

void Agent::say(const std::string &text) {
    SpeechQueue::get().enqueue(text);
    SpeechQueue::get().end_of_answer();
}

void Agent::add_context(const std::string &note) {
    std::lock_guard<std::mutex> lk(mtx_);
    const time_t now = time(nullptr);
    if (now - context_at_ >= 600) context_.clear();
    context_ += (context_.empty() ? "" : " ") + note;
    context_at_ = now;
}

void Agent::set_state(AgentState s, const std::string &detail) {
    std::lock_guard<std::mutex> lk(mtx_);
    state_ = s;
    detail_ = detail;
}

AgentState Agent::state() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (busy_ && state_ != AgentState::Idle) return state_;
    }
    return SpeechQueue::get().active() ? AgentState::Speaking : AgentState::Idle;
}

std::string Agent::detail() {
    std::lock_guard<std::mutex> lk(mtx_);
    return detail_;
}

size_t Agent::history_size() {
    std::lock_guard<std::mutex> lk(mtx_);
    return history_.size();
}

json Agent::history_snapshot() {
    std::lock_guard<std::mutex> lk(mtx_);
    return history_;
}

// DIAGNOSTICS (temporary, ZB-051): see agent.h.
json Agent::whole_conversation(size_t *dropped) {
    std::lock_guard<std::mutex> lk(mtx_);
    json all = earlier_;
    for (const json &m : history_) all.push_back(m);
    if (dropped) *dropped = earlier_dropped_;
    return all;
}

bool Agent::has_pending() {
    std::lock_guard<std::mutex> lk(mtx_);
    return !inputs_.empty();
}

// Music held back for the length of a request plays once the reply has been heard. With a reply
// still being spoken the speech queue lets go when it finishes; with none, this does.
static void release_music() {
    if (!SpeechQueue::get().active()) audio::Music::get().hold(audio::Music::kExchange, false);
}

void Agent::worker() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [&] { return !inputs_.empty() && !g_blocking; });
        }
        busy_ = true;
        run_turn();
        set_state(AgentState::Idle);
        release_music();
        busy_ = false;
    }
}

std::string Agent::run_blocking(const std::string &text) {
    g_blocking = true;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        Input in;
        in.text = text;
        inputs_.push_back(std::move(in));
    }
    busy_ = true;
    last_answer_.clear();
    run_turn();
    set_state(AgentState::Idle);
    release_music();
    busy_ = false;
    g_blocking = false;
    return last_answer_;
}

// Moves queued input into the conversation. Returns how many user messages were added.
int Agent::absorb_inputs(bool first) {
    int added = 0;
    for (;;) {
        Input in;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (inputs_.empty()) break;
            in = std::move(inputs_.front());
            inputs_.pop_front();
        }
        std::string text = in.text;
        if (in.audio) {
            set_state(AgentState::Transcribing);
            std::string err;
            text = llm::transcribe(audio::wav_from_pcm(in.pcm, audio::kMicRate), err, &cancel_);
            if (cancel_) break;
            if (!err.empty()) { if (ev_.error) ev_.error("Could not transcribe: " + err); continue; }
            if (text.empty()) { if (ev_.error) ev_.error("I didn't catch that."); continue; }
        }
        const bool steering = !first || added > 0;
        if (ev_.user_message) ev_.user_message(text, steering);
        // A smart-home change that waits for the user's yes may go ahead only on the very next
        // thing they say (see home::execute); it is told what that was.
        home::user_spoke(text);
        // The time travels with each message rather than in the system prompt, so the prompt and
        // the earlier conversation stay byte-for-byte the same from call to call and the server can
        // reuse its cached prefix. A prompt that changes every minute costs a second or two per call.
        const std::string stamp = "[" + util::format_time(time(nullptr), "%a %d %b %Y, %H:%M") + "] ";
        std::lock_guard<std::mutex> lk(mtx_);
        // Numeric dates are spelt out here, by rule, so that the day/month order and the year are
        // never left to the model's reading.
        std::string content = stamp + util::spell_numeric_dates(text);
        if (!context_.empty()) {
            if (time(nullptr) - context_at_ < 600) content += "\n\n(Automatic note, not from the user: " + context_ + ")";
            context_.clear();
        }
        history_.push_back({{"role", "user"}, {"content", content}});
        ++added;
    }
    return added;
}

void Agent::trim_history() {
    size_t user_messages = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (history_.size() > kMaxHistory) {
            size_t start = history_.size() - kMaxHistory;
            // Context must begin with a user message, never in the middle of a tool exchange.
            while (start < history_.size() && util::jget(history_[start], "role", std::string()) != "user") ++start;
            // DIAGNOSTICS (temporary, ZB-051): what leaves the context stays available to a saved conversation.
            for (size_t i = 0; i < start; ++i) earlier_.push_back(activity::compact(history_[i]));
            if (earlier_.size() > kMaxEarlier) {
                const size_t over = earlier_.size() - kMaxEarlier;
                earlier_.erase(earlier_.begin(), earlier_.begin() + static_cast<long>(over));
                earlier_dropped_ += over;
            }
            json kept = json::array();
            for (size_t i = start; i < history_.size(); ++i) kept.push_back(history_[i]);
            history_ = std::move(kept);
        }
        for (const auto &m : history_) user_messages += util::jget(m, "role", std::string()) == "user";
    }
    // Text from outside holds smart-home changes back only while the model can still see it.
    home::context_kept(user_messages);
}

void Agent::run_turn() {
    cancel_ = false;
    interrupted_ = false;
    if (absorb_inputs(true) == 0) return;
    trim_history();
    activity::request();   // DIAGNOSTICS (temporary): the activity log

    const json tool_schema = tools::schema();
    tools::Context tool_ctx;
    tool_ctx.in_conversation = true;
    tool_ctx.cancel = &cancel_;
    tool_ctx.on_image = ev_.image;
    tool_ctx.on_breathe = ev_.breathe;
    SpeechQueue &speech = SpeechQueue::get();
    const bool speak = Settings::get().speak_replies && !getenv("ZB_NO_AUDIO");

    bool acted = false;       // a tool has run for the current request
    bool guarded = false;     // the unbacked-claim guard fires at most once per request
    std::string correction;   // one-off note for the next model call; never stored in the history
    // Tool calls that failed in this request, by what they asked (call_key), with their error. The
    // same call is not run again until the user says something more: a change that may have
    // reached a device is never repeated by the model on its own, and a model that keeps trying one
    // thing is told so instead of spending the request on it.
    std::map<std::string, std::string> failed_calls;
    bool any_ok = false;            // a tool call of this request did what it was asked
    int failures = 0;               // tool calls of this request that failed
    bool failure_guarded = false;   // a "done" after calls that all failed has been withdrawn once
    bool nudged = false;            // an empty reply has been sent back once
    std::string done_said;          // the confirmations ("say") of calls that worked, for an answer made here
    auto new_request = [&] {
        acted = guarded = any_ok = failure_guarded = nudged = false;
        failures = 0;
        failed_calls.clear();
        done_said.clear();
    };
    // Input that arrived meanwhile joins this request. It may be "try that again", so the calls
    // that failed may be made once more.
    auto steer_in = [&] {
        const int added = absorb_inputs(false);
        if (added > 0) failed_calls.clear();
        return added;
    };

    // While the microphone is open, what the user is saying will steer this request, so a reply
    // that is ready must not be spoken over them. Returns once they have finished (or cancelled).
    auto wait_while_listening = [&] {
        while (listening_ && !cancel_) usleep(50 * 1000);
    };
    // An answer made here rather than by the model (a confirmation, an apology): shown, spoken and
    // kept in the conversation like the model's own.
    auto deliver = [&](const std::string &text) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            history_.push_back({{"role", "assistant"}, {"content", text}});
        }
        last_answer_ = text;
        if (ev_.answer_begin) ev_.answer_begin();
        if (ev_.answer_delta) ev_.answer_delta(text);
        set_state(AgentState::Speaking);
        if (speak) speech.enqueue(text);
        if (ev_.answer_end) ev_.answer_end(text, false);
    };

    int round = 0;
    for (; round < kMaxRounds && !cancel_; ++round) {
        set_state(AgentState::Thinking);
        round_cancel_ = false;

        json messages = json::array({{{"role", "system"}, {"content", prompt::system_prompt()}}});
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto &m : history_) messages.push_back(m);
        }
        if (round == kMaxRounds - 1) add_note(messages, "You have used many tool calls. Answer now with what you have.");

        const bool corrected = !correction.empty();
        if (corrected) add_note(messages, correction);
        correction.clear();

        bool began = false, speech_started = false, steered = false, unbacked = false;
        std::vector<std::string> unsaid;   // sentences composed while the microphone was open
        SpeechQueue::Chunker chunker;
        llm::ChatOptions opt;
        opt.model = Settings::get().eff_llm_model();
        opt.fast = Settings::get().fast_mode;
        bool deciding;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            deciding = history_.empty() || util::jget(history_.back(), "role", std::string()) != "tool";
        }
        // In fast mode the model's commonest mistake is to answer in words when it should have
        // used a tool: "done" with nothing done, or a price or opening time from memory. So a reply
        // that arrives with no tool call is held back and checked before a word of it is spoken.
        const bool hold = deciding && !acted && !guarded && opt.fast;
        if (hold) add_note(messages, kDecideNote);
        opt.cancel = &round_cancel_;
        opt.on_text = [&](const std::string &piece) {
            if (steered || unbacked || hold) return;
            if (!began) { began = true; if (ev_.answer_begin) ev_.answer_begin(); }
            if (ev_.answer_delta) ev_.answer_delta(piece);
            for (auto &sentence : chunker.feed(piece)) {
                // New input arrived before anything was said: withdraw this draft and let the
                // model reconsider with the extra instruction.
                if (!speech_started && has_pending()) { steered = true; round_cancel_ = true; return; }
                // Checked before the first words are spoken: once said, a claim cannot be taken back.
                // "Done" is false when no tool has run, and when every one that ran failed or is
                // waiting for the user's yes.
                if (!speech_started && ((!acted && !guarded && claims_action(sentence)) ||
                                        (acted && !any_ok && !failure_guarded && claims_done(sentence)))) {
                    unbacked = true;
                    round_cancel_ = true;
                    return;
                }
                // The user has started talking: keep the words back until they have finished.
                if (listening_ || !unsaid.empty()) { unsaid.push_back(sentence); continue; }
                speech_started = true;
                set_state(AgentState::Speaking);
                if (speak) speech.enqueue(sentence);
            }
        };
        llm::ChatResult res = llm::chat(messages, round == kMaxRounds - 1 ? json::array() : tool_schema, opt);
        // The user was talking when this step was decided. Nothing has been said or done yet, so
        // what they add is taken into account first: a correction should land before the action.
        const bool talked_over = !speech_started && listening_;
        if (!speech_started) wait_while_listening();

        if (steered || (!speech_started && !res.cancelled && res.error.empty() && has_pending() &&
                        (res.tool_calls.empty() || talked_over))) {
            if (began && ev_.answer_discard) ev_.answer_discard();
            steer_in();
            --round;  // a withdrawn draft does not count against the budget
            continue;
        }
        if (cancel_) {
            if (began && ev_.answer_end) ev_.answer_end(res.content, true);
            break;
        }
        if (interrupted_) {
            // The user talked over the answer. Keep what was said so the model knows where it stopped.
            if (!res.content.empty()) {
                std::lock_guard<std::mutex> lk(mtx_);
                history_.push_back({{"role", "assistant"}, {"content", res.content + " [the user interrupted here]"}});
            }
            if (began && ev_.answer_end) ev_.answer_end(res.content, true);
            last_answer_ = res.content;
            return;  // what they say next starts a new turn
        }
        if (!res.error.empty()) {
            if (began && ev_.answer_end) ev_.answer_end(res.content, true);
            if (ev_.error) ev_.error(res.error);
            // Tools have run, so something may have changed, or failed, that the user has not been
            // told about. Say what is known rather than leave them with silence.
            if (acted) {
                const char *lost = "I lost the connection to the AI service before I could finish, so please ask again.";
                deliver(speech_started ? lost
                        : done_said + (failures ? "Something I tried did not work, and I lost the connection to the AI "
                                                  "service before I could say what. Please ask again."
                                                : lost));
            }
            break;
        }

        if (hold && res.tool_calls.empty() && !res.content.empty()) {
            json review = messages;
            review.push_back({{"role", "assistant"}, {"content", res.content}});
            review.push_back({{"role", "user"}, {"content", kSecondLook}});
            llm::ChatOptions check;
            check.model = opt.model;
            check.fast = true;
            check.cancel = &cancel_;
            llm::ChatResult second = llm::chat(review, tool_schema, check);
            wait_while_listening();
            if (cancel_) break;
            if (has_pending()) {  // the user added something while the draft was being checked
                steer_in();
                --round;
                continue;
            }
            // A failed check lets the draft through: a network hiccup must not silence the assistant.
            if (second.error.empty() && !second.tool_calls.empty()) {
                res.tool_calls = std::move(second.tool_calls);
                res.content.clear();
            }
        }

        // "Done" after tool calls that all failed or wait for the user's yes: nothing was done, and
        // the user must hear what went wrong, or the question, instead.
        if (acted && (unbacked || (!any_ok && !failure_guarded && !speech_started && res.tool_calls.empty() && claims_done(res.content)))) {
            failure_guarded = true;
            if (began && ev_.answer_discard) ev_.answer_discard();
            correction = "(Automatic note, not from the user.) Your draft reply began \"" + util::truncate_utf8(res.content, 160) +
                         "\" but nothing has been done: every tool call for this request failed or is waiting for the "
                         "user's answer (see the ERROR and CONFIRM results above), and the user has not heard that reply. "
                         "Tell them plainly what did not work, or put to them the question a CONFIRM result gives.";
            continue;
        }
        // "Done, it's in your calendar" with no tool call behind it: withdraw the draft and make
        // the model either do the thing or admit it cannot.
        if (unbacked || (!acted && !guarded && !speech_started && res.tool_calls.empty() && claims_action(res.content))) {
            guarded = true;
            if (began && ev_.answer_discard) ev_.answer_discard();
            correction = "(Automatic note, not from the user.) Your draft reply began \"" + util::truncate_utf8(res.content, 160) +
                         "\" but no tool has been called for this request, so nothing has actually been done and the "
                         "user has not heard that reply. If they asked for something a tool does, call that tool now. "
                         "If no tool can do it, tell them plainly that you cannot. If they were only chatting or "
                         "asking a question, answer without claiming to have done anything.";
            continue;
        }
        // "I don't have Dad's birthday saved", said without looking: the calendar may well have it.
        if (!unbacked && !acted && !guarded && !speech_started && res.tool_calls.empty() && claims_not_saved(res.content)) {
            guarded = true;
            if (began && ev_.answer_discard) ev_.answer_discard();
            correction = "(Automatic note, not from the user.) Your draft reply began \"" + util::truncate_utf8(res.content, 160) +
                         "\" but you have not looked: no tool has been called for this request, and the user has not "
                         "heard that reply. Call calendar_list with search (or reminder_list) now and answer from "
                         "what it returns.";
            continue;
        }
        // An empty reply leaves the user with silence: it goes back to the model once, and if the
        // next one is empty too, an answer is made here from what is known.
        if (res.tool_calls.empty() && util::trim(res.content).empty() && !speech_started && unsaid.empty()) {
            if (began && ev_.answer_discard) ev_.answer_discard();
            if (!nudged && round < kMaxRounds - 1) {
                nudged = true;
                correction = std::string("(Automatic note, not from the user.) Your reply was empty, so the user has heard "
                                         "nothing. Answer them now in words") +
                             (failures ? ": a tool call failed, so say plainly what did not work." : ".");
                continue;
            }
            deliver(done_said + (failures ? "Sorry, something I tried did not work." : "Sorry, I have no answer to that."));
            if (absorb_inputs(false) > 0) { new_request(); round = -1; continue; }   // a new request: fresh budget
            break;
        }
        if (hold && res.tool_calls.empty()) {
            // The draft stands: release it as if it had streamed.
            began = true;
            if (ev_.answer_begin) ev_.answer_begin();
            if (ev_.answer_delta) ev_.answer_delta(res.content);
            set_state(AgentState::Speaking);
            for (auto &sentence : chunker.feed(res.content))
                if (speak) speech.enqueue(sentence);
        }
        // Words held back while the user was talking, who then cancelled rather than adding anything.
        if (!unsaid.empty() && res.tool_calls.empty()) {
            set_state(AgentState::Speaking);
            for (auto &sentence : unsaid)
                if (speak) speech.enqueue(sentence);
        }

        if (!res.tool_calls.empty()) {
            json calls = json::array();
            for (auto &c : res.tool_calls)
                calls.push_back({{"id", c.id}, {"type", "function"}, {"function", {{"name", c.name}, {"arguments", c.args}}}});
            {
                std::lock_guard<std::mutex> lk(mtx_);
                history_.push_back({{"role", "assistant"}, {"content", res.content}, {"tool_calls", calls}});
            }
            if (began) {
                const std::string rest = chunker.flush();
                if (speak && !rest.empty() && !listening_ && unsaid.empty()) speech.enqueue(rest);
                if (ev_.answer_end) ev_.answer_end(res.content, false);
            }
            std::string confirm;
            bool all_confirmed = true;
            for (auto &c : res.tool_calls) {
                const tools::Tool *tool = tools::find(c.name);
                const std::string label = tool ? tool->label : c.name;
                set_state(AgentState::Working, label);
                if (ev_.tool) ev_.tool(label);
                const auto tool_started = std::chrono::steady_clock::now();
                const std::string key = call_key(c.name, c.args);
                const auto earlier = failed_calls.find(key);
                std::string result;
                if (cancel_) result = "ERROR: cancelled by the user";
                else if (earlier != failed_calls.end())
                    result = "ERROR: this exact call already failed in this request, so it was not made again. It said: " +
                             earlier->second + ". Change what that asks you to change, or tell the user it did not work";
                else result = tools::run(c.name, c.args, tool_ctx);
                // DIAGNOSTICS (temporary): the activity log
                activity::tool(c.name, c.args, result,
                               static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                     std::chrono::steady_clock::now() - tool_started).count()));
                if (result.size() > kMaxToolResult) result = util::truncate_utf8(result, kMaxToolResult, " …[truncated]");
                if (tools::failed(result)) {
                    ++failures;
                    failed_calls.emplace(key, error_words(result));
                } else if (result.compare(0, 7, "CONFIRM") != 0) {
                    any_ok = true;
                }
                const std::string say = cancel_ ? "" : tools::confirmation(c.name, c.args, result);
                if (say.empty()) all_confirmed = false;
                else if (confirm.find(say) == std::string::npos) confirm += (confirm.empty() ? "" : " ") + say;
                if (!say.empty() && done_said.find(say) == std::string::npos) done_said += say + " ";
                std::lock_guard<std::mutex> lk(mtx_);
                history_.push_back({{"role", "tool"}, {"tool_call_id", c.id}, {"content", result}});
            }
            acted = true;
            wait_while_listening();
            // Anything said meanwhile steers the next step, so the model has to look again.
            if (steer_in() > 0 || cancel_ || !all_confirmed) continue;

            // Every call succeeded and came with its own confirmation: speak that now rather than
            // spending another model call on it.
            deliver(confirm);
            if (absorb_inputs(false) > 0) { new_request(); round = -1; continue; }   // a new request: fresh budget
            break;
        }

        // Final answer for this request.
        const std::string rest = chunker.flush();
        if (speak && !util::trim(rest).empty()) speech.enqueue(rest);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            history_.push_back({{"role", "assistant"}, {"content", res.content}});
        }
        home::assistant_replied(res.content);   // it may put the question a smart-home change waits on
        last_answer_ = res.content;
        if (began && ev_.answer_end) ev_.answer_end(res.content, false);
        if (res.content.empty() && ev_.error) ev_.error("The model returned an empty answer.");
        // Something typed while the answer was being spoken becomes the next request.
        if (absorb_inputs(false) > 0) { new_request(); round = -1; continue; }   // a new request: fresh budget
        break;
    }
    // Leaving by running out of rounds means the last one asked to go round again, so nothing has
    // been said for this request. Say so rather than falling silent.
    if (round >= kMaxRounds && !cancel_) deliver(done_said + "Sorry, that took more steps than I can do in one go. Please ask again.");
    speech.end_of_answer();
}
