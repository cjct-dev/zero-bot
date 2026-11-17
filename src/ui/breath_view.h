// Full-screen guided breathing: an orb that grows as you breathe in and shrinks as you breathe out.
#pragma once
#include <string>

#include "core/breath.h"
#include "lvgl.h"

class BreathView {
public:
    void create(lv_obj_t *parent);
    void start(const std::string &pattern_id, int minutes);
    void stop();
    bool active() const { return active_; }
    // Advances the animation. Returns true once, when a session has run its full length.
    bool tick(unsigned long now_ms);
    void show_at(long elapsed_ms);     // draws a given moment (used for screenshots)

private:
    lv_obj_t *root_ = nullptr, *ring_ = nullptr, *orb_ = nullptr;
    lv_obj_t *title_ = nullptr, *phase_ = nullptr, *count_ = nullptr, *foot_ = nullptr;
    const breath::Pattern *pattern_ = nullptr;
    unsigned long started_ = 0, done_at_ = 0;
    long total_ms_ = 0;
    bool active_ = false;
    int shown_step_ = -1, shown_seconds_ = -1, shown_size_ = -1, shown_minute_ = -1;
};
