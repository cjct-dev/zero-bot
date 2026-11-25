// The music player's scenes (ZB-133), one for each family of genres (which plays for which: pick.h), each at dusk
// and at night. Eight frames a second, a loop of 64 (8 s); a beat is 4 frames. Each has its sky, its land (with what
// never moves drawn on it), what the night lights up beyond the land's colours, and stars that twinkle, then its own
// parts:
//   pop         24 bars dance on the lake's waterline, a lift on every beat, their reflection under them
//   rock        the stage lamps light in two groups on alternate beats, colours moving along the bar; two
//               searchlights swing behind the stage
//   jazz        steam puffs rise from the street grate; the saxophone sign buzzes off now and then
//   electronic  the grid's cross lines run towards the viewer, one spacing a second
//   classical   the candle flames flicker, the chandelier's and the candelabra's on alternate frames
// The pictures are assets/art/music-<family>/, added in the order they are drawn; what the night only recolours
// (the land and the moving parts) blends from dusk to night.
#include <algorithm>
#include <cmath>
#include <iterator>

#include "ui/art/scene.h"

namespace art {

namespace {

constexpr unsigned kPeriodMs = 125;
constexpr int kLoop = 64;                             // frames: 8 s
constexpr int kBeat = 4;                              // frames
constexpr int kStarEvery = 11, kStarFirst = 3;        // star k is out on frames 11k + 3 and 11k + 4 (mod 64)

// What every music scene has, and its clock.
class Music : public Scene {
public:
    explicit Music(const char *family) : dir_(std::string("music-") + family + "/") {}
    unsigned period_ms() const override { return kPeriodMs; }
    bool start(Canvas &canvas, Assets &assets, const Moment &m) override;
    void frame(Canvas &canvas, const Moment &m) override;

protected:
    std::string path(const char *name, const Moment &m) const { return dir_ + name + (m.night ? "-night.png" : "-dusk.png"); }
    std::string stem(const char *name) const { return dir_ + name; }
    // The family's own parts, over the rest; false when a picture they need is missing.
    virtual bool add_parts(Canvas &canvas, Assets &assets, const Moment &m) = 0;
    virtual void move_parts(Canvas &canvas, int f) = 0;   // at frame f of the loop

private:
    std::string dir_;
    std::vector<int> stars_;
};

bool Music::start(Canvas &canvas, Assets &assets, const Moment &m) {
    const Image *sky = assets.image(path("sky", m)), *land = assets.lit(stem("land"), m.blend);
    if (!sky || !land) return false;
    canvas.add(*sky);
    canvas.add(*land);
    if (const Image *lights = assets.image(path("lights", m))) add_overlay(canvas, *lights);
    stars_ = add_stars(canvas, assets.image(path("twinkle", m)));
    return add_parts(canvas, assets, m);
}

void Music::frame(Canvas &canvas, const Moment &m) {
    const int f = static_cast<int>(m.ms / kPeriodMs % kLoop);
    for (size_t k = 0; k < stars_.size(); ++k)
        canvas.show(stars_[k], wrap(f - kStarEvery * static_cast<int>(k) - kStarFirst, kLoop) < 2);
    move_parts(canvas, f);
}

// ------------------------------------------------------------------ pop (and anything else)
constexpr int kBars = 24, kBarsX = 32, kBarStep = 4;   // bar i stands at x 32 + 4i, two pixels wide...
constexpr int kWater = 49, kTallest = 17;               // ...on row 49, up to 17 high

class Pop : public Music {
public:
    Pop() : Music("pop") {}
    Spot title_spot() const override { return {36, 10, 120}; }   // the dark sky right of the tree

private:
    bool add_parts(Canvas &canvas, Assets &assets, const Moment &m) override {
        const Image *bars = assets.lit(stem("bars"), m.blend);   // a cell for each height, the reflection under it
        if (!bars) return false;
        bars_.clear();
        for (int i = 0; i < kBars; ++i) bars_.push_back(canvas.add(*bars, {0, 0, 2, bars->h}, kBarsX + kBarStep * i, kWater + 1 - kTallest));
        return true;
    }
    void move_parts(Canvas &canvas, int f) override {
        for (int i = 0; i < kBars; ++i) canvas.cell(bars_[static_cast<size_t>(i)], {2 * (height(i, f) - 1), 0, 2, 2 * kTallest});
    }
    // Bar i's height at frame f: three waves of its own under a hump in the middle, and a lift on every beat.
    static int height(int i, int f) {
        const double pi = 3.141592653589793, u = (i - (kBars - 1) / 2.0) / (kBars / 2.0);
        const double hump = 0.25 + 0.75 * std::exp(-2.2 * u * u);
        const double waves = 0.5 + 0.22 * std::sin(2 * pi * (f / 32.0 + i * 0.37)) + 0.18 * std::sin(2 * pi * (f / 16.0 + i * 0.61)) +
                             0.10 * std::sin(2 * pi * (f / 8.0 + i * 0.23));
        const double lift = std::max(0.0, 1 - (f % kBeat) / 2.5) * 0.25;
        return std::max(1, static_cast<int>(std::lround(kTallest * hump * std::min(1.0, waves + lift))));
    }

    std::vector<int> bars_;
};

// ------------------------------------------------------------------ rock
constexpr int kBeamsX = 17, kBeamsY = 11, kBeamSteps = 32;    // the searchlights' band of the sky; a step every 2 frames
constexpr int kLamps[] = {49, 58, 66, 75, 84, 92, 101, 109};   // each stage lamp's left edge...
constexpr int kLampY = 35, kLampW = 2, kLampH = 3, kColours = 3;   // ...its face and the glint under it; three colours

class Rock : public Music {
public:
    Rock() : Music("rock") {}
    Spot title_spot() const override { return {4, 61, 120}; }   // the dark ground in front of the stage

private:
    bool add_parts(Canvas &canvas, Assets &assets, const Moment &m) override {
        beams_img_ = assets.lit(stem("beams"), m.blend);   // a cell for each step, down
        const Image *lamps = assets.image(stem("lamps") + ".png");   // a cell for each lamp, across, and colour, down
        if (!beams_img_ || !lamps) return false;
        beams_ = canvas.add(*beams_img_, {0, 0, beams_img_->w, beams_img_->h / kBeamSteps}, kBeamsX, kBeamsY);
        lamps_.clear();
        for (const int x : kLamps) lamps_.push_back(canvas.add(*lamps, {0, 0, kLampW, kLampH}, x, kLampY, false));
        return true;
    }
    void move_parts(Canvas &canvas, int f) override {
        const int h = beams_img_->h / kBeamSteps, beat = f / kBeat;
        canvas.cell(beams_, {0, f / 2 % kBeamSteps * h, beams_img_->w, h});
        for (int i = 0; i < static_cast<int>(lamps_.size()); ++i) {   // half of them on each beat
            const int id = lamps_[static_cast<size_t>(i)];
            const bool lit = (i + beat) % 2 == 0;
            if (lit) canvas.cell(id, {i * kLampW, (i / 2 + beat / 2) % kColours * kLampH, kLampW, kLampH});
            canvas.show(id, lit);
        }
    }

    const Image *beams_img_ = nullptr;
    int beams_ = -1;
    std::vector<int> lamps_;
};

// ------------------------------------------------------------------ jazz (and blues)
constexpr int kGrateX = 75, kGrateY = 75;                    // the street grate the steam rises from
constexpr int kPuffs = 3, kPuffLife = 16, kPuffApart = 5;    // puff k is (f + 5k) mod 16 frames old, a row higher a frame
constexpr int kPuffW = 6, kPuffH = 3, kPuffSizes = 4;        // its cell: a size for each 4 frames of its life
constexpr int kBuzz[] = {8, 9, 11, 40, 41};                  // the sign's frames off

class Jazz : public Music {
public:
    Jazz() : Music("jazz") {}
    Spot title_spot() const override { return {36, 10, 120}; }   // the top of the club's wall and the sky

private:
    bool add_parts(Canvas &canvas, Assets &assets, const Moment &m) override {
        const Image *steam = assets.lit(stem("steam"), m.blend), *sign = assets.lit(stem("sign"), m.blend);
        if (!steam || !sign) return false;
        puffs_.clear();
        for (int k = 0; k < kPuffs; ++k) puffs_.push_back(canvas.add(*steam, {0, 0, kPuffW, kPuffH}, 0, 0));
        sign_ = add_overlay(canvas, *sign, false);
        return true;
    }
    void move_parts(Canvas &canvas, int f) override {
        for (int k = 0; k < kPuffs; ++k) {   // each sways as it rises, and grows thinner
            const int age = (f + kPuffApart * k) % kPuffLife;
            const int sway = static_cast<int>(std::lround(1.6 * std::sin((age + 3 * k) / 2.2)));
            const int x = kGrateX + sway + 2 * (k - 1), y = kGrateY - 1 - age;
            const int id = puffs_[static_cast<size_t>(k)];
            canvas.cell(id, {age * kPuffSizes / kPuffLife * kPuffW, 0, kPuffW, kPuffH});
            canvas.move(id, x - 2, y - 1);
        }
        if (sign_ >= 0) canvas.show(sign_, std::find(std::begin(kBuzz), std::end(kBuzz), f) != std::end(kBuzz));
    }

    std::vector<int> puffs_;
    int sign_ = -1;
};

// ------------------------------------------------------------------ electronic
constexpr int kHorizon = 58, kGridFoot = 75;     // the grid runs from the horizon to row 75...
constexpr int kFirstLine = 61, kNearFrom = 65;   // ...its cross lines from row 61, bright from row 65
constexpr int kLineEvery = 8;                    // frames for a line to reach the next one's place

class Electronic : public Music {
public:
    Electronic() : Music("electronic") {}
    Spot title_spot() const override { return {4, 10, 120}; }   // the dark top of the sky

private:
    bool add_parts(Canvas &canvas, Assets &assets, const Moment &m) override {
        const Image *lines = assets.lit(stem("lines"), m.blend);   // row 0 near, row 1 far
        if (!lines) return false;
        lines_.clear();
        for (int y = kFirstLine; y <= kGridFoot; ++y) lines_.push_back(canvas.add(*lines, {0, y >= kNearFrom ? 0 : 1, kW, 1}, 0, y, false));
        return true;
    }
    // The rows the cross lines are on: the n-th at a depth of 1 + 0.6 (n less how far the frame is through its step).
    void move_parts(Canvas &canvas, int f) override {
        std::vector<bool> on(lines_.size(), false);
        const double phase = f % kLineEvery / static_cast<double>(kLineEvery);
        for (int n = 0; n < 40; ++n) {
            const double z = 1.0 + (n - phase) * 0.6;
            if (z <= 0.7) continue;
            const int y = static_cast<int>(std::lround(kHorizon + (kGridFoot - kHorizon) / z));
            if (y >= kFirstLine && y <= kGridFoot) on[static_cast<size_t>(y - kFirstLine)] = true;
        }
        for (size_t i = 0; i < lines_.size(); ++i) canvas.show(lines_[i], on[i]);
    }

    std::vector<int> lines_;   // rows 61 to 75
};

// ------------------------------------------------------------------ classical
struct Candle {
    int x, y, group;   // its top, under its flame; the chandelier's (0) or the candelabra's (1)
};
constexpr Candle kCandles[] = {{79, 14, 0}, {73, 16, 0}, {86, 16, 0}, {117, 50, 1}, {114, 51, 1}, {121, 51, 1}};
constexpr int kShapes[] = {0, 0, 1, 0, 4, 0, 2, 0, 0, 3, 0, 1, 4, 0, 2, 0};   // a flame's shapes, each for 2 frames

class Classical : public Music {
public:
    Classical() : Music("classical") {}
    Spot title_spot() const override { return {4, 67, 100}; }   // the floor

private:
    bool add_parts(Canvas &canvas, Assets &assets, const Moment &) override {
        const Image *flames = assets.image(stem("flames") + ".png");   // a 3x3 cell for each shape
        if (!flames) return false;
        flames_.clear();
        for (const Candle &c : kCandles) flames_.push_back(canvas.add(*flames, {0, 0, 3, 3}, c.x - 1, c.y - 3));
        return true;
    }
    // Each flame keeps its shape for 2 frames, the chandelier's changing on even frames and the candelabra's on odd
    // ones (each update is then a few rows); candle k is 5 shapes on from the one before.
    void move_parts(Canvas &canvas, int f) override {
        for (size_t k = 0; k < flames_.size(); ++k) {
            const int changed = wrap(f - wrap(f - kCandles[k].group, 2), kLoop);
            const int shape = kShapes[(changed / 2 + static_cast<int>(k) * 5) % 16];
            canvas.cell(flames_[k], {3 * shape, 0, 3, 3});
        }
    }

    std::vector<int> flames_;
};

}  // namespace

}  // namespace art

std::unique_ptr<art::Scene> make_pop_scene() { return std::make_unique<art::Pop>(); }
std::unique_ptr<art::Scene> make_rock_scene() { return std::make_unique<art::Rock>(); }
std::unique_ptr<art::Scene> make_jazz_scene() { return std::make_unique<art::Jazz>(); }
std::unique_ptr<art::Scene> make_electronic_scene() { return std::make_unique<art::Electronic>(); }
std::unique_ptr<art::Scene> make_classical_scene() { return std::make_unique<art::Classical>(); }
