// "Start at power-on": a per-user systemd drop-in that runs the app before the launcher.
#pragma once

namespace autostart {
bool supported();        // is the stock launcher service present?
bool enabled();
bool set(bool on);       // returns false if the change could not be written
}
