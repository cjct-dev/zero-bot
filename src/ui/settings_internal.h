// What the settings pages share: reading and changing the settings, a few ways of showing values,
// slow work off the UI thread, and the pages one file opens from another. For settings_pages.cpp
// (the top page, its sections, the status page, the key guide), settings_data.cpp (Your data),
// settings_home.cpp (Smart home) and setup_guide.cpp (the first-run guide); nothing else includes it.
#pragma once
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <string>
#include <thread>

#include "core/settings.h"
#include "core/ui_queue.h"
#include "ui/menu.h"

namespace ui {

// The settings as they are now: a copy that cannot be written to (settings.h). The agent's tools
// change settings too, so every change here goes through change(): made under the settings' lock,
// then saved. It returns the settings after the change.
inline const Settings S() { return Settings::get(); }
inline Settings change(const std::function<void(Settings &)> &fn) { return Settings::update(fn); }

inline std::string or_dash(const std::string &s) { return s.empty() ? "not set" : s; }

inline std::string masked(const std::string &secret) {
    if (secret.empty()) return "";
    if (secret.size() <= 4) return "••••";
    return "••••" + secret.substr(secret.size() - 4);
}

inline std::string on_off(bool v) { return v ? "On" : "Off"; }

// Drops "https://" for display; other values pass through unchanged.
inline std::string short_address(const std::string &v) {
    for (const char *scheme : {"https://", "http://"})
        if (v.compare(0, strlen(scheme), scheme) == 0) return v.substr(strlen(scheme));
    return v;
}

inline int clampi(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

// Runs slow work off the UI thread, then delivers the result back on it. Work that throws (it
// should not) is dropped rather than taking the app down with its thread.
template <typename Work, typename Done>
void background(Work work, Done done) {
    std::thread([work, done]() {
        try {
            auto result = work();
            uiq::post([done, result]() { done(result); });
        } catch (const std::exception &e) {
            fprintf(stderr, "settings: background work failed: %s\n", e.what());
        } catch (...) {
            fprintf(stderr, "settings: background work failed\n");
        }
    }).detach();
}

// The pages one file opens from another.
void pick_model(Menu &menu, const std::string &title, const std::string &mode, std::string Settings::*field,
                bool allow_default);   // settings_pages.cpp
void open_calendar(Menu &menu);         // settings_data.cpp
void open_reminders(Menu &menu);
void open_memory(Menu &menu);
void open_remote(Menu &menu);
void open_saved_conversations(Menu &menu);   // DIAGNOSTICS (temporary, ZB-051)
void open_radio(Menu &menu);
void open_smart_home(Menu &menu);       // settings_home.cpp

}  // namespace ui
