// The art on the screen (ZB-061): a scene composed by art::Canvas and shown by one lv_image, in one of two places
// (Place): the scene of what plays (the radio's mast, the city, a music scene) on the chat's stage, between the
// status bar and the bottom row, with its title (a station's name, a song's) over a band of it where nothing moves;
// or the breathing scene over the whole screen, under the breathing screen's words. Only the areas that changed are
// drawn, at the scene's own clock, and nothing is composed while it is not in front. It comes and goes with a
// dissolve in 2x2 art-pixel blocks, six steps of 50 ms (the panel takes one update in 50 ms at most), or at once. Its
// owner says when it is in front (the app: screen_idle.h, scene_in_front; the breathing screen: while it shows and
// the art is allowed) and drives it on every pass of the main loop, which sleeps until ms_to_next (App::sleep_ms).
// The app's view on the stage is created after the chat and before the menu, the breathing screen and the alarm
// banner, which stay in front of it: it never brings itself forward. The breathing screen's own view is a child of
// the breathing screen, created before its words.
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

// Which art rows a view shows, each at its own place on the screen (art row r on screen rows 2r and 2r + 1).
enum class Place {
    Stage,        // the chat's stage: art rows 9 to 75, between the status bar and the bottom row
    FullScreen,   // all 85 rows (the breathing screen)
};

class ArtView {
public:
    explicit ArtView(Place place = Place::Stage);   // its pictures are the ones built into the program
    void create(lv_obj_t *parent);
    // What plays: a scene by its name ("radio") and the title shown over it; "" for none, and the view lets
    // go of the scene's pictures.
    void play(const std::string &scene, const std::string &title);
    bool title_fits(const std::string &scene, const std::string &title) const;   // on one line over that scene
    // In front of the chat, or not. A change dissolves from `now`, unless `at_once` (nobody would see it: the
    // screen is off, or something covers the whole stage).
    void front(bool on, unsigned long now, bool at_once);
    void tick(unsigned long now);                        // a frame or a step of the dissolve, when one is due
    unsigned long ms_to_next(unsigned long now) const;   // until tick() has work; kNever when it has none
    bool in_front() const { return front_; }             // in front of the chat, or dissolving in
    bool on_screen() const;                              // in front, or dissolving either way
    unsigned long frames() const { return frames_; }     // frames composed so far (for --shots checks)
    // How full the breath is, 0 to 1 (breath::Moment::level), from the next frame on: the breathing scene's disc
    // follows it (Moment::level).
    void set_level(float level) { level_ = level; }
    // --shots: the scene at `ms` from its start in a light (Moment::blend: 0 dusk, 1 night), in front at once, its
    // clock as if started at `now` less `ms`.
    void show_at(unsigned long now, unsigned long ms, float blend);

private:
    void load(const std::string &scene);
    bool catch_up(unsigned long now);
    bool begin(unsigned long ms, float blend);
    void draw_frame(unsigned long now);
    void draw(const std::vector<Rect> &areas);
    void step();
    void set_opaque(bool opaque);
    void fill_alpha(uint8_t alpha);
    size_t pixels() const { return static_cast<size_t>(kW * 2) * static_cast<size_t>(rows_ * 2); }

    const int first_row_, rows_;       // the art rows shown (Place)
    lv_obj_t *img_ = nullptr, *title_ = nullptr;
    lv_image_dsc_t solid_{}, holed_{};   // the stage as RGB565 (in front) and RGB565A8 (dissolving): one picture
    std::vector<uint16_t> stage_;      // the rows shown, RGB565, then an alpha byte for each of their pixels
    std::vector<uint16_t> order_;      // the dissolve's blocks, shuffled once
    Assets assets_;
    Canvas canvas_;
    std::unique_ptr<Scene> scene_;
    std::string name_, playing_;       // the scene taken up, and the one asked for (play)
    bool started_ = false, front_ = false, opaque_ = true;
    float blend_ = 0;                  // the light the scene was started in (Moment::blend), in steps of a minute
    float level_ = 0;
    unsigned long clock_ = 0;          // when the scene's clock started
    unsigned long frame_ = 0;          // the scene's frame shown, counted from its start
    unsigned long night_checked_ = 0;  // when the lighting was last looked at
    int step_ = kSteps;                // the dissolve's steps done; kSteps when none is under way
    unsigned long next_step_ = 0;
    unsigned long frames_ = 0;
};

}  // namespace art
