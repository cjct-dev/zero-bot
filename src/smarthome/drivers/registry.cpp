// The smart-home families built into the app.
//
// Adding a family is one line in the list below, X(<family>, "<Name>"), plus its files in this
// folder: <family>.h and <family>.cpp, which implement home::Driver (smarthome/driver.h) and define
//     std::unique_ptr<home::Driver> make_<family>_driver();
// The name is what the settings pages call the family's devices ("WiZ", "Hue Bridge").
// Discovery must also report the family's devices under the kind its driver claims.
#include "smarthome/home.h"

#define ZB_DRIVERS(X) \
    X(tapo, "Tapo")   \
    X(firetv, "Fire TV") \
    X(homekit, "HomeKit") \
    X(wiz, "WiZ") \
    /* end of the list */

#define ZB_DECLARE(family, name) std::unique_ptr<home::Driver> make_##family##_driver();
ZB_DRIVERS(ZB_DECLARE)
#undef ZB_DECLARE

namespace home {

void register_builtin_drivers() {
#define ZB_REGISTER(family, name) register_driver(make_##family##_driver(), name);
    ZB_DRIVERS(ZB_REGISTER)
#undef ZB_REGISTER
}

}  // namespace home
