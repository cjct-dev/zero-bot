// Microphone capture, speech playback and music playback via PipeWire command-line tools.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
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
    // Ends the player at once without waiting for it, from any thread; a write() stuck on a player
    // that has stopped reading returns. The thread that writes to it reaps it with stop().
    void kill();
    bool playing();                               // still producing sound?

private:
    std::mutex mtx_;
    proc::Child child_;
};

std::string wav_from_pcm(const std::vector<int16_t> &pcm, int rate);
void set_volume(int percent);                     // system output volume, 0..100
// The same without waiting: the mixer command runs on a thread of its own, and when several
// settings arrive meanwhile (a held key, a settings row pressed repeatedly) only the newest is
// applied. For the UI thread.
void set_volume_async(int percent);
// Which microphone the recorder uses. With `builtin` (the default) it asks for the device's own
// microphone by name, so that connecting Bluetooth headphones neither moves recording to the
// headset nor makes the system drop the headphones into phone-call mode. Otherwise it takes
// whatever the system's default input is.
void set_mic_builtin(bool builtin);
void set_mic_gain(int percent);                   // microphone amplification, 100 = default
void normalize(std::vector<int16_t> &pcm);        // even out the level of a finished recording
void chime(int kind);                             // 0 = start listening, 1 = done, 2 = alert

// The sound of a timer's alarm, synthesised like the chimes: one burst of four short, high beeps
// followed by a pause, at `rate` samples a second.
std::vector<int16_t> alarm_burst(int rate);

// Plays the alarm burst over and over. Each ring has a player process and a thread of its own, so
// start() and stop() return at once and never wait for a player, not even one that has hung: a
// ring under way is cut off (its player is ended there and then) and the new one begins afresh.
// Call start() and stop() from one thread (the UI thread).
class AlarmSound {
public:
    static AlarmSound &get();
    static constexpr int kGiveWayMs = 5000;
    // Rings until stop(), for at most max_ms. While `wait` returns true (the assistant's voice is
    // coming out of the speaker) no new burst begins, so the words can be heard, but for no more
    // than give_way_ms in all: the first burst always plays, and an alarm is never kept quiet for
    // long by whatever is being said.
    void start(std::function<bool()> wait, int max_ms = 65000, int give_way_ms = kGiveWayMs);
    void stop();
    void stop_and_wait();                         // leaving the app: no player is left on return
    bool active() const;                          // a ring's thread is still running

private:
    struct Ring;
    AlarmSound() = default;
    static void run(std::shared_ptr<Ring> ring);
    std::shared_ptr<Ring> ring_;                  // the ring under way, if any
};

// Background music player with a simple queue.
class Music {
public:
    static Music &get();
    void play(std::vector<std::string> paths);    // replace the queue and start
    void stop();
    void next();
    void previous();                              // back to the track before this one
    // Reasons the music is kept silent. It plays only while none is set, and a queue started
    // while one is set waits for it to clear, so a song never starts over the assistant's own
    // reply only to be cut off by it.
    enum Hold {
        kUser = 1,        // paused by the user
        kMic = 2,         // the microphone is live
        kSpeech = 4,      // the assistant is speaking
        kExchange = 8,    // a request is being answered: from the end of the question to the end of the spoken reply
        kAlarm = 16,      // a timer's alarm is ringing
    };
    void hold(int reasons, bool on);
    bool holding(int reasons) const { return (holds_ & reasons) != 0; }
    void pause(bool on) { hold(kUser, on); }
    bool playing();
    bool paused() const { return (holds_ & kUser) != 0; }
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
    std::atomic<int> holds_{0};
    bool stopped_ = false;                        // the player process is suspended (guarded by mtx_)
    std::atomic<int> generation_{0};
};

}  // namespace audio
