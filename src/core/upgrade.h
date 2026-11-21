// What an older version left behind that this one no longer uses, tidied once at start.
#pragma once

#include <string>

namespace upgrade {

// "Start at power-on" (removed after 0.4.8: the boot logo hid the app it started) wrote a
// drop-in for the launcher's user service. Removes HOME's copy only when its first line says Zero
// Bot wrote it, and its folder when that is then empty. True when the file was removed, so the
// user's service manager must be reloaded.
bool remove_power_on_dropin(const std::string &home);

// At start of the app: the above for this user, then the reload beside the UI thread. Quiet;
// failures are ignored.
void tidy_at_start();

}  // namespace upgrade
