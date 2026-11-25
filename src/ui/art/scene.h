// A scene of the art engine (ZB-061): what it puts on the canvas for a lighting, and what moves, worked out
// from the moment alone: the time since it started, and for the breathing scene how full the breath is (so that
// pausing and coming back need nothing, and --shots and the unit tests can draw any moment exactly). One file per
// scene in scenes/, one line in the registry (scenes.cpp). No LVGL: the unit tests run every scene on a fake clock
// (tests/unit/art_test.cpp).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ui/art/assets.h"
#include "ui/art/canvas.h"

namespace art {

struct Moment {
    unsigned long ms = 0;   // since the scene started
    bool night = false;     // the night's pictures (the sky, stars, lit windows), else dusk's: blend at least a half
    // How far what the night only recolours (the land) is from dusk's colours to the night's: 0 to 1, in between
    // only while one turns into the other (core/dusk.h: from the local sunset). Assets::lit gives such pictures.
    float blend = 0;
    float level = 0;        // how full the breath is, 0 to 1 (breath::Moment::level): the breathing scene follows it
    static Moment at(unsigned long ms, float blend, float level = 0) { return {ms, blend >= 0.5f, blend, level}; }
};

// Where the view puts a scene's title (a station's name, a song's), in art pixels: a band of the picture where
// nothing moves, wide enough for the text.
struct Spot {
    int x = 0, y = 0, w = 0;
};

class Scene {
public:
    virtual ~Scene() = default;
    virtual unsigned period_ms() const = 0;   // the scene's clock: everything in it steps together
    // Puts the scene's pictures on the (cleared) canvas in the moment's lighting. False when a picture it
    // needs is missing.
    virtual bool start(Canvas &canvas, Assets &assets, const Moment &m) = 0;
    virtual void frame(Canvas &canvas, const Moment &m) = 0;   // what shows at m.ms
    virtual Spot title_spot() const = 0;
};

// The scenes built in, by name ("radio"); nullptr for a name the registry does not list.
std::unique_ptr<Scene> make(const std::string &name);
const std::vector<std::string> &names();

// What several scenes are made of.
// A picture of the whole grid laid over the others, cropped to the pixels it has (the canvas then looks at it only
// there): its id; -1 when it has none.
int add_overlay(Canvas &canvas, const Image &img, bool shown = true);
// Each pixel of a picture of the whole grid that is not clear, added to the canvas as a hidden 1x1 sprite, from left
// to right (down a column first: the order the art numbers its stars and windows in), with its palette index
// (windows of one schedule share one).
struct Dot {
    int id;
    uint8_t index;
};
std::vector<Dot> add_dots(Canvas &canvas, const Image &img);
// The stars of a lighting's `twinkle` picture (none when there is none), each of its pixels added to the canvas as a
// hidden 1x1 sprite of the colour that star takes when it goes out: their ids, from left to right (down a column
// first), for the scene to show each on its own frames.
std::vector<int> add_stars(Canvas &canvas, const Image *twinkle);
// v mod n, from 0 to n - 1 even for v below 0: a frame of a loop counted back.
inline int wrap(int v, int n) { return (v % n + n) % n; }

}  // namespace art
