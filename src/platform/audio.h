// Microphone capture, speech playback and music playback via PipeWire command-line tools.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "platform/proc.h"

namespace audio {

constexpr int kMicRate = 16000;

// Push-to-talk recorder: 16 kHz mono signed 16-bit.
class Recorder {
public:
    ~Recorder();
    bool start();
    std::vector<int16_t> stop();          // returns everything captured
    void cancel();
    bool active() const { return active_; }
    bool has_audio() const { return samples_seen_ > 0; }
    float level() const { return level_; }   // 0..1 smoothed loudness for the meter
    double seconds() const { return static_cast<double>(samples_seen_) / kMicRate; }

private:
    void reader();
    proc::Child child_;
    std::thread thread_;
    std::mutex mtx_;
    std::vector<int16_t> pcm_;
    std::atomic<bool> active_{false};
    std::atomic<size_t> samples_seen_{0};
    std::atomic<float> level_{0};
};

// Streaming player for synthesized speech (raw PCM chunks written as they arrive).
class Speaker {
public:
    ~Speaker();
    bool begin(int rate);                         // opens the output stream
    void write(const void *data, size_t bytes);   // blocking
    void finish();                                // no more audio; let it drain
    void stop();                                  // cut off immediately
    bool playing();                               // still producing sound?

private:
    std::mutex mtx_;
    proc::Child child_;
};

std::string wav_from_pcm(const std::vector<int16_t> &pcm, int rate);
void set_volume(int percent);                     // system output volume, 0..100
void set_mic_gain(int percent);                   // microphone amplification, 100 = default
void normalize(std::vector<int16_t> &pcm);        // even out the level of a finished recording
void chime(int kind);                             // 0 = start listening, 1 = done, 2 = alert

// Background music player with a simple queue.
class Music {
public:
    static Music &get();
    void play(std::vector<std::string> paths);    // replace the queue and start
    void stop();
    void next();
    void pause(bool on);                          // used to duck while talking
    bool playing();
    bool paused() const { return paused_; }
    std::string current();                        // path of the track playing, or ""

private:
    Music() = default;
    void run();
    std::mutex mtx_;
    std::vector<std::string> queue_;
    size_t index_ = 0;
    std::string current_;
    proc::Child child_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> skip_{false};
    std::atomic<bool> paused_{false};
    std::atomic<int> generation_{0};
};

}  // namespace audio
