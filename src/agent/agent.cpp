#include "agent/agent.h"

#include <chrono>
#include <map>
#include <unistd.h>

#include "agent/experiment.h"
#include "agent/guards.h"
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
constexpr int kCheckWaitMs = 1500;       // experiments only (E3): how long a finished draft waits for the parallel check

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

    void new_request();
    int steer_in();
    void wait_while_listening();
    void to_speech(const std::string &sentence);
    void deliver(const std::string &text);

    Agent &a_;
    experiment::RequestTrace rt_;   // the experiments' trace (ZB_EXP_TRACE); empty in the app users run
    json tool_schema_;              // the tools offered, as the request began
    SpeechQueue &speech_ = SpeechQueue::get();
    bool speak_ = false;            // answers are spoken, not only shown
    Request req_;
    std::string correction_;        // one-off note for the next model call; never stored in the history
};

void Agent::run_turn() { Turn(*this).run(); }

// A request typed while the last answer was spoken: it starts afresh.
void Agent::Turn::new_request() {
    rt_.begin();
    req_.reset();
}

// Input that arrived meanwhile joins this request. It may be "try that again", so the calls
// that failed may be made once more.
int Agent::Turn::steer_in() {
    const int added = a_.absorb_inputs(false);
    if (added > 0) { req_.failed_calls.clear(); rt_.steered(); }
    return added;
}

// While the microphone is open, what the user is saying will steer this request, so a reply
// that is ready must not be spoken over them. Returns once they have finished (or cancelled).
void Agent::Turn::wait_while_listening() {
    while (a_.listening_ && !a_.cancel_) usleep(50 * 1000);
}

// Every sentence handed to speech goes through here (traced whether or not it is spoken).
void Agent::Turn::to_speech(const std::string &sentence) {
    rt_.sentence(sentence);
    if (speak_) speech_.enqueue(sentence);
}

// An answer made here rather than by the model (a confirmation, an apology): shown, spoken and
// kept in the conversation like the model's own.
void Agent::Turn::deliver(const std::string &text) {
    {
        std::lock_guard<std::mutex> lk(a_.mtx_);
        a_.history_.push_back({{"role", "assistant"}, {"content", text}});
    }
    a_.last_answer_ = text;
    if (a_.ev_.answer_begin) a_.ev_.answer_begin();
    if (a_.ev_.answer_delta) a_.ev_.answer_delta(text);
    a_.set_state(AgentState::Speaking);
    to_speech(text);
    if (a_.ev_.answer_end) a_.ev_.answer_end(text, false);
}

void Agent::Turn::run() {
    a_.cancel_ = false;
    a_.interrupted_ = false;
    rt_.absorbing();
    if (a_.absorb_inputs(true) == 0) return;
    rt_.begin();
    a_.trim_history();
    activity::request();   // DIAGNOSTICS (temporary): the activity log

    tool_schema_ = tools::schema();
    tools::Context &tool_ctx = req_.tool_ctx;
    tool_ctx.in_conversation = true;
    tool_ctx.cancel = &a_.cancel_;
    tool_ctx.on_image = a_.ev_.image;
    tool_ctx.on_breathe = a_.ev_.breathe;
    speak_ = Settings::get().speak_replies && !getenv("ZB_NO_AUDIO");

    int round = 0;
    for (; round < kMaxRounds && !a_.cancel_; ++round) {
        a_.set_state(AgentState::Thinking);
        a_.round_cancel_ = false;

        json messages = json::array({{{"role", "system"}, {"content", prompt::system_prompt()}}});
        {
            std::lock_guard<std::mutex> lk(a_.mtx_);
            for (auto &m : a_.history_) messages.push_back(m);
        }
        if (round == kMaxRounds - 1) add_note(messages, guards::kLastRound);

        const bool corrected = !correction_.empty();
        if (corrected) add_note(messages, correction_);
        correction_.clear();

        bool began = false, speech_started = false, steered = false;
        const guards::Claim *unbacked = nullptr;   // the claim guard a sentence tripped as it streamed
        std::vector<std::string> unsaid;   // sentences composed while the microphone was open
        SpeechQueue::Chunker chunker;
        llm::ChatOptions opt;
        opt.model = Settings::get().eff_llm_model();
        opt.fast = Settings::get().fast_mode;
        bool deciding;
        {
            std::lock_guard<std::mutex> lk(a_.mtx_);
            deciding = a_.history_.empty() || util::jget(a_.history_.back(), "role", std::string()) != "tool";
        }
        // In fast mode the model's commonest mistake is to answer in words when it should have
        // used a tool: "done" with nothing done, or a price or opening time from memory. So a reply
        // that arrives with no tool call is held back and checked before a word of it is spoken.
        const bool fast_decision = deciding && !req_.acted && !req_.guarded && opt.fast;
        // Experiments only (E3b, ZB_EXP_DECIDE=required): the decision is forced into a tool call, words
        // come through the reply tool and stream as they are written; nothing is held for a second look.
        if (fast_decision && experiment::decide_required()) opt.tool_choice = "required";
        const bool hold = fast_decision && (!experiment::decide_required() || experiment::reply_held());
        // Experiments only (E3, ZB_EXP_DECIDE=parallel): a short check beside this call says whether a
        // tool is needed at all. A draft it clears is spoken as it streams, without the second look.
        experiment::ParallelCheck pcheck;
        if (hold && experiment::parallel_check()) pcheck.start(messages, tool_schema_, opt.model);
        bool released = false;     // E3: the held draft is being spoken as it streams
        std::string held_text;     // E3: the draft so far, while the check has not cleared it
        if (fast_decision) add_note(messages, guards::kDecideNote);
        opt.purpose = round == kMaxRounds - 1 ? "last_round" : corrected ? "correction" : deciding ? "decide" : "after_tool";
        opt.cancel = &a_.round_cancel_;
        opt.on_text = [&](const std::string &piece) {
            if (steered || unbacked) return;
            std::string text = piece;
            if (hold && !released) {
                if (!pcheck.active() && !experiment::reply_held()) return;   // held whole, checked once complete
                held_text += piece;
                if (!pcheck.said_none()) return;   // E3c: released by on_call when the reply tool begins
                released = true;                // E3: cleared while it is still being written
                pcheck.used("stream");
                text = held_text;
            }
            if (!began) { began = true; if (a_.ev_.answer_begin) a_.ev_.answer_begin(); }
            if (a_.ev_.answer_delta) a_.ev_.answer_delta(text);
            for (auto &sentence : chunker.feed(text)) {
                // New input arrived before anything was said: withdraw this draft and let the
                // model reconsider with the extra instruction.
                if (!speech_started && a_.has_pending()) { steered = true; a_.round_cancel_ = true; return; }
                // Checked before the first words are spoken: once said, a claim cannot be taken back.
                // "Done" is false when no tool has run, and when every one that ran failed or is
                // waiting for the user's yes.
                if (!speech_started)
                    for (const guards::Claim &c : guards::kClaims)
                        if (c.as_it_streams && req_.claim_open(c) && c.test(sentence)) {
                            unbacked = &c;
                            a_.round_cancel_ = true;
                            return;
                        }
                // The user has started talking: keep the words back until they have finished.
                if (a_.listening_ || !unsaid.empty()) { unsaid.push_back(sentence); continue; }
                speech_started = true;
                a_.set_state(AgentState::Speaking);
                to_speech(sentence);
            }
        };
        // Experiments only (E3c): words written before the reply tool begins are its answer; before a real tool
        // they are never spoken.
        if (hold && experiment::reply_held())
            opt.on_call = [&](const std::string &name) {
                if (released || name != "reply") return;
                released = true;
                if (!held_text.empty()) opt.on_text(held_text);
            };
        llm::ChatResult res = llm::chat(messages, round == kMaxRounds - 1 ? json::array() : tool_schema_, opt);
        // The user was talking when this step was decided. Nothing has been said or done yet, so
        // what they add is taken into account first: a correction_ should land before the action.
        const bool talked_over = !speech_started && a_.listening_;
        if (!speech_started) wait_while_listening();

        if (steered || (!speech_started && !res.cancelled && res.error.empty() && a_.has_pending() &&
                        (res.tool_calls.empty() || talked_over))) {
            if (began && a_.ev_.answer_discard) a_.ev_.answer_discard();
            steer_in();
            --round;  // a withdrawn draft does not count against the budget
            continue;
        }
        if (a_.cancel_) {
            if (began && a_.ev_.answer_end) a_.ev_.answer_end(res.content, true);
            break;
        }
        if (a_.interrupted_) {
            // The user talked over the answer. Keep what was said so the model knows where it stopped.
            if (!res.content.empty()) {
                std::lock_guard<std::mutex> lk(a_.mtx_);
                a_.history_.push_back({{"role", "assistant"}, {"content", res.content + " [the user interrupted here]"}});
            }
            if (began && a_.ev_.answer_end) a_.ev_.answer_end(res.content, true);
            a_.last_answer_ = res.content;
            return;  // what they say next starts a new turn
        }
        if (!res.error.empty()) {
            if (began && a_.ev_.answer_end) a_.ev_.answer_end(res.content, true);
            if (a_.ev_.error) a_.ev_.error(res.error);
            // Tools have run, so something may have changed, or failed, that the user has not been
            // told about. Say what is known rather than leave them with silence.
            if (req_.acted) {
                const char *lost = "I lost the connection to the AI service before I could finish, so please ask again.";
                deliver(speech_started ? lost
                        : req_.done_said + (req_.failures ? "Something I tried did not work, and I lost the connection to the AI "
                                                  "service before I could say what. Please ask again."
                                                : lost));
            }
            break;
        }

        // E3: a draft the parallel check clears once the draft is complete stands without the second look.
        bool cleared = released;
        if (pcheck.active()) {
            if (!res.tool_calls.empty()) pcheck.used("main_tool");
            else if (hold && !released && !res.content.empty()) {
                cleared = pcheck.wait_none(kCheckWaitMs);
                pcheck.used(cleared ? "after" : pcheck.answered() ? "second_look" : "late");
            }
            pcheck.stop();
            rt_.parallel_check(pcheck.summary());
        }
        if (hold && !cleared && res.tool_calls.empty() && !res.content.empty()) {
            json review = messages;
            review.push_back({{"role", "assistant"}, {"content", res.content}});
            review.push_back({{"role", "user"}, {"content", guards::kSecondLook}});
            llm::ChatOptions check;
            check.model = opt.model;
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
            if (a_.cancel_) break;
            if (a_.has_pending()) {  // the user added something while the draft was being checked
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

        // A draft that claims what no tool call backs is withdrawn, and the model told why (guards::kClaims).
        const guards::Claim *claim = unbacked;
        if (!claim && !speech_started && res.tool_calls.empty())
            for (const guards::Claim &c : guards::kClaims)
                if (req_.claim_open(c) && c.test(res.content)) { claim = &c; break; }
        if (claim) {
            (claim->scope == guards::Claim::NothingWorked ? req_.failure_guarded : req_.guarded) = true;
            rt_.guard(claim->name);
            if (began && a_.ev_.answer_discard) a_.ev_.answer_discard();
            correction_ = claim->note(res.content);
            continue;
        }
        // An empty reply leaves the user with silence: it goes back to the model once, and if the
        // next one is empty too, an answer is made here from what is known.
        if (res.tool_calls.empty() && util::trim(res.content).empty() && !speech_started && unsaid.empty()) {
            if (began && a_.ev_.answer_discard) a_.ev_.answer_discard();
            rt_.guard(req_.nudged || round >= kMaxRounds - 1 ? "empty_answered_here" : "empty");
            if (!req_.nudged && round < kMaxRounds - 1) {
                req_.nudged = true;
                correction_ = guards::empty_reply_note(req_.failures);
                continue;
            }
            deliver(req_.done_said + (req_.failures ? "Sorry, something I tried did not work." : "Sorry, I have no answer to that."));
            if (a_.absorb_inputs(false) > 0) { new_request(); round = -1; continue; }   // a new request: fresh budget
            break;
        }
        if (hold && !released && res.tool_calls.empty()) {
            // The draft stands: release it as if it had streamed.
            began = true;
            if (a_.ev_.answer_begin) a_.ev_.answer_begin();
            if (a_.ev_.answer_delta) a_.ev_.answer_delta(res.content);
            a_.set_state(AgentState::Speaking);
            for (auto &sentence : chunker.feed(res.content)) to_speech(sentence);
        }
        // Words held back while the user was talking, who then cancelled rather than adding anything.
        if (!unsaid.empty() && res.tool_calls.empty()) {
            a_.set_state(AgentState::Speaking);
            for (auto &sentence : unsaid) to_speech(sentence);
        }

        if (!res.tool_calls.empty()) {
            json calls = json::array();
            for (auto &c : res.tool_calls)
                calls.push_back({{"id", c.id}, {"type", "function"}, {"function", {{"name", c.name}, {"arguments", c.args}}}});
            {
                std::lock_guard<std::mutex> lk(a_.mtx_);
                a_.history_.push_back({{"role", "assistant"}, {"content", res.content}, {"tool_calls", calls}});
            }
            if (began) {
                const std::string rest = chunker.flush();
                if (!rest.empty() && !a_.listening_ && unsaid.empty()) to_speech(rest);
                if (a_.ev_.answer_end) a_.ev_.answer_end(res.content, false);
            }
            std::string confirm;
            bool all_confirmed = true;
            for (auto &c : res.tool_calls) {
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
                else result = tools::run(c.name, c.args, tool_ctx);
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
                    if (!tool_ctx.say_refused) req_.failed_calls.emplace(key, error_words(result));
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
            if (steer_in() > 0 || a_.cancel_ || !all_confirmed) continue;

            // Every call succeeded and came with its own confirmation: speak that now rather than
            // spending another model call on it.
            deliver(confirm);
            if (a_.absorb_inputs(false) > 0) { new_request(); round = -1; continue; }   // a new request: fresh budget
            break;
        }

        // Final answer for this request.
        const std::string rest = chunker.flush();
        if (!util::trim(rest).empty()) to_speech(rest);
        {
            std::lock_guard<std::mutex> lk(a_.mtx_);
            a_.history_.push_back({{"role", "assistant"}, {"content", res.content}});
        }
        home::assistant_replied(res.content);   // it may put the question a smart-home change waits on
        a_.last_answer_ = res.content;
        if (began && a_.ev_.answer_end) a_.ev_.answer_end(res.content, false);
        if (res.content.empty() && a_.ev_.error) a_.ev_.error("The model returned an empty answer.");
        // Something typed while the answer was being spoken becomes the next request.
        if (a_.absorb_inputs(false) > 0) { new_request(); round = -1; continue; }   // a new request: fresh budget
        break;
    }
    // Leaving by running out of rounds means the last one asked to go round again, so nothing has
    // been said for this request. Say so rather than falling silent.
    if (round >= kMaxRounds && !a_.cancel_) deliver(req_.done_said + "Sorry, that took more steps than I can do in one go. Please ask again.");
    speech_.end_of_answer();
}
