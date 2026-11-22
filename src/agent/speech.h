// Speaks sentences as they are produced: each is synthesized and streamed to the speaker.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class SpeechQueue {
public:
    static SpeechQueue &get();
    void enqueue(const std::string &sentence);
    void end_of_answer();        // nothing more is coming for now; let playback drain
    void stop();                 // silence immediately and drop anything queued
    bool active();               // synthesizing or audibly playing
    // The voice is coming out of the speaker: from the first sound of an answer to its last, but
    // not while its first sentence is still being synthesized (which can take as long as the
    // service does).
    bool playing() const { return playing_; }
    // Called on the speech thread when an answer cannot be spoken (the speech service failed, or
    // the speaker could not be opened), at most once per answer, with the reason. The text stays
    // on the screen; this says why it was not heard. Set once, before the first answer.
    void set_on_failure(std::function<void(const std::string &reason)> cb) { on_failure_ = std::move(cb); }
    // For the command line's timings: told each answer's number as its first sentence is queued (on the
    // thread that queues it, so it can be tied to the request giving it), and again, with `over`, once
    // its last sound has played out, or it was stopped or could not be spoken (on the speech thread).
    // Called with the queue's lock held: it must not call into the queue. Set once, before the first answer.
    void set_observer(std::function<void(int answer, bool over)> cb) { observer_ = std::move(cb); }

    // Splits streamed text into speakable chunks.
    class Chunker {
    public:
        // Feed text; returns complete sentences ready to speak (possibly none).
        std::vector<std::string> feed(const std::string &piece);
        std::string flush();
        void reset() { buf_.clear(); }
    private:
        std::string buf_;
    };

private:
    struct Sentence { std::string text; int answer; };   // answer: the number of the answer it belongs to
    SpeechQueue();
    void run();
    bool take(Sentence &next, int &gen);
    void speak(const Sentence &next, int gen);
    void close_stream(int gen);
    int last_unqueued() const;
    void played_out(int answer);
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Sentence> queue_;
    std::thread thread_;
    std::atomic<bool> stop_flag_{false};
    std::atomic<int> generation_{0};
    std::atomic<bool> working_{false};
    std::atomic<bool> playing_{false};
    bool ended_ = true;
    bool stream_open_ = false;
    std::function<void(const std::string &)> on_failure_;
    std::function<void(int, bool)> observer_;
    int answer_ = 0;            // counts answers (guarded by mtx_)
    int over_ = 0;              // the answers up to this one are over and told so (guarded by mtx_)
    int sounding_ = 0;          // the answer the open stream plays: each answer has its own (speech thread only)
    int reported_ = -1;         // the answer whose failure was last reported (speech thread only)
    void failed(int answer, const std::string &reason);
};
