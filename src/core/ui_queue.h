// Thread-safe queue of closures that worker threads post and the LVGL thread drains.
#pragma once
#include <functional>
#include <mutex>
#include <vector>

namespace uiq {
inline std::mutex &mtx() { static std::mutex m; return m; }
inline std::vector<std::function<void()>> &items() { static std::vector<std::function<void()>> v; return v; }

inline void post(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(mtx());
    items().push_back(std::move(fn));
}

inline void drain() {
    std::vector<std::function<void()>> batch;
    {
        std::lock_guard<std::mutex> lk(mtx());
        batch.swap(items());
    }
    for (auto &fn : batch) fn();
}
}  // namespace uiq
