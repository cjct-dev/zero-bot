// The art on the screen (ZB-061): the scene of what plays (the radio's mast), composed by art::Canvas and shown
// by one lv_image on the chat's stage, between the status bar and the bottom row, with its title (a station's
// name) over a band of it where nothing moves. Only the areas that changed are drawn, at the scene's own clock,
// and nothing is composed while it is not in front. It comes and goes with a dissolve in 2x2 art-pixel blocks,
// six steps of 50 ms (the panel takes one update in 50 ms at most). The app says when it is in front
// (screen_idle.h: scene_in_front) and drives it from App::periodic, sleeping until ms_to_next (App::sleep_ms).
// Create it after the chat and before the menu, the breathing screen and the alarm banner, which stay in front
// of it: it never brings itself forward.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "lvgl.h"
#include "ui/art/assets.h"
#include "ui/art/canvas.h"
#include "ui/art/scene.h"

namespace art {

constexpr unsigned long kNever = ~0ul;   // ms_to_next: no work to come
constexpr unsigned long kStepMs = 50;    // a step of the dissolve: the panel's shortest update
constexpr int kSteps = 6;

class ArtView {
public:
    ArtView();                         // its pictures are the ones built into the program
    void create(lv_obj_t *parent);
    // What plays: a scene by its name ("radio") and the title shown over it; "" for none, and the view lets
    // go of the scene's pictures.
    void play(const std::string &scene, const std::string &title);
    // In front of the chat, or not. A change dissolves from `now`, unless `at_once` (nobody would see it: the
    // screen is off, or something covers the whole stage).
    void front(bool on, unsigned long now, bool at_once);
    void tick(unsigned long now);                        // a frame or a step of the dissolve, when one is due
    unsigned long ms_to_next(unsigned long now) const;   // until tick() has work; kNever when it has none
    bool in_front() const { return front_; }             // in front of the chat, or dissolving in
    bool on_screen() const;                              // in front, or dissolving either way
    unsigned long frames() const { return frames_; }     // frames composed so far (for --shots checks)
    // --shots: the scene at `ms` from its start in a lighting, in front at once, its clock as if started at
    // `now` less `ms`.
    void show_at(unsigned long now, unsigned long ms, bool night);

private:
    void load(const std::string &scene);
    bool catch_up(unsigned long now);
    bool begin(unsigned long ms, bool night);
    void draw_frame(unsigned long now);
    void draw(const std::vector<Rect> &areas);
    void step();
    void set_opaque(bool opaque);
    void fill_alpha(uint8_t alpha);

    lv_obj_t *img_ = nullptr, *title_ = nullptr;
    lv_image_dsc_t solid_{}, holed_{};   // the stage as RGB565 (in front) and RGB565A8 (dissolving): one picture
    std::vector<uint16_t> stage_;      // the stage's RGB565 rows, then an alpha byte for each of its pixels
    std::vector<uint16_t> order_;      // the dissolve's blocks, shuffled once
    Assets assets_;
    Canvas canvas_;
    std::unique_ptr<Scene> scene_;
    std::string name_, playing_;       // the scene taken up, and the one asked for (play)
    bool started_ = false, night_ = false, front_ = false, opaque_ = true;
    unsigned long clock_ = 0;          // when the scene's clock started
    unsigned long frame_ = 0;          // the scene's frame shown, counted from its start
    unsigned long night_checked_ = 0;  // when the lighting was last looked at
    int step_ = kSteps;                // the dissolve's steps done; kSteps when none is under way
    unsigned long next_step_ = 0;
    unsigned long frames_ = 0;
};

}  // namespace art
