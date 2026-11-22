#include "agent/agent.h"

#include <chrono>
#include <cstring>
#include <map>
#include <optional>
#include <unistd.h>

#include "agent/controls.h"
#include "agent/experiment.h"
#include "agent/guards.h"
#include "agent/llm.h"
#include "agent/prompt.h"
#include "agent/speech.h"
#include "agent/tools.h"
#include "core/activity.h"
#include "core/result.h"
#include "core/settings.h"
#include "platform/audio.h"
#include "smarthome/home.h"

namespace {
constexpr int kMaxRounds = 8;            // model calls per request, including tool round-trips
constexpr size_t kMaxHistory = 40;       // messages kept as context
constexpr size_t kMaxToolResult = 6000;  // characters of tool output passed back to the model
constexpr size_t kMaxEarlier = 200;      // DIAGNOSTICS (temporary): trimmed messages kept for a saved conversation

// Adds an instruction for this one model call. It is attached to the newest user message where
// possible, because some chat templates reject a system message after the first.
void add_note(json &messages, const std::string &note) {
    json &last = messages.back();
    if (util::jget(last, "role", std::string()) == "user" && last["content"].is_string())
        last["content"] = last["content"].get<std::string>() + "\n\n" + note;
    else
        messages.push_back({{"role", "system"}, {"content", note}});
}

// What a tool call asked for, to recognise the same call again: the tool and its arguments in a
// fixed order, without "say" (a reworded confirmation is still the same call).
std::string call_key(const std::string &name, const std::string &raw_args) {
    json a = util::parse_untrusted_json(util::trim(raw_args).empty() ? "{}" : raw_args);
    if (!a.is_object()) return name + "\n" + raw_args;
    a.erase("say");
    return name + "\n" + a.dump(-1, ' ', false, json::error_handler_t::replace);
}

// Said aloud when a spoken request comes to nothing (Agent::Turn::say_failed): the reason is on the screen.
constexpr const char *kFailedAloud = "Sorry, that did not work. The details are on the screen.";
constexpr const char *kNotCaught = "I didn't catch that.";   // shown, and said, for a recording with no words in it

// A failure's words without the "ERROR: " in front, short enough to quote.
std::string error_words(const std::string &text) { return util::truncate_utf8(result::words(text), 200, "..."); }

// The model calls a request may make (kMaxRounds), its tools' round trips included. A draft withdrawn
// because the user said more does not count, and a request typed while the answer to the last one was
// spoken starts afresh: one budget shared by chained requests once cut the last of them short.
class Budget {
public:
    bool left() const { return used_ < kMaxRounds; }
    bool last() const { return used_ == kMaxRounds - 1; }   // no tools are offered to the last call
    bool spent() const { return used_ >= kMaxRounds; }
    void spend() { ++used_; }
    void restart() { used_ = 0; }

private:
    int used_ = 0;
};

// ZB_NO_AUDIO (the command line sets it unless its caller did): nothing is spoken. "0" lets answers be
// spoken there too, for the tests, which put a stand-in player on PATH.
bool audio_off() {
    const char *v = getenv("ZB_NO_AUDIO");
    return v && strcmp(v, "0") != 0;
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
        history_gone_ += history_.size();
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

json Agent::history_snapshot(size_t *gone) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (gone) *gone = history_gone_;
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
    Input in;
    in.text = text;
    return run_blocking(std::move(in));
}

std::string Agent::run_blocking(std::vector<int16_t> pcm) {
    Input in;
    in.audio = true;
    in.pcm = std::move(pcm);
    return run_blocking(std::move(in));
}

std::string Agent::run_blocking(Input in) {
    g_blocking = true;
    {
        std::lock_guard<std::mutex> lk(mtx_);
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

// Moves queued input into the conversation. Returns how many user messages were added; `spoken` is
// set when one of them was said aloud, and `missed` to the sentence to say when a recording gave
// none (it could not be transcribed, or held no words); both are left alone otherwise.
int Agent::absorb_inputs(bool first, bool *spoken, std::string *missed) {
    // The recording is lost: the reason is shown, and the caller told what to say if nothing else comes of it.
    auto lost = [&](const std::string &shown, const char *aloud) {
        if (ev_.error) ev_.error(shown);
        if (missed) *missed = aloud;
    };
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
            if (!err.empty()) { lost("Could not transcribe: " + err, kFailedAloud); continue; }
            if (text.empty()) { lost(kNotCaught, kNotCaught); continue; }
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
        std::string content = stamp + (controls::on(controls::DateSpelling) ? util::spell_numeric_dates(text) : text);
        if (!context_.empty()) {
            if (time(nullptr) - context_at_ < 600) content += "\n\n(Automatic note, not from the user: " + context_ + ")";
            context_.clear();
        }
        history_.push_back({{"role", "user"}, {"content", content}});
        ++added;
        if (in.audio && spoken) *spoken = true;
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
            history_gone_ += start;
        }
        for (const auto &m : history_) user_messages += util::jget(m, "role", std::string()) == "user";
    }
    // Text from outside holds smart-home changes back only while the model can still see it.
    home::context_kept(user_messages);
}

// One call of run_turn: a request, with whatever the user added while it was worked on, and any
// request typed while its answer was being spoken.
class Agent::Turn {
public:
    explicit Turn(Agent &agent) : a_(agent) {}
    void run();

private:
    // What one request has done so far. Input that steers the request joins it; a request typed
    // while the answer to the last one was spoken starts afresh (reset()).
    struct Request {
        bool acted = false;             // a tool has run for the current request
        bool guarded = false;           // the unbacked-claim guard fires at most once per request
        bool any_ok = false;            // a tool call of this request did what it was asked
        int failures = 0;               // tool calls of this request that failed
        bool failure_guarded = false;   // a "done" after calls that all failed has been withdrawn once
        bool nudged = false;            // an empty reply has been sent back once
        std::string done_said;          // the confirmations ("say") of calls that worked, for an answer made here
        // Tool calls that failed in this request, by what they asked (call_key), with their error. The
        // same call is not run again until the user says something more: a change that may have
        // reached a device is never repeated by the model on its own, and a model that keeps trying one
        // thing is told so instead of spending the request on it.
        std::map<std::string, std::string> failed_calls;
        tools::Context tool_ctx;        // what the tools are given (tool_ctx.worked: the calls that worked)

        // A claim guard may still fire in this request: each kind fires at most once (guards::Claim).
        bool claim_open(const guards::Claim &c) const {
            if (!controls::on(controls::ClaimGuards)) return false;
            return c.scope == guards::Claim::NoTool ? !acted && !guarded : acted && !any_ok && !failure_guarded;
        }
        void reset() {
            acted = guarded = any_ok = failure_guarded = nudged = false;
            failures = 0;
            failed_calls.clear();
            done_said.clear();
            tool_ctx.worked.clear();
        }
    };

    // One model call: what it is sent, how its reply streams in, and what came back.
    struct Round {
        json messages;                     // what the call is sent
        bool corrected = false;            // it carries a correction note
        bool deciding = false;             // nothing has run since the user last spoke: the reply decides what to do
        bool hold = false;                 // a reply without a tool call is held back for the second look
        llm::ChatOptions opt;
        llm::ChatResult res;
        bool began = false;                // the answer has begun on screen
        bool speech_started = false;       // a sentence of it has been handed to speech
        bool steered = false;              // input arrived before anything was said: the draft is withdrawn
        const guards::Claim *unbacked = nullptr;   // the claim guard a sentence tripped as it streamed
        std::vector<std::string> unsaid;   // sentences composed while the microphone was open
        SpeechQueue::Chunker chunker;
    };

    // How a round ends: what the turn does next.
    enum class Then {
        Next,         // go round again: the model is called once more for this request
        Again,        // the draft was withdrawn because the user said more: go round again, not counted
        NewRequest,   // something typed while the answer was spoken is the next request: a fresh budget
        Finish,       // the request is answered, cancelled, or the service failed
        Leave,        // the user talked over the answer: what they say next starts a new turn
    };

    // A round's steps, in the order one_round takes them.
    Then one_round(const Budget &budget);
    void messages_for(Round &r, const Budget &budget);
    void call_model(Round &r, const Budget &budget);
    void stream(Round &r, const std::string &piece);   // the call's on_text
    bool withdraw_if_steered(Round &r);
    std::optional<Then> cut_short(Round &r);
    bool on_error(Round &r);
    std::optional<Then> second_look(Round &r);
    std::optional<Then> judge_draft(Round &r, const Budget &budget);
    void release_held(Round &r);
    Then run_tools(Round &r);
    void not_made(const llm::ToolCall &c);
    Then finish_answer(Round &r);

    Then next_request();
    int take_request(bool first, bool *spoken);
    void new_request();
    void set_spoken(bool spoken);
    bool heard() const { return speak_ && spoken_; }   // the request's answer is spoken, not only shown
    void answering();
    void say_failed(const std::string &sentence);
    int steer_in();
    void wait_while_listening();
    void to_speech(const std::string &sentence);
    void deliver(const std::string &text);

    Agent &a_;
    experiment::RequestTrace rt_;   // the experiments' trace (ZB_EXP_TRACE); empty in the app users run
    json tool_schema_;              // the tools offered, as the request began
    SpeechQueue &speech_ = SpeechQueue::get();
    bool speak_ = false;            // answers may be spoken (spoken replies are on)
    bool spoken_ = false;           // the request has input the user said aloud, so its answer is spoken too (ZB-091)
    Request req_;
    std::string correction_;        // one-off note for the next model call; never stored in the history
};

void Agent::run_turn() { Turn(*this).run(); }

// The turn: input absorbed, then rounds until the request is answered or the budget is spent. A
// request typed while an answer was spoken is taken in the same turn, with a fresh budget.
void Agent::Turn::run() {
    a_.cancel_ = false;
    a_.interrupted_ = false;
    speak_ = Settings::get().speak_replies && !audio_off();
    rt_.absorbing();
    bool spoken = false;
    if (take_request(true, &spoken) == 0) return;
    rt_.begin();
    a_.trim_history();
    activity::request();   // DIAGNOSTICS (temporary): the activity log

    tool_schema_ = tools::schema();
    req_.tool_ctx.cancel = &a_.cancel_;
    req_.tool_ctx.on_image = a_.ev_.image;
    req_.tool_ctx.on_breathe = a_.ev_.breathe;
    set_spoken(spoken);

    Budget budget;
    while (budget.left() && !a_.cancel_) {
        const Then then = one_round(budget);
        if (then == Then::Leave) return;   // the user talked over the answer: what they say next starts a new turn
        if (then == Then::Finish) break;
        if (then == Then::NewRequest) {
            new_request();
            budget.restart();
        } else if (then == Then::Next) {
            budget.spend();
        }   // Then::Again: a withdrawn draft does not count against the budget
    }
    // Leaving by running out of rounds means the last one asked to go round again, so nothing has
    // been said for this request. Say so rather than falling silent.
    if (budget.spent() && !a_.cancel_) deliver(req_.done_said + "Sorry, that took more steps than I can do in one go. Please ask again.");
    speech_.end_of_answer();
}

// One model call, and what comes of it: a draft withdrawn, a correction, tools run, an answer.
Agent::Turn::Then Agent::Turn::one_round(const Budget &budget) {
    a_.set_state(AgentState::Thinking);
    a_.round_cancel_ = false;
    Round r;
    messages_for(r, budget);
    call_model(r, budget);
    if (withdraw_if_steered(r)) return Then::Again;   // a withdrawn draft does not count against the budget
    if (const auto then = cut_short(r)) return *then;
    if (on_error(r)) return Then::Finish;
    if (const auto then = second_look(r)) return *then;
    if (const auto then = judge_draft(r, budget)) return *then;
    release_held(r);
    return r.res.tool_calls.empty() ? finish_answer(r) : run_tools(r);
}

// What the model is sent this round: the system prompt, the conversation, and the notes for this
// call alone (the last round's, a correction). Built for every call, never kept in the history.
void Agent::Turn::messages_for(Round &r, const Budget &budget) {
    r.messages = json::array({{{"role", "system"}, {"content", prompt::system_prompt()}}});
    {
        std::lock_guard<std::mutex> lk(a_.mtx_);
        for (auto &m : a_.history_) r.messages.push_back(m);
    }
    if (budget.last()) add_note(r.messages, guards::kLastRound);

    r.corrected = !correction_.empty();
    if (r.corrected) add_note(r.messages, correction_);
    correction_.clear();
}

// The round's model call, streamed through stream(). Decides first whether a reply without a tool
// call is to be held back for the second look (hold), and adds fast mode's note asking for a tool.
void Agent::Turn::call_model(Round &r, const Budget &budget) {
    r.opt.model = Settings::get().eff_llm_model();
    r.opt.fast = Settings::get().fast_mode;
    {
        std::lock_guard<std::mutex> lk(a_.mtx_);
        r.deciding = a_.history_.empty() || util::jget(a_.history_.back(), "role", std::string()) != "tool";
    }
    // In fast mode the model's commonest mistake is to answer in words when it should have
    // used a tool: "done" with nothing done, or a price or opening time from memory. So a reply
    // that arrives with no tool call is held back and checked before a word of it is spoken.
    const bool fast_decision = r.deciding && !req_.acted && !req_.guarded && r.opt.fast;
    r.hold = fast_decision && controls::on(controls::SecondLook);
    if (fast_decision && controls::on(controls::DecideNote)) add_note(r.messages, guards::kDecideNote);
    r.opt.purpose = budget.last() ? "last_round" : r.corrected ? "correction" : r.deciding ? "decide" : "after_tool";
    r.opt.cancel = &a_.round_cancel_;
    r.opt.on_text = [this, &r](const std::string &piece) { stream(r, piece); };
    r.res = llm::chat(r.messages, budget.last() ? json::array() : tool_schema_, r.opt);
}

// The reply as it streams in: shown, and spoken sentence by sentence, unless it is held back (fast
// mode's second look), withdrawn (the user said more, or it claims what no tool did) or kept back
// while the microphone is open.
void Agent::Turn::stream(Round &r, const std::string &piece) {
    if (r.steered || r.unbacked) return;
    if (r.hold) return;   // held whole, checked once complete
    if (!r.began) { r.began = true; if (a_.ev_.answer_begin) a_.ev_.answer_begin(); }
    if (a_.ev_.answer_delta) a_.ev_.answer_delta(piece);
    for (auto &sentence : r.chunker.feed(piece)) {
        // New input arrived before anything was said: withdraw this draft and let the
        // model reconsider with the extra instruction.
        if (!r.speech_started && a_.has_pending()) { r.steered = true; a_.round_cancel_ = true; return; }
        // Checked before the first words are spoken: once said, a claim cannot be taken back.
        // "Done" is false when no tool has run, and when every one that ran failed or is
        // waiting for the user's yes.
        if (!r.speech_started)
            for (const guards::Claim &c : guards::kClaims)
                if (c.as_it_streams && req_.claim_open(c) && c.test(sentence)) {
                    r.unbacked = &c;
                    a_.round_cancel_ = true;
                    return;
                }
        // The user has started talking: keep the words back until they have finished.
        if (a_.listening() || !r.unsaid.empty()) { r.unsaid.push_back(sentence); continue; }
        r.speech_started = true;
        answering();
        to_speech(sentence);
    }
}

// What the user said while the reply was on its way takes its place: the draft is withdrawn, what
// they said joins the request, and the model is asked again. True when that happened.
bool Agent::Turn::withdraw_if_steered(Round &r) {
    // The user was talking when this step was decided. Nothing has been said or done yet, so
    // what they add is taken into account first: a correction should land before the action.
    const bool talked_over = !r.speech_started && a_.listening();
    if (!r.speech_started) wait_while_listening();

    if (r.steered || (!r.speech_started && !r.res.cancelled && r.res.error.empty() && a_.has_pending() &&
                    (r.res.tool_calls.empty() || talked_over))) {
        if (r.began && a_.ev_.answer_discard) a_.ev_.answer_discard();
        steer_in();
        return true;
    }
    return false;
}

// The user cancelled the request (Finish), or talked over its answer (Leave); nothing otherwise.
std::optional<Agent::Turn::Then> Agent::Turn::cut_short(Round &r) {
    if (a_.cancel_) {
        if (r.began && a_.ev_.answer_end) a_.ev_.answer_end(r.res.content, true);
        return Then::Finish;
    }
    if (a_.interrupted_) {
        // The user talked over the answer. Keep what was said so the model knows where it stopped.
        if (!r.res.content.empty()) {
            std::lock_guard<std::mutex> lk(a_.mtx_);
            a_.history_.push_back({{"role", "assistant"}, {"content", r.res.content + " [the user interrupted here]"}});
        }
        if (r.began && a_.ev_.answer_end) a_.ev_.answer_end(r.res.content, true);
        a_.last_answer_ = r.res.content;
        return Then::Leave;  // what they say next starts a new turn
    }
    return std::nullopt;
}

// The service failed: the error is shown, and once tools have run, what is known is said; before that, a
// spoken request with nothing of its answer heard yet is told that it failed. True when the call failed.
bool Agent::Turn::on_error(Round &r) {
    if (r.res.error.empty()) return false;
    if (r.began && a_.ev_.answer_end) a_.ev_.answer_end(r.res.content, true);
    if (a_.ev_.error) a_.ev_.error(r.res.error);
    // Tools have run, so something may have changed, or failed, that the user has not been
    // told about. Say what is known rather than leave them with silence.
    if (req_.acted) {
        const char *lost = "I lost the connection to the AI service before I could finish, so please ask again.";
        deliver(r.speech_started ? lost
                : req_.done_said + (req_.failures ? "Something I tried did not work, and I lost the connection to the AI "
                                                    "service before I could say what. Please ask again."
                                                  : lost));
    } else if (heard() && !r.speech_started) {
        say_failed(kFailedAloud);
    }
    return true;
}

// Fast mode's second look at a draft held back because it called no tool: the model, shown its own
// draft, either lets it stand or calls the tool it should have. The user saying more meanwhile
// withdraws the draft (Again); cancelling ends the request (Finish).
std::optional<Agent::Turn::Then> Agent::Turn::second_look(Round &r) {
    if (r.hold && r.res.tool_calls.empty() && !r.res.content.empty()) {
        json review = r.messages;
        review.push_back({{"role", "assistant"}, {"content", r.res.content}});
        review.push_back({{"role", "user"}, {"content", guards::kSecondLook}});
        llm::ChatOptions check;
        check.model = r.opt.model;
        check.fast = true;
        check.cancel = &a_.cancel_;
        check.purpose = "second_look";
        llm::ChatResult second = llm::chat(review, tool_schema_, check);
        if (experiment::tracing()) {
            std::string outcome = second.error.empty() ? "" : "error";
            for (const auto &c : second.tool_calls) outcome += (outcome.empty() ? "" : ",") + c.name;
            rt_.second_look(outcome.empty() ? "ok" : outcome);
        }
        wait_while_listening();
        if (a_.cancel_) return Then::Finish;
        if (a_.has_pending()) {  // the user added something while the draft was being checked
            steer_in();
            return Then::Again;
        }
        // A failed check lets the draft through: a network hiccup must not silence the assistant.
        if (second.error.empty() && !second.tool_calls.empty()) {
            r.res.tool_calls = std::move(second.tool_calls);
            r.res.content.clear();
        }
    }
    return std::nullopt;
}

// The reply, judged before what is left of it is spoken: a claim no tool call backs is withdrawn and
// the model told (Next), and an empty one goes back once, then is answered here.
std::optional<Agent::Turn::Then> Agent::Turn::judge_draft(Round &r, const Budget &budget) {
    // A draft that claims what no tool call backs is withdrawn, and the model told why (guards::kClaims).
    const guards::Claim *claim = r.unbacked;
    if (!claim && !r.speech_started && r.res.tool_calls.empty())
        for (const guards::Claim &c : guards::kClaims)
            if (req_.claim_open(c) && c.test(r.res.content)) { claim = &c; break; }
    if (claim) {
        (claim->scope == guards::Claim::NothingWorked ? req_.failure_guarded : req_.guarded) = true;
        rt_.guard(claim->name);
        if (r.began && a_.ev_.answer_discard) a_.ev_.answer_discard();
        correction_ = claim->note(r.res.content);
        return Then::Next;
    }
    // An empty reply leaves the user with silence: it goes back to the model once, and if the
    // next one is empty too, an answer is made here from what is known.
    if (r.res.tool_calls.empty() && util::trim(r.res.content).empty() && !r.speech_started && r.unsaid.empty()) {
        if (r.began && a_.ev_.answer_discard) a_.ev_.answer_discard();
        rt_.guard(req_.nudged || budget.last() ? "empty_answered_here" : "empty");
        if (!req_.nudged && !budget.last()) {
            req_.nudged = true;
            correction_ = guards::empty_reply_note(req_.failures);
            return Then::Next;
        }
        deliver(req_.done_said + (req_.failures ? "Sorry, something I tried did not work." : "Sorry, I have no answer to that."));
        return next_request();
    }
    return std::nullopt;
}

// What was held back is spoken now that it stands: a draft kept for the second look, and words
// composed while the microphone was open.
void Agent::Turn::release_held(Round &r) {
    if (r.hold && r.res.tool_calls.empty()) {
        // The draft stands: release it as if it had streamed.
        r.began = true;
        if (a_.ev_.answer_begin) a_.ev_.answer_begin();
        if (a_.ev_.answer_delta) a_.ev_.answer_delta(r.res.content);
        answering();
        for (auto &sentence : r.chunker.feed(r.res.content)) to_speech(sentence);
    }
    // Words held back while the user was talking, who then cancelled rather than adding anything.
    if (!r.unsaid.empty() && r.res.tool_calls.empty()) {
        answering();
        for (auto &sentence : r.unsaid) to_speech(sentence);
    }
}

// The round's tool calls, run in order and their results put in the conversation. A call that failed
// in this request is not made again unchanged, none is made while the user is talking, and once they
// have said more the rest are not made (not_made). When every call worked and came with its own
// confirmation ("say"), that is spoken without another model call; otherwise the model is called again.
Agent::Turn::Then Agent::Turn::run_tools(Round &r) {
    json calls = json::array();
    for (auto &c : r.res.tool_calls)
        calls.push_back({{"id", c.id}, {"type", "function"}, {"function", {{"name", c.name}, {"arguments", c.args}}}});
    {
        std::lock_guard<std::mutex> lk(a_.mtx_);
        a_.history_.push_back({{"role", "assistant"}, {"content", r.res.content}, {"tool_calls", calls}});
    }
    if (r.began) {
        const std::string rest = r.chunker.flush();
        if (!rest.empty() && !a_.listening() && r.unsaid.empty()) to_speech(rest);
        if (a_.ev_.answer_end) a_.ev_.answer_end(r.res.content, false);
    }
    std::string confirm;
    bool all_confirmed = true;
    bool said_more = false;   // the user said more while the calls were made: the rest are not made
    for (size_t i = 0; i < r.res.tool_calls.size(); ++i) {
        const llm::ToolCall &c = r.res.tool_calls[i];
        // No action runs while the microphone is open, or what the user said is on its way (AGENTS.md
        // section 4): each call waits for them to finish, and once they have said more, it and the rest are
        // not made. The first call is held only by the gate: input that came while the model was deciding,
        // the microphone closed, was already weighed (withdraw_if_steered).
        if (!said_more && !a_.cancel_ && (i > 0 || a_.listening())) {
            wait_while_listening();
            said_more = a_.has_pending();
        }
        if (said_more && !a_.cancel_) {
            all_confirmed = false;
            not_made(c);
            continue;
        }
        const tools::Tool *tool = tools::find(c.name);
        const std::string label = tool ? tool->label : c.name;
        a_.set_state(AgentState::Working, label);
        if (a_.ev_.tool) a_.ev_.tool(label);
        const auto tool_started = std::chrono::steady_clock::now();
        const std::string key = call_key(c.name, c.args);
        const auto earlier = req_.failed_calls.find(key);
        std::string result;
        if (a_.cancel_) result = "ERROR: cancelled by the user";
        else if (earlier != req_.failed_calls.end())
            result = "ERROR: this exact call already failed in this request, so it was not made again. It said: " +
                     earlier->second + ". Change what that asks you to change, or tell the user it did not work";
        else result = tools::run(c.name, c.args, req_.tool_ctx);
        const long tool_ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tool_started).count());
        activity::tool(c.name, c.args, result, tool_ms);   // DIAGNOSTICS (temporary): the activity log
        rt_.tool(c.name, tool_ms, result);
#if ZB_EXPERIMENTS
        if (a_.ev_.tool_done) a_.ev_.tool_done(c.id, c.name, c.args, result);
#endif
        if (result.size() > kMaxToolResult) result = util::truncate_utf8(result, kMaxToolResult, " …[truncated]");
        if (tools::failed(result)) {
            ++req_.failures;
            // Refused only for what its "say" claimed, nothing done: it may come again
            // with another say, or after the step it lacked.
            if (!req_.tool_ctx.say_refused) req_.failed_calls.emplace(key, error_words(result));
        } else if (result.compare(0, 7, "CONFIRM") != 0) {
            req_.any_ok = true;
        }
        const std::string say = a_.cancel_ ? "" : tools::confirmation(c.name, c.args, result);
        if (say.empty()) all_confirmed = false;
        else if (confirm.find(say) == std::string::npos) confirm += (confirm.empty() ? "" : " ") + say;
        if (!say.empty() && req_.done_said.find(say) == std::string::npos) req_.done_said += say + " ";
        std::lock_guard<std::mutex> lk(a_.mtx_);
        a_.history_.push_back({{"role", "tool"}, {"tool_call_id", c.id}, {"content", result}});
    }
    req_.acted = true;
    wait_while_listening();
    // Anything said meanwhile steers the next step, so the model has to look again.
    if (steer_in() > 0 || a_.cancel_ || !all_confirmed) return Then::Next;

    // Every call succeeded and came with its own confirmation: speak that now rather than
    // spending another model call on it.
    deliver(confirm);
    return next_request();
}

// A call of the batch not made because the user said more first. It still gets its result, so the model decides
// again with their words; not made is not failed (nothing was tried), and nothing is shown as running.
void Agent::Turn::not_made(const llm::ToolCall &c) {
    const std::string result = "ERROR: this call was not made, because the user said more first. Read what they "
                               "added, and make it again only if it is still wanted";
    activity::tool(c.name, c.args, result, 0);   // DIAGNOSTICS (temporary): the activity log
#if ZB_EXPERIMENTS
    if (a_.ev_.tool_done) a_.ev_.tool_done(c.id, c.name, c.args, result);
#endif
    std::lock_guard<std::mutex> lk(a_.mtx_);
    a_.history_.push_back({{"role", "tool"}, {"tool_call_id", c.id}, {"content", result}});
}

// The model's final answer for this request: the rest of it spoken, kept in the conversation, and
// reported to the smart-home layer, which may be waiting on a question it puts.
Agent::Turn::Then Agent::Turn::finish_answer(Round &r) {
    const std::string rest = r.chunker.flush();
    if (!util::trim(rest).empty()) to_speech(rest);
    {
        std::lock_guard<std::mutex> lk(a_.mtx_);
        a_.history_.push_back({{"role", "assistant"}, {"content", r.res.content}});
    }
    home::assistant_replied(r.res.content);   // it may put the question a smart-home change waits on
    a_.last_answer_ = r.res.content;
    if (r.began && a_.ev_.answer_end) a_.ev_.answer_end(r.res.content, false);
    if (r.res.content.empty() && a_.ev_.error) a_.ev_.error("The model returned an empty answer.");
    return next_request();
}

// Something typed or said while the answer was given becomes the next request (NewRequest: a fresh
// budget), spoken or not by what it was; with nothing new the turn is over (Finish).
Agent::Turn::Then Agent::Turn::next_request() {
    // This request's answer is complete. Its speech plays out and lets go of the speaker and the music now,
    // not when the turn ends: the next request, typed while it was spoken, may take a while.
    speech_.end_of_answer();
    bool spoken = false;
    if (take_request(false, &spoken) == 0) return Then::Finish;
    set_spoken(spoken);
    return Then::NewRequest;
}

// Input for a request of its own (the turn's first, or one typed or said while the last answer was given): how many
// user messages it added. A recording that gave no words, and so leaves nothing to answer, is told so aloud. (Steering
// is taken by steer_in: the request it joins goes on, and its answer is still to come.)
int Agent::Turn::take_request(bool first, bool *spoken) {
    std::string missed;
    const int added = a_.absorb_inputs(first, spoken, &missed);
    if (added == 0 && !missed.empty()) say_failed(missed);
    return added;
}

// A request typed while the last answer was spoken: it starts afresh.
void Agent::Turn::new_request() {
    rt_.begin();
    req_.reset();
}

// Whether the request's answer is spoken: a typed request is answered on the screen only, and one with
// anything said aloud is spoken too (with spoken replies on). Music waits for an answer that is heard, never
// for one that is only shown: music a tool starts, and music already playing, which the question held
// (App::finish_recording) until this was known. That comes back now, unless an earlier answer is still being
// spoken (the speech queue lets go when it ends), rather than when the turn ends.
void Agent::Turn::set_spoken(bool spoken) {
    spoken_ = spoken;
    req_.tool_ctx.in_conversation = heard();
    if (heard()) audio::Music::get().hold(audio::Music::kExchange, true);
    else release_music();
}

// The answer is being given. One that is heard is Speaking: talking over it stops it. One that is only shown
// (a typed request's, or any with spoken replies off: ZB-091) is Answering, and nothing about it acts as if it
// could be heard: talking over it neither stops it nor marks it as interrupted.
void Agent::Turn::answering() { a_.set_state(heard() ? AgentState::Speaking : AgentState::Answering); }

// A spoken request that came to nothing (the service failed before a word of its answer was heard, or the recording
// gave no words) is not left in silence for a user who is not looking: one short sentence is said, and the reason
// stays on the screen. It is the app's notice, not an answer, so the conversation does not keep it and the model is
// told nothing new. Not while the microphone is open (what the user says then is the next request) nor after a cancel.
// It is only queued, never waited for: speech uses the same service and may fail too, and then the speech queue adds
// its own note below the reason, which stays.
void Agent::Turn::say_failed(const std::string &sentence) {
    if (speak_ && !a_.listening() && !a_.cancel_) a_.say(sentence);
}

// Input that arrived meanwhile joins this request. It may be "try that again", so the calls
// that failed may be made once more. Said aloud, it makes the rest of the answer spoken.
int Agent::Turn::steer_in() {
    bool spoken = false;
    const int added = a_.absorb_inputs(false, &spoken);
    if (added > 0) { req_.failed_calls.clear(); rt_.steered(); }
    if (spoken) set_spoken(true);
    return added;
}

// While the microphone is open, what the user is saying will steer this request, so a reply
// that is ready must not be spoken over them. Returns once they have finished (or cancelled).
void Agent::Turn::wait_while_listening() {
    while (a_.listening() && !a_.cancel_) usleep(50 * 1000);
}

// Every sentence handed to speech goes through here (traced whether or not it is spoken): spoken when
// replies are and the request was spoken.
void Agent::Turn::to_speech(const std::string &sentence) {
    rt_.sentence(sentence);
    if (heard()) speech_.enqueue(sentence);
}

// An answer made here rather than by the model (a confirmation, an apology): shown, spoken as the
// model's would be, kept in the conversation and reported to the smart-home layer like the model's own.
// A confirmation is the model's "say", and may put the question a smart-home change waits on.
void Agent::Turn::deliver(const std::string &text) {
    {
        std::lock_guard<std::mutex> lk(a_.mtx_);
        a_.history_.push_back({{"role", "assistant"}, {"content", text}});
    }
    home::assistant_replied(text);
    a_.last_answer_ = text;
    if (a_.ev_.answer_begin) a_.ev_.answer_begin();
    if (a_.ev_.answer_delta) a_.ev_.answer_delta(text);
    answering();
    to_speech(text);
    if (a_.ev_.answer_end) a_.ev_.answer_end(text, false);
}
