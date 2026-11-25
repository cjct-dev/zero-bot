// The scenes built into the app.
//
// Adding a scene is one line in the list below, X(<name>), plus its file in scenes/, <name>.cpp (or with its kin:
// city.cpp holds the city in both weathers, music.cpp the five music scenes), which implements art::Scene
// (ui/art/scene.h) and defines
//     std::unique_ptr<art::Scene> make_<name>_scene();
// and its pictures in assets/art/<name>/ (a music scene's in music-<name>/; tests/art_check.py checks them; the
// build embeds them). Which one plays for what is ui/art/pick.h's.
#include "ui/art/scene.h"

#include <algorithm>

#define ZB_SCENES(X) \
    X(radio) \
    X(breathe) \
    X(city) \
    X(city_rain) \
    X(pop) \
    X(rock) \
    X(jazz) \
    X(electronic) \
    X(classical) \
    /* end of the list */

#define ZB_DECLARE(name) std::unique_ptr<art::Scene> make_##name##_scene();
ZB_SCENES(ZB_DECLARE)
#undef ZB_DECLARE

namespace art {

std::unique_ptr<Scene> make(const std::string &name) {
#define ZB_MAKE(scene) if (name == #scene) return make_##scene##_scene();
    ZB_SCENES(ZB_MAKE)
#undef ZB_MAKE
    return nullptr;
}

const std::vector<std::string> &names() {
#define ZB_NAME(scene) #scene,
    static const std::vector<std::string> all = {ZB_SCENES(ZB_NAME)};
#undef ZB_NAME
    return all;
}

int add_overlay(Canvas &canvas, const Image &img, bool shown) {
    int x1 = img.w, y1 = img.h, x2 = -1, y2 = -1;
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x)
            if (img.at(x, y)) {
                x1 = std::min(x1, x); y1 = std::min(y1, y);
                x2 = std::max(x2, x); y2 = std::max(y2, y);
            }
    if (x2 < 0) return -1;
    return canvas.add(img, {x1, y1, x2 - x1 + 1, y2 - y1 + 1}, x1, y1, shown);
}

std::vector<Dot> add_dots(Canvas &canvas, const Image &img) {
    std::vector<Dot> dots;
    for (int x = 0; x < img.w; ++x)
        for (int y = 0; y < img.h; ++y)
            if (const uint8_t index = img.at(x, y)) dots.push_back({canvas.add(img, {x, y, 1, 1}, x, y, false), index});
    return dots;
}

std::vector<int> add_stars(Canvas &canvas, const Image *twinkle) {
    std::vector<int> stars;
    if (twinkle)
        for (const Dot &star : add_dots(canvas, *twinkle)) stars.push_back(star.id);
    return stars;
}

}  // namespace art
