// A scene of the art engine (ZB-061): what it puts on the canvas for a lighting, and what moves, worked out
// from the time since it started alone (so that pausing and coming back need nothing, and --shots and the
// unit tests can draw any moment exactly). One file per scene in scenes/, one line in the registry
// (scenes.cpp). No LVGL: the unit tests run every scene on a fake clock (tests/unit/art_test.cpp).
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "ui/art/assets.h"
#include "ui/art/canvas.h"

namespace art {

struct Moment {
    unsigned long ms = 0;   // since the scene started
    bool night = false;     // the night's lighting, else dusk's (core/dusk.h: from the local sunset)
};

// Where the view puts a scene's title (a station's name), in art pixels: a band of the picture where
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

}  // namespace art
