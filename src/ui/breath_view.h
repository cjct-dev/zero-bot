// Full-screen guided breathing (ZB-064). While Dynamic art allows it, the breathing scene fills the screen (a sun
// between two mountains at dusk, a moon over the waves at night: ui/art/scenes/breathe.cpp) and grows as you breathe
// in and shrinks as you breathe out, with the session and the time left over its dark band at the top, and the phase
// and its seconds over the one at the foot. Otherwise a counter on black: the seconds left in the phase, large, and
// the phase under them. The timing is core/breath's.
#pragma once
#include <string>

#include "core/breath.h"
#include "lvgl.h"
#include "ui/art/art_view.h"

class BreathView {
public:
    void create(lv_obj_t *parent);
    void start(const std::string &pattern_id, int minutes);
    void stop();
    bool active() const { return active_; }
    // Advances the words. Returns true once, when a session has run its full length.
    bool tick(unsigned long now_ms);
    // On every pass of the main loop: the scene while `art` (Dynamic art allows it: screen::art_allowed), with its
    // frame when one is due; else the counter. The scene comes and goes at once, as the breathing screen does.
    void follow_art(bool art, unsigned long now_ms);
    unsigned long ms_to_next(unsigned long now_ms) const { return active_ ? art_.ms_to_next(now_ms) : art::kNever; }
    bool scene_shown() const { return active_ && scene_; }   // the scene, not the counter (for --shots checks)
    // Draws a given moment of the session (used for screenshots): the scene in a lighting, its clock as if started
    // at `now_ms` less `elapsed_ms`, or the counter.
    void show_at(unsigned long now_ms, long elapsed_ms, bool night);

private:
    breath::Moment moment(unsigned long now_ms) const;
    void show(const breath::Moment &m);
    void lay_out(bool scene);

    art::ArtView art_{art::Place::FullScreen};
    lv_obj_t *root_ = nullptr;
    lv_obj_t *title_ = nullptr, *phase_ = nullptr, *count_ = nullptr, *foot_ = nullptr;
    const breath::Pattern *pattern_ = nullptr;
    unsigned long started_ = 0, done_at_ = 0;
    long total_ms_ = 0;
    bool active_ = false, scene_ = false;
    int shown_step_ = -1, shown_seconds_ = -1, shown_minute_ = -1;
};
