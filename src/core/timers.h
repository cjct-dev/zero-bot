// Countdown timers ("set a timer for ten minutes"): the countdown text, which timer a few spoken
// words mean, the timer tool's actions and the alarm that rings when one ends. Timers themselves
// are kept by Store. Nothing here draws or plays anything (the app passes the sound in), so all of
// it can be tested without a screen or a speaker.
#pragma once
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "core/store.h"
#include "core/util.h"

namespace timers {

constexpr long kMaxSeconds = 24 * 3600;       // the longest timer
constexpr long kMissedAfter = 120;             // a carried-over timer whose end is noticed this many seconds late does not ring
constexpr unsigned long kRingMs = 60000;       // the longest the alarm rings
constexpr int kClockWaitMs = 1000;             // the longest the tool waits for the main loop to follow a clock step

// Time left the way a clock shows it: "0:00", "4:59", "59:59", "1:00:00", "1:02:03".
std::string clock_text(long seconds);
// The status bar's countdown: the soonest end, then "+N" for the other timers; "" when none.
std::string badge(const std::vector<time_t> &ends, time_t now);
// A length in words, for the model: "45 seconds", "4 minutes 59 seconds", "1 hour 2 minutes".
std::string length_words(long seconds);
// What a timer is called: the label the user gave, tidied ("Food out of the oven"), or, without
// one, its length ("5 minute timer").
std::string make_label(const std::string &raw, long seconds);
// The timers among `running` that a few words from a label mean ("study", "the oven one"). Words
// that name no timer in particular ("the timer", "") mean all of them. When several fit equally
// well all are returned, and the caller asks which.
std::vector<Reminder> match(const std::vector<Reminder> &running, const std::string &words);

// The timer tool: actions start, cancel, list and add_time. Returns text for the model, starting
// "ERROR: " when nothing was done. `now` is the current time, passed in so tests can fix it, with
// how far the wall clock then stood from the steady one (`offset_ms`, see Store::reckon): the tool
// works with the time as the timers reckon it, should the clock have been set just then.
std::string run_tool(const json &args, time_t now, long long offset_ms = Store::kNoClock);
// The same at this moment, from both clocks: what the assistant's calls use. Just after the wall
// clock has been set it first waits, kClockWaitMs at most, for the main loop to move the timers.
std::string run_tool(const json &args);

// The clock that screens showing a timer's time left read. Screenshots fix it so that their
// images do not depend on when they were taken; 0 returns to the real clock.
time_t now();
void fix_clock(time_t t);
// Keeps the timers set in this run counting real time when the wall clock is set (a time sync
// after start-up): reads the clocks and passes them to Store::follow_clock. Call on every tick.
// Returns the time it read, for the rest of the tick: a second reading of the clock could fall on
// the other side of a step from the timers.
time_t follow_clock();

// A timer the app has just noticed is over.
struct Ended {
    int id = 0;
    std::string label;
    time_t end = 0;
    // It was carried over from an earlier run of the app and ended kMissedAfter or more before it
    // was noticed, so while the app was closed: shown, not rung.
    bool missed = false;
};

// The alarm that rings when timers end. It keeps the music quiet while it rings and lets go on
// every way out: a key, the time limit, stop() (leaving the app) and its destruction.
class Alarm {
public:
    struct Sound {                       // starts and stops the ringing; left empty in tests
        std::function<void()> start, stop;
    };
    explicit Alarm(Sound sound = {});
    ~Alarm();
    Alarm(const Alarm &) = delete;
    Alarm &operator=(const Alarm &) = delete;

    // Call often from the main loop. Takes the timers that have ended from the store, oldest
    // first; all but the missed ones ring (with `quiet`, as when the user has muted the device,
    // without sound). Each that rings starts the sound again, so one that joins a ring under way
    // gets a whole one of its own. While the microphone is open nothing is taken: an end waits
    // until it has closed, as reminders do.
    std::vector<Ended> poll(time_t now, unsigned long now_ms, bool recording, bool quiet);
    // Ends the ring kRingMs after the last timer that joined it. True when it did so now.
    bool tick(unsigned long now_ms);
    // A key was pressed. While ringing that silences the alarm and the key is used up: Silence for
    // any key, Listen for TAB (the app then starts listening). None when nothing was ringing.
    enum class Key { None, Silence, Listen };
    Key key(bool tab);
    void stop();
    bool ringing() const { return ringing_; }
    bool sounding() const { return sounding_; }
    const std::vector<std::string> &labels() const { return labels_; }   // the timers ringing, in the order they ended

private:
    Sound sound_;
    bool ringing_ = false, sounding_ = false;
    unsigned long until_ = 0;
    std::vector<std::string> labels_;
};

}  // namespace timers
