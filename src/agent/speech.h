// Speaks sentences as they are produced: each is synthesized and streamed to the speaker.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
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
    SpeechQueue();
    void run();
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::string> queue_;
    std::thread thread_;
    std::atomic<bool> stop_flag_{false};
    std::atomic<int> generation_{0};
    std::atomic<bool> working_{false};
    std::atomic<bool> playing_{false};
    bool ended_ = true;
    bool stream_open_ = false;
};
