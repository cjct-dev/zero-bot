#include "platform/power.h"

#include <fstream>

namespace power {

Battery battery(const std::string &gauge) {
    Battery b;
    std::ifstream capacity(gauge + "/capacity");
    if (!(capacity >> b.percent)) b.percent = -1;
    std::string status;
    std::ifstream(gauge + "/status") >> status;
    b.charging = status == "Charging";
    return b;
}

}  // namespace power
