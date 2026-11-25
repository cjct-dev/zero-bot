// The radio's scene (ZB-062): a mast on a hill at dusk or at night, its red light blinking and rings of radio
// waves spreading from its tip over the sky. The rings lie behind the land, so the mast and the hills stay in
// front of them. Four frames a second, a loop of 16 (4 s): a ring leaves the tip every second and grows for two,
// fading; the light is on for half of every second; at night two stars twinkle. The pictures are
// assets/art/radio/, added in the order they are drawn.
#include <array>

#include "ui/art/scene.h"

namespace art {

namespace {

constexpr unsigned kPeriodMs = 250;
constexpr int kLoop = 16;                          // frames: 4 s
constexpr int kBornEvery = 4;                      // a ring leaves the tip every 4 frames...
constexpr int kAges = 8;                           // ...and shows for 8, one step bigger each
constexpr int kCell = 49;                          // the rings' sheet: a cell per age across, per turn down (-1 to 3)
constexpr int kTipX = 122, kTipY = 29;             // the mast's light, and the rings' centre
constexpr int kTwinkleStep = 5, kTwinkleFirst = 2;   // the k-th star goes out on frame (5k + 2) mod 16

class Radio : public Scene {
public:
    unsigned period_ms() const override { return kPeriodMs; }
    bool start(Canvas &canvas, Assets &assets, const Moment &m) override;
    void frame(Canvas &canvas, const Moment &m) override;
    Spot title_spot() const override { return {4, 21, 88}; }   // the sky left of the rings, clear of the moon

private:
    void add_stars(Canvas &canvas, const Image *twinkle);

    std::array<int, kAges> rings_{};
    std::vector<int> stars_;   // from left to right
    int light_ = -1;
};

bool Radio::start(Canvas &canvas, Assets &assets, const Moment &m) {
    const std::string lighting = m.night ? "-night.png" : "-dusk.png";
    const Image *sky = assets.image("radio/sky" + lighting), *rings = assets.image("radio/rings" + lighting),
                *land = assets.image("radio/land" + lighting), *light = assets.image("radio/light.png");
    if (!sky || !rings || !land || !light) return false;
    canvas.add(*sky);
    add_stars(canvas, assets.image("radio/twinkle" + lighting));
    for (int age = kAges - 1; age >= 0; --age)   // the oldest first, so that newer rings lie on top
        rings_[static_cast<size_t>(age)] = canvas.add(*rings, {age * kCell, 0, kCell, kCell}, kTipX - kCell / 2, kTipY - kCell / 2, false);
    canvas.add(*land);
    if (const Image *lights = assets.image("radio/lights" + lighting)) canvas.add(*lights);   // windows lit at night
    light_ = canvas.add(*light, {0, 0, light->w, light->h}, kTipX, kTipY, false);
    return true;
}

// The stars that twinkle, if this lighting has any: each pixel of the picture is the colour one takes when it goes out.
void Radio::add_stars(Canvas &canvas, const Image *twinkle) {
    stars_.clear();
    if (!twinkle) return;
    for (int x = 0; x < twinkle->w; ++x)
        for (int y = 0; y < twinkle->h; ++y)
            if (twinkle->at(x, y)) stars_.push_back(canvas.add(*twinkle, {x, y, 1, 1}, x, y, false));
}

void Radio::frame(Canvas &canvas, const Moment &m) {
    const int f = static_cast<int>(m.ms / kPeriodMs % kLoop);
    for (size_t k = 0; k < stars_.size(); ++k)
        canvas.show(stars_[k], f == static_cast<int>((kTwinkleStep * k + kTwinkleFirst) % kLoop));
    for (int age = 0; age < kAges; ++age) {
        const int born = f - age;                   // the frame the ring left the tip: -7 to 15
        const bool shown = born % kBornEvery == 0;
        const int id = rings_[static_cast<size_t>(age)];
        if (shown) canvas.cell(id, {age * kCell, (born / kBornEvery + 1) * kCell, kCell, kCell});
        canvas.show(id, shown);
    }
    canvas.show(light_, f % 4 < 2);
}

}  // namespace

}  // namespace art

std::unique_ptr<art::Scene> make_radio_scene() { return std::make_unique<art::Radio>(); }
