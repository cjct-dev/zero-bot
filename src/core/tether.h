// Internet through a paired phone's Bluetooth tethering, as a fallback when Wi-Fi is not
// available. Uses the Bluetooth network profile NetworkManager saved when the phone was paired.
#pragma once
#include <string>

namespace tether {
std::string default_link();     // interface carrying the default route ("wlan0", "bnep0", ...) or ""
bool online();                  // is there a default route at all?
bool via_phone();               // is the default route the Bluetooth link?
bool available();               // is a phone's Bluetooth network profile saved?
std::string connect();          // blocks up to ~25 s; returns "" on success or a short reason
void disconnect();
}  // namespace tether
