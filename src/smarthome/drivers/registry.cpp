// The smart-home families built into the app.
//
// Adding a family is one line in the list below, plus its files in this folder: <family>.h and
// <family>.cpp, which implement home::Driver (smarthome/driver.h) and define
//     std::unique_ptr<home::Driver> make_<family>_driver();
// Discovery must also report the family's devices under the kind its driver claims.
#include "smarthome/home.h"

#define ZB_DRIVERS(X) \
    X(tapo)           \
    /* end of the list */

#define ZB_DECLARE(family) std::unique_ptr<home::Driver> make_##family##_driver();
ZB_DRIVERS(ZB_DECLARE)
#undef ZB_DECLARE

namespace home {

void register_builtin_drivers() {
#define ZB_REGISTER(family) register_driver(make_##family##_driver());
    ZB_DRIVERS(ZB_REGISTER)
#undef ZB_REGISTER
}

}  // namespace home
