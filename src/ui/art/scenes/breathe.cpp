// The breathing scene (ZB-064): a sun between two mountains at dusk, a moon over the sea at night, growing as the
// breath comes in and shrinking as it goes out, its light on the water growing with it; waves lap up the beach and
// slide back every 4 s on their own; at night four stars twinkle, each for half a second in 16. Four frames a second.
// The disc takes one of 8 sizes from how full the breath is (Moment::level), never from the time, so that it
// follows any pattern. The breathing exercise shows it over the whole screen (BreathView), its words over the dark
// bands at the top and the foot. The pictures are assets/art/breathe/, added in the order they are drawn.
#include <algorithm>

#include "ui/art/scene.h"

namespace art {

namespace {

constexpr unsigned kPeriodMs = 250;
constexpr int kSizes = 8;                            // the disc's sizes, smallest first: a cell of its sheet each, across
constexpr int kDiscX = 70, kDiscY = 24;              // where a cell goes: the disc in the V between the peaks, and
constexpr int kDiscW = 21, kDiscH = 38;              // its light on the water under it
constexpr int kPhases = 16;                          // the waves' loop, in frames (4 s): a cell of their sheets each, down
constexpr int kWaveY = 62, kWaveH = 15;              // the rows a cell covers, along the whole shore
constexpr int kLoop = 64;                            // the stars' loop, in frames (16 s)
constexpr int kTwinkleStep = 13, kTwinkleFirst = 5;  // the k-th star is out on frames 13k + 5 and 13k + 6, mod 64
constexpr int kTwinkleFrames = 2;

class Breathe : public Scene {
public:
    unsigned period_ms() const override { return kPeriodMs; }
    bool start(Canvas &canvas, Assets &assets, const Moment &m) override;
    void frame(Canvas &canvas, const Moment &m) override;
    Spot title_spot() const override { return {4, 1, 152}; }   // the dark band at the top

private:
    std::vector<int> stars_;   // from left to right
    int disc_ = -1, waves_ = -1, shore_ = -1;
};

bool Breathe::start(Canvas &canvas, Assets &assets, const Moment &m) {
    const std::string lighting = m.night ? "-night.png" : "-dusk.png";
    const Image *sky = assets.image("breathe/sky" + lighting), *land = assets.image("breathe/land" + lighting),
                *disc = assets.image("breathe/disc" + lighting), *waves = assets.image("breathe/waves" + lighting);
    if (!sky || !land || !disc || !waves) return false;
    canvas.add(*sky);
    stars_ = add_stars(canvas, assets.image("breathe/twinkle" + lighting));
    canvas.add(*land);
    if (const Image *lights = assets.image("breathe/lights" + lighting)) canvas.add(*lights);   // lit at night
    disc_ = canvas.add(*disc, {0, 0, kDiscW, kDiscH}, kDiscX, kDiscY);
    waves_ = canvas.add(*waves, {0, 0, kW, kWaveH}, 0, kWaveY);
    // What dusk adds to the waves (the wet sand as the water slides back): the same cells, at dusk only.
    const Image *shore = assets.image("breathe/shore" + lighting);
    shore_ = shore ? canvas.add(*shore, {0, 0, kW, kWaveH}, 0, kWaveY) : -1;
    return true;
}

void Breathe::frame(Canvas &canvas, const Moment &m) {
    const int f = static_cast<int>(m.ms / kPeriodMs % kLoop);
    for (size_t k = 0; k < stars_.size(); ++k) {
        const int since = ((f - kTwinkleStep * static_cast<int>(k) - kTwinkleFirst) % kLoop + kLoop) % kLoop;
        canvas.show(stars_[k], since < kTwinkleFrames);
    }
    const float level = m.level > 0 ? std::min(m.level, 1.f) : 0.f;   // and 0 for a level that is not a number
    const int size = static_cast<int>(level * (kSizes - 1) + 0.5f);
    canvas.cell(disc_, {size * kDiscW, 0, kDiscW, kDiscH});
    const Rect wave{0, f % kPhases * kWaveH, kW, kWaveH};
    canvas.cell(waves_, wave);
    if (shore_ >= 0) canvas.cell(shore_, wave);
}

}  // namespace

}  // namespace art

std::unique_ptr<art::Scene> make_breathe_scene() { return std::make_unique<art::Breathe>(); }
