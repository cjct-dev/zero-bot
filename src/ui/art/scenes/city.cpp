// The city (ZB-059): a rooftop over a city at dusk or at night, for lo-fi and chill music and calm stations. Eight
// frames a second, a loop of 640 (80 s): a train crosses once, on the viaduct behind the rooftop; ten windows go dark
// and light again, each on its own schedule; the pink billboard flickers twice. In clear weather four clouds drift
// round the dusk sky once, and a star twinkles at night; in the rain 46 streaks fall over the whole picture, a step
// four times a second, and splash on the roof. Two scenes: "city" clear, "city_rain" in the rain (which plays is
// art::Weather's, pick.h). The pictures are assets/art/city/<part>-<clear|rain>-<dusk|night>.png, added in the
// order they are drawn; what the night only recolours (the land, the windows, the billboard, the train, the plant,
// the rain) blends from dusk to night.
#include <algorithm>
#include <iterator>

#include "ui/art/scene.h"

namespace art {

namespace {

constexpr unsigned kPeriodMs = 125;
constexpr int kLoop = 640;                                  // frames: 80 s
constexpr int kTrainY = 50, kTrainFrom = 40, kShedRight = 35;   // the train's top row; its nose comes out from behind
                                                            // the shed wall (all left of x 35) on frame 40, a pixel a frame
constexpr int kCloudsY = 13, kCloudEvery = 4;               // the clouds' strip, a pixel to the right every 4 frames
constexpr int kStarEvery = 97, kStarFirst = 23, kStarOut = 3;   // star k is out for 3 frames from (97k + 23) mod 640
constexpr int kWindowEvery = 61, kWindowFirst = 17;         // window k goes dark on frame (61k + 17) mod 640...
constexpr int kDarkFor = 96, kDarkMore = 29, kDarkSpread = 160;   // ...for 96 + (29k mod 160) frames
constexpr int kFlicker[] = {300, 301, 303, 306, 307, 560, 562};   // the billboard's frames off
constexpr int kDrops = 46, kRainSeed = 7;                   // streaks, from where a fixed sequence puts them
constexpr int kRainTop = 12, kRainRows = 64, kRainBottom = 76;   // they fall round rows 12 to 75, drawn down to 76
constexpr int kRainEvery = 2, kRainDx = -2;                // a step every 2 frames, 2 pixels to the left...
constexpr int kFirstDy = 10, kRainDy = 2;                   // ...and 2 rows down, the first streak 10 (as approved)
constexpr int kSplashes = 5;                                // splashes a step, where the roof is under them
constexpr Rect kStreak{0, 0, 2, 3}, kSplash{2, 0, 3, 2};    // the rain's cells

class City : public Scene {
public:
    explicit City(bool rain) : rain_(rain) {}
    unsigned period_ms() const override { return kPeriodMs; }
    bool start(Canvas &canvas, Assets &assets, const Moment &m) override;
    void frame(Canvas &canvas, const Moment &m) override;
    // The roof's dark floor; the rain falls over it too, as over everything.
    Spot title_spot() const override { return {4, 68, 100}; }

private:
    void add_rain(Canvas &canvas, Assets &assets, const Moment &m);
    void move_train(Canvas &canvas, int f);
    void fall(Canvas &canvas, int f);

    bool rain_;
    std::vector<int> stars_;
    std::vector<std::vector<int>> windows_;   // window k's pixels
    int clouds_[2] = {-1, -1}, neon_ = -1, train_ = -1;
    const Image *train_img_ = nullptr;
    std::vector<int> drops_, splashes_;       // two items a drop: the second wraps round the left edge
    std::vector<int> roof_;                   // the roof's row in each column, -1 where none is
};

bool City::start(Canvas &canvas, Assets &assets, const Moment &m) {
    const std::string lighting = m.night ? "-night.png" : "-dusk.png";
    auto part = [this](const char *name) { return std::string("city/") + name + (rain_ ? "-rain" : "-clear"); };
    const Image *sky = assets.image(part("sky") + lighting), *land = assets.lit(part("land"), m.blend);
    if (!sky || !land) return false;
    canvas.add(*sky);
    stars_ = add_stars(canvas, assets.image(part("twinkle") + lighting));
    clouds_[0] = clouds_[1] = -1;
    if (const Image *clouds = assets.image(part("clouds") + lighting))
        for (int &id : clouds_) id = canvas.add(*clouds, {0, 0, clouds->w, clouds->h}, 0, kCloudsY);
    canvas.add(*land);
    if (const Image *lights = assets.image(part("lights") + lighting)) add_overlay(canvas, *lights);
    const Image *windows = assets.lit(part("windows"), m.blend), *neon = assets.lit(part("neon"), m.blend),
                *front = assets.lit(part("front"), m.blend);
    train_img_ = assets.lit(part("train"), m.blend);
    if (!windows || !neon || !front || !train_img_) return false;
    windows_.clear();
    for (const Dot &dot : add_dots(canvas, *windows)) {
        if (windows_.size() < dot.index) windows_.resize(dot.index);
        windows_[dot.index - 1u].push_back(dot.id);
    }
    neon_ = add_overlay(canvas, *neon, false);
    train_ = canvas.add(*train_img_, {0, 0, 0, 0}, 0, kTrainY, false);
    add_overlay(canvas, *front);   // the plant's leaves, in front of the train
    drops_.clear();
    splashes_.clear();
    if (rain_) add_rain(canvas, assets, m);
    return !rain_ || !drops_.empty();
}

void City::add_rain(Canvas &canvas, Assets &assets, const Moment &m) {
    const Image *rain = assets.lit("city/rain", m.blend), *roof = assets.image(std::string("city/roof") + (m.night ? "-night.png" : "-dusk.png"));
    if (!rain || !roof) return;
    roof_.assign(kW, -1);
    for (int x = 0; x < roof->w && x < kW; ++x)
        for (int y = 0; y < roof->h && roof_[static_cast<size_t>(x)] < 0; ++y)
            if (roof->at(x, y)) roof_[static_cast<size_t>(x)] = y;
    for (int i = 0; i < 2 * kDrops; ++i) drops_.push_back(canvas.add(*rain, kStreak, 0, 0, false));
    for (int i = 0; i < kSplashes; ++i) splashes_.push_back(canvas.add(*rain, kSplash, 0, 0, false));
}

void City::frame(Canvas &canvas, const Moment &m) {
    const int f = static_cast<int>(m.ms / kPeriodMs % kLoop);
    for (size_t k = 0; k < stars_.size(); ++k)
        canvas.show(stars_[k], wrap(f - kStarEvery * static_cast<int>(k) - kStarFirst, kLoop) < kStarOut);
    const int drift = f / kCloudEvery % kW;
    if (clouds_[0] >= 0) {
        canvas.move(clouds_[0], drift, kCloudsY);
        canvas.move(clouds_[1], drift - kW, kCloudsY);
    }
    for (size_t k = 0; k < windows_.size(); ++k) {
        const int dark_from = static_cast<int>(kWindowEvery * k + kWindowFirst) % kLoop;
        const bool dark = wrap(f - dark_from, kLoop) < kDarkFor + static_cast<int>(kDarkMore * k) % kDarkSpread;
        for (const int id : windows_[k]) canvas.show(id, dark);
    }
    if (neon_ >= 0) canvas.show(neon_, std::find(std::begin(kFlicker), std::end(kFlicker), f) != std::end(kFlicker));
    move_train(canvas, f);
    if (!drops_.empty()) fall(canvas, f);
}

// The train, a pixel further each frame, hidden left of the shed wall.
void City::move_train(Canvas &canvas, int f) {
    const int x = kShedRight - train_img_->w + f - kTrainFrom, hidden = std::max(0, kShedRight - x);
    const bool on = f >= kTrainFrom && x < kW && hidden < train_img_->w;
    if (on) {
        canvas.cell(train_, {hidden, 0, train_img_->w - hidden, train_img_->h});
        canvas.move(train_, x + hidden, kTrainY);
    }
    canvas.show(train_, on);
}

// The rain at its step: each streak where the fixed sequence started it, moved on; the splashes where the step's
// numbers fall on the roof.
void City::fall(Canvas &canvas, int f) {
    const int step = f / kRainEvery;
    uint32_t seed = kRainSeed;
    for (int k = 0; k < kDrops; ++k) {
        seed = (seed * 1103515245u + 12345u) & 0x7fffffffu;
        const int x0 = static_cast<int>(seed % kW);
        seed = (seed * 1103515245u + 12345u) & 0x7fffffffu;
        const int y0 = static_cast<int>(seed % kRainRows);
        const int dy = k == 0 ? kFirstDy : kRainDy;
        const int x = wrap(x0 + kRainDx * step, kW), top = kRainTop + wrap(y0 + dy * step, kRainRows);
        const Rect cell{kStreak.x, kStreak.y, kStreak.w, std::min(kStreak.h, kRainBottom + 1 - top)};
        for (int copy = 0; copy < 2; ++copy) {   // its foot is a column to the left, round the edge from x 0
            const int id = drops_[static_cast<size_t>(2 * k + copy)];
            canvas.cell(id, cell);
            canvas.move(id, x - 1 + copy * kW, top);
            canvas.show(id, true);
        }
    }
    for (int k = 0; k < kSplashes; ++k) {   // its column from the step's numbers, as drawn
        const int x = (step * 37 + k * 53) % 997 * 7 % kW, y = roof_[static_cast<size_t>(x)];
        const int id = splashes_[static_cast<size_t>(k)];
        if (y >= 0) canvas.move(id, x - 1, y - 1);
        canvas.show(id, y >= 0);
    }
}

}  // namespace

}  // namespace art

std::unique_ptr<art::Scene> make_city_scene() { return std::make_unique<art::City>(false); }
std::unique_ptr<art::Scene> make_city_rain_scene() { return std::make_unique<art::City>(true); }
