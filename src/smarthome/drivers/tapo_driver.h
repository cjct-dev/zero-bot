// TP-Link Tapo plugs and power strips as a smart-home driver, over the local transport in
// smarthome/tapo.h. A plug is one thing; each socket of a strip is one thing. Every plug obeys
// one Tapo account, which is kept as the family's secret (secrets.tapo: e-mail and the SHA-1 of
// the password, never the password itself).
#pragma once
#include <memory>
#include <string>

#include "smarthome/driver.h"

std::unique_ptr<home::Driver> make_tapo_driver();

namespace home {

// The Tapo account, for the settings page: who it is and whether a password is kept, never the
// password or its hash.
struct TapoAccount {
    std::string email;
    bool password_saved = false;
    bool refused = false;   // a plug rejected it or locked itself: nothing is sent until it is typed again
    bool set() const { return !email.empty() && password_saved; }
};
TapoAccount tapo_account();
// An empty password keeps the saved one; an empty e-mail forgets the account. Either change
// lets the plugs be tried afresh.
void set_tapo_account(const std::string &email, const std::string &password);

}  // namespace home
