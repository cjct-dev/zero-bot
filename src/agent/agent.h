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
    std::function<void(const std::string &message)> error;
};

class Agent {
public:
    static Agent &get();
    void start(AgentEvents events);

    void submit_text(const std::string &text);
    void submit_audio(std::vector<int16_t> pcm);     // 16 kHz mono
    void interrupt_speech();    // user started talking: stop speaking and stop composing the spoken answer
    void cancel();              // abandon the current run entirely
    void reset_conversation();
    void say(const std::string &text);               // speak a notice (reminders) without involving the model

    AgentState state();
    std::string detail();       // e.g. the tool currently running
    bool busy() const { return busy_; }
    bool has_pending();         // a request has been handed over but not yet picked up
    size_t history_size();

    // Runs one request to completion on the calling thread and returns the final answer
    // (used by the command-line test mode).
    std::string run_blocking(const std::string &text);
    json history_snapshot();

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
    std::atomic<bool> busy_{false};
    std::atomic<bool> cancel_{false};       // whole run
    std::atomic<bool> round_cancel_{false}; // current model call
    std::atomic<bool> interrupted_{false};
    AgentState state_ = AgentState::Idle;
    std::string detail_;
    std::string last_answer_;
};
