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
namespace detail {
// With a plug in the 3.5 mm jack, points the codec's input back at the built-in microphone and
// returns a number for put_back_jack_mic, which restores the jack's input afterwards (exposed for
// the unit test; amixer is run, so neither may be called on the UI thread).
int point_at_builtin_mic();
void put_back_jack_mic(int gen);
}  // namespace detail

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
    // Blocking. Another thread may stop(), kill() or finish() meanwhile: the write then fails or
    // completes, and never reaches anything but this player.
    void write(const void *data, size_t bytes);
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

    // A live stream (internet radio) in place of the queue, played until stop() or play(). It has
    // no place to come back to, so while it is held its player is ended (it uses no data then),
    // and once the holds clear it starts afresh at the live point. A stream that ends by itself is
    // joined again; after three failures in a row it is given up and take_problem() says so.
    void play_live(const std::string &url, const std::string &title);
    bool live() const { return live_; }
    std::string title();                          // the live stream's name ("Harbour FM"), or ""
    std::string take_problem();                   // why a live stream ended by itself, once; else ""

private:
    Music() = default;
    void run_live(int gen, std::string url);
    void stop_locked();                           // stop() with ctl_ already held
    // play(), play_live() and stop() are called from the UI thread (ESC, fn+W/fn+E, settings) and
    // the agent's (the music and radio tools) and each replaces thread_: one at a time.
    std::mutex ctl_;
    std::mutex mtx_;
    std::vector<std::string> queue_;
    size_t index_ = 0;
    std::string current_;
    std::string title_, problem_;                 // the live stream's name, and why it ended (guarded by mtx_)
    proc::Child child_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> live_{false};
    std::atomic<bool> skip_{false};
    std::atomic<int> holds_{0};
    bool stopped_ = false;                        // the player process is suspended (guarded by mtx_)
    std::atomic<int> generation_{0};
};

}  // namespace audio
