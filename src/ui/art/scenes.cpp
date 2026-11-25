// The scenes built into the app.
//
// Adding a scene is one line in the list below, X(<name>), plus its file in scenes/, <name>.cpp, which
// implements art::Scene (ui/art/scene.h) and defines
//     std::unique_ptr<art::Scene> make_<name>_scene();
// and its pictures in assets/art/<name>/ (tests/art_check.py checks them; the build embeds them).
#include "ui/art/scene.h"

#define ZB_SCENES(X) \
    X(radio) \
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

}  // namespace art
