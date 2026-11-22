// Thread-safe queue of closures that worker threads post and the LVGL thread drains, and the main
// loop's sleep between passes: wait() ends early when something is posted or a key arrives (wake).
#pragma once
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <vector>

namespace uiq {
inline std::mutex &mtx() { static std::mutex m; return m; }
inline std::vector<std::function<void()>> &items() { static std::vector<std::function<void()>> v; return v; }
inline std::condition_variable &woken() { static std::condition_variable c; return c; }
inline bool &wake_due() { static bool due = false; return due; }   // under mtx(): a wake not yet waited for

inline void post(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lk(mtx());
        items().push_back(std::move(fn));
        wake_due() = true;
    }
    woken().notify_one();
}

// Ends the main loop's current or next wait at once (a key from the keyboard's thread).
inline void wake() {
    {
        std::lock_guard<std::mutex> lk(mtx());
        wake_due() = true;
    }
    woken().notify_one();
}

// The main loop's sleep: until `ms` have passed, or a post or a wake, whichever comes first. One that
// came since the last wait ends this one at once, so none is lost between a pass and its sleep.
inline void wait(unsigned long ms) {
    std::unique_lock<std::mutex> lk(mtx());
    woken().wait_for(lk, std::chrono::milliseconds(ms), [] { return wake_due(); });
    wake_due() = false;
}

// How many closures wait to be run: for the checks in --shots, which hold an answer back while they
// move on in the menu, then deliver it with drain().
inline size_t pending() {
    std::lock_guard<std::mutex> lk(mtx());
    return items().size();
}

// Runs what has been posted; how many there were.
inline size_t drain() {
    std::vector<std::function<void()>> batch;
    {
        std::lock_guard<std::mutex> lk(mtx());
        batch.swap(items());
    }
    for (auto &fn : batch) fn();
    return batch.size();
}
}  // namespace uiq
