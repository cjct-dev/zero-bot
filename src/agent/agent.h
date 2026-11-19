// The conversation loop: takes typed or spoken input, calls the model and tools, and
// speaks the answer. New input that arrives mid-run is folded in as steering.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/util.h"

enum class AgentState { Idle, Transcribing, Thinking, Working, Speaking };

struct AgentEvents {   // called from worker threads
    std::function<void(const std::string &text, bool steering)> user_message;
    std::function<void()> answer_begin;
    std::function<void(const std::string &delta)> answer_delta;
    std::function<void(const std::string &full, bool interrupted)> answer_end;
    std::function<void()> answer_discard;                      // a draft was withdrawn because the user steered
    std::function<void(const std::string &label)> tool;
    std::function<void(const std::string &jpeg_path)> image;
    std::function<void(const std::string &pattern, int minutes)> breathe;   // show a breathing exercise once the reply is spoken
    std::function<void(const std::string &message)> error;
#if ZB_EXPERIMENTS
    // The command line's records for the test sets (cli build only): each tool call as it finishes,
    // with the model's call id, its arguments and the whole result, before any cut for the model.
    std::function<void(const std::string &id, const std::string &name, const std::string &args, const std::string &result)> tool_done;
#endif
};

class Agent {
public:
    static Agent &get();
    void start(AgentEvents events);

    void submit_text(const std::string &text);
    void submit_audio(std::vector<int16_t> pcm);     // 16 kHz mono
    void interrupt_speech();    // user started talking: stop speaking and stop composing the spoken answer
    // The microphone is open. Until it closes nothing new is spoken: an answer that becomes ready
    // meanwhile waits, and is withdrawn if what the user said is then handed over as steering.
    void set_listening(bool on) { listening_ = on; }
    void cancel();              // abandon the current run entirely
    void reset_conversation();
    void say(const std::string &text);               // speak a notice (reminders) without involving the model
    // Something the user has just seen or heard without the model taking part (a timer's alarm),
    // which what they say next may well be about ("give it five more minutes"). It rides along
    // with their next message, if that comes within ten minutes.
    void add_context(const std::string &note);

    AgentState state();
    std::string detail();       // e.g. the tool currently running
    bool busy() const { return busy_; }
    bool has_pending();         // a request has been handed over but not yet picked up

    // Runs one request to completion on the calling thread and returns the final answer
    // (used by the command-line test mode).
    std::string run_blocking(const std::string &text);
    // A copy of the history. With `gone`, also how many messages have left it since the agent started
    // (trimmed from its front, or cleared with the conversation), taken under the same lock: the copy's
    // first message is message number *gone of the whole run, so a caller can tell exactly where a
    // request's messages begin however the history was trimmed meanwhile.
    json history_snapshot(size_t *gone = nullptr);
    // DIAGNOSTICS (temporary, ZB-051; core/activity.h): the conversation from its start, as the
    // model saw it, for saving when the user asks: the messages trimmed from the model's context
    // (shortened) and then the context itself, copied under the lock. `dropped` is set to how
    // many messages even earlier than those were not kept.
    json whole_conversation(size_t *dropped = nullptr);

private:
    Agent() = default;
    struct Input { std::string text; std::vector<int16_t> pcm; bool audio = false; };
    void worker();
    void run_turn();
    int absorb_inputs(bool first);
    void trim_history();
    void set_state(AgentState s, const std::string &detail = "");

    AgentEvents ev_;
    std::thread thread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Input> inputs_;
    json history_ = json::array();
    size_t history_gone_ = 0;               // see history_snapshot (guarded by mtx_)
    std::atomic<bool> busy_{false};
    std::atomic<bool> cancel_{false};       // whole run
    std::atomic<bool> round_cancel_{false}; // current model call
    std::atomic<bool> interrupted_{false};
    std::atomic<bool> listening_{false};
    AgentState state_ = AgentState::Idle;
    std::string detail_;
    std::string last_answer_;
    std::string context_;                   // see add_context (guarded by mtx_)
    time_t context_at_ = 0;
    // DIAGNOSTICS (temporary): what trim_history took out of history_, for whole_conversation
    // (guarded by mtx_; at most kMaxEarlier messages, the rest counted in earlier_dropped_).
    json earlier_ = json::array();
    size_t earlier_dropped_ = 0;
};
