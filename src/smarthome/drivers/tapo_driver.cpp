// TP-Link Tapo plugs and strips on the smart-home driver contract. All protocol work is in
// smarthome/tapo.{h,cpp}; this file maps it onto things, changes and the pairing steps.
//
// A plug locks itself after a few rejected logins, so a login is never retried: the transport
// remembers a rejection while the program runs, and this driver keeps it in the family secret
// ("refused") so that a later run does not try the same password again either. A password just
// typed is on trial (driver.h home::trial) until a plug accepts it: the trial is written before
// the first login with it, and nothing is sent when that write fails, so a refusal that could not
// be recorded (a full store, the app stopped) still stops a later run. The account e-mail is
// checked against the owner a plug announces before any login.
#include "smarthome/drivers/tapo_driver.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>

#include "core/util.h"
#include "smarthome/home.h"
#include "smarthome/tapo.h"

namespace {

using home::json;

const char *const kFamily = "tapo";
const char *const kNotSaved = "the Tapo account could not be saved on this device (its storage may be full), so nothing was sent "
                              "to the plug";

// The account is read, changed and written back by the settings page and by every plug's reads at
// once (up to four threads): one change at a time, so that none undoes another (a refusal kept by
// one thread must not be overwritten by another's trial).
std::mutex g_account_mtx;

struct Account {
    std::string email, sha1;
    bool refused = false;
    bool unconfirmed = false;   // tried by an earlier run that never learnt whether it worked: counts as refused
    bool on_trial = false;      // typed, and not yet accepted by a plug
};

Account read_account(const home::Secrets *s) {
    Account a;
    if (!s) return a;
    const json j = s->get(kFamily);
    a.email = util::jget(j, "email", std::string());
    a.sha1 = util::jget(j, "password_sha1", std::string());
    a.unconfirmed = !util::jget(j, "refused", false) && home::trial::stale(j);   // a refusal kept says more
    a.refused = util::jget(j, "refused", false) || a.unconfirmed;
    a.on_trial = j.contains("trial");
    return a;
}

// Whether `j` still holds the account `a` was read from (it may have been typed again meanwhile).
bool same_account(const json &j, const Account &a) {
    return util::jget(j, "email", std::string()) == a.email && util::jget(j, "password_sha1", std::string()) == a.sha1;
}

tapo::Credentials credentials(const Account &a) {
    tapo::Credentials c;
    c.email = a.email;
    c.password_sha1_hex = a.sha1;
    return c;
}

std::string info_of(const home::Device &d, const char *key) {
    const auto it = d.info.find(key);
    return it == d.info.end() ? std::string() : it->second;
}

// What the transport needs, from what discovery recorded about the plug.
tapo::Device transport(const home::Device &d) {
    tapo::Device t;
    t.ip = d.ip;
    t.port = d.port > 0 ? d.port : 80;
    t.scheme = info_of(d, "scheme");
    // Saved from discovery's answer, which before ZB-093 could nest to any depth: one that cannot be read
    // is not used, and the plug is asked for it again (tapo.h Device::tpap).
    const json tpap = util::parse_untrusted_json(info_of(d, "tpap"));
    if (tpap.is_object()) t.tpap = tpap;
    t.owner = info_of(d, "owner");
    t.mac = d.mac;   // the plug discovery saved: another plug of the account at its address is refused
    return t;
}

std::string upper(std::string s) {
    for (char &c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}

// The kinds of Tapo device switched here, by the device type TP-Link reports ("SMART.TAPOPLUG").
// Hubs, cameras, doorbells and robot cleaners are not exposed at all. Nor are "IOT." types: those
// are Kasa devices that moved to the newer login. Inside it they still take the older protocol's
// commands, not the ones this driver sends (and some use an older form of the login too); the
// Kasa driver claims them (drivers/kasa.h).
std::string type_for(const std::string &device_type) {
    const std::string t = upper(device_type);
    if (t.compare(0, 4, "IOT.") == 0) return "";
    if (t.empty() || t.find("PLUG") != std::string::npos) return "plug";
    if (t.find("SWITCH") != std::string::npos) return "switch";
    if (t.find("BULB") != std::string::npos) return "light";
    return "";
}

// Models that measure the power they pass.
bool meters_energy(const std::string &model) {
    const std::string m = upper(model);
    for (const char *p : {"P110", "P115", "P125M"})
        if (m.compare(0, strlen(p), p) == 0) return true;
    return false;
}

bool only_power(const home::Change &c) {
    return c.on && !c.brightness && !c.kelvin && !c.hs && !c.target_c && !c.mode && !c.position && !c.fan && !c.volume &&
           !c.volume_step && !c.mute && !c.transport && !c.source && !c.key;
}

class TapoDriver : public home::Driver {
public:
    const char *kind() const override { return kFamily; }

    bool claims(const std::string &device_kind, const std::map<std::string, std::string> &info) const override {
        if (device_kind != kFamily) return false;
        const auto it = info.find("device_type");
        return !type_for(it == info.end() ? std::string() : it->second).empty();
    }

    home::PairStep next_step(const home::Device &dev) const override {
        const Account a = read_account(secrets_);
        home::PairStep s;
        if (a.email.empty()) {
            s.kind = home::Pairing::EnterCode;
            s.prompt = "Tapo plugs obey only the Tapo account they were set up with. Type that account's e-mail address.";
            s.field = "Tapo account e-mail";
        } else if (!tapo::owned_by(transport(dev), a.email)) {
            s.kind = home::Pairing::EnterCode;
            s.prompt = "This plug was set up with a different Tapo account from the one saved. Only one account is kept: "
                       "typing this plug's account replaces the saved one, and its password is asked for next.";
            s.field = "Tapo account e-mail";
        } else if (a.sha1.empty() || a.refused) {
            s.kind = home::Pairing::Password;
            s.prompt = a.unconfirmed ? "No plug has confirmed the Tapo password typed last (Zero Bot stopped before it knew), so it "
                                       "is not sent again. Type the Tapo account's password again; capitals count."
                       : a.refused   ? "A Tapo plug did not accept the saved password, so nothing more is sent to the plugs. Type "
                                       "the Tapo account's password again; capitals count."
                                     : "Type the password of the Tapo account. Only a scrambled form of it is kept on this device.";
            s.field = "Tapo account password";
        }
        return s;
    }

    bool pair(home::Device &dev, const std::string &input, std::string &err) override {
        if (!secrets_) { err = "Nothing can be saved."; return false; }
        const home::PairStep step = next_step(dev);
        json j = secrets_->get(kFamily);
        if (step.kind == home::Pairing::EnterCode) {
            const std::string email = util::trim(input);
            if (email.size() < 3 || email.size() > 254 || email.find('@') == std::string::npos) {
                err = "That does not look like an e-mail address.";
                return false;
            }
            if (!tapo::owned_by(transport(dev), email)) {
                // Checked against what the plug announces; nothing is sent to it.
                err = "This plug was set up with a different Tapo account. Check the address; capitals count.";
                return false;
            }
            // Another account starts afresh: the saved password belongs to the old one and is
            // never tried with the new address, so that account's password is asked for next.
            std::lock_guard<std::mutex> lk(g_account_mtx);
            j = secrets_->get(kFamily);
            if (util::jget(j, "email", std::string()) != email) j = {{"email", email}};
            if (!secrets_->put(kFamily, j)) {
                err = "The e-mail address could not be saved on this device; its storage may be full.";
                return false;
            }
            tapo::forget_sessions();
            return true;
        }
        if (step.kind == home::Pairing::Password) {
            if (input.empty()) { err = "Type the password."; return false; }
            {
                // Saved with its trial begun, before the one login that checks it: when that write
                // fails, nothing is sent.
                std::lock_guard<std::mutex> lk(g_account_mtx);
                j = secrets_->get(kFamily);
                j["password_sha1"] = tapo::sha1_hex(input);
                j.erase("refused");
                home::trial::typed(j);
                home::trial::begin(j);
                if (!secrets_->put(kFamily, j)) {
                    err = std::string("The password could not be saved on this device (its storage may be full), so it was not sent "
                                      "to the plug.");
                    return false;
                }
            }
            tapo::forget_sessions();
            // One login to check it. A refusal is kept, so that it is never tried again by itself.
            const Account a = read_account(secrets_);
            const tapo::Device t = transport(dev);
            json info;
            std::string why;
            if (tapo::get_info(t, credentials(a), info, why)) {
                accepted(a);
                return true;
            }
            if (keep_refusal(t, a)) {
                err = "The plug did not accept this password. Capitals count; type it again.";
                return false;
            }
            err = "The password is saved, but the plug did not answer: " + why;
            return false;
        }
        return true;
    }

    bool enumerate(home::Device &dev, std::vector<home::Thing> &out, std::string &err) override {
        out.clear();
        Account a;
        if (!ready(dev, a, err)) return false;
        const tapo::Device t = transport(dev);
        const tapo::Credentials c = credentials(a);
        json info;
        if (!tapo::get_info(t, c, info, err)) { keep_refusal(t, a); return false; }
        accepted(a);
        const std::string name = util::jget(info, "name", std::string());
        if (!name.empty()) dev.name = name;   // the shared layer cleans it
        const std::string model = util::jget(info, "model", std::string());
        if (!model.empty()) dev.model = model;
        std::string device_type = info_of(dev, "device_type");
        if (device_type.empty()) device_type = util::jget(info, "type", std::string());
        const std::string type = type_for(device_type);
        if (type.empty()) { err = "Zero Bot cannot control this kind of Tapo device."; return false; }
        if (util::jget(info, "sockets", 0) > 0) {
            json sockets;
            if (!tapo::list_sockets(t, c, sockets, err)) { keep_refusal(t, a); return false; }
            for (const auto &s : sockets) {
                home::Thing th;
                th.id = util::jget(s, "id", std::string());
                if (th.id.empty()) continue;
                th.name = util::jget(s, "name", std::string());
                th.type = "plug";
                th.caps = home::kPower;
                th.ref = {{"child", th.id}};
                th.state = {{"on", util::jget(s, "on", false)}};
                out.push_back(th);
            }
            return true;
        }
        home::Thing th;
        th.id = "main";
        th.name = name;
        th.type = type;
        th.caps = home::kPower;
        if (meters_energy(dev.model)) th.caps |= home::kSensor;
        th.ref = {{"child", ""}};
        th.state = {{"on", util::jget(info, "on", false)}};
        out.push_back(th);
        return true;
    }

    // Asked for particular things, a plug that measures power is also asked for its readings
    // (a second request); asked for "all of them", only whether each is on.
    bool read(const home::Device &dev, std::vector<home::Thing> &things, std::string &err) override {
        const bool detail = !things.empty();
        if (!detail) things = dev.things;
        Account a;
        if (!ready(dev, a, err)) return false;
        const tapo::Device t = transport(dev);
        const tapo::Credentials c = credentials(a);
        const bool strip = std::any_of(things.begin(), things.end(),
                                       [](const home::Thing &th) { return !util::jget(th.ref, "child", std::string()).empty(); });
        if (strip) {
            json sockets;
            if (!tapo::list_sockets(t, c, sockets, err)) { keep_refusal(t, a); return false; }
            accepted(a);
            for (auto &th : things) {
                const std::string child = util::jget(th.ref, "child", std::string());
                for (const auto &s : sockets)
                    if (util::jget(s, "id", std::string()) == child) th.state = {{"on", util::jget(s, "on", false)}};
            }
            return true;
        }
        json info;
        if (!tapo::get_info(t, c, info, err)) { keep_refusal(t, a); return false; }
        accepted(a);
        json energy;
        std::string ignored;
        const bool metered = detail && std::any_of(things.begin(), things.end(), [](const home::Thing &th) {
            return (th.caps & home::kSensor) != 0;
        }) && tapo::get_energy(t, c, energy, ignored) && energy.is_object();
        for (auto &th : things) {
            th.state = {{"on", util::jget(info, "on", false)}};
            if (metered && (th.caps & home::kSensor)) {
                if (energy.contains("power_w")) th.state["power_w"] = energy["power_w"];
                if (energy.contains("today_wh")) th.state["energy_today_wh"] = energy["today_wh"];
            }
        }
        return true;
    }

    bool apply(const home::Device &dev, const home::Thing &thing, const home::Change &change, int, std::string &err) override {
        if (!only_power(change)) { err = "this plug can only be switched on and off"; return false; }
        Account a;
        if (!ready(dev, a, err)) return false;
        const tapo::Device t = transport(dev);
        const std::string child = util::jget(thing.ref, "child", std::string());
        const bool ok = child.empty() ? tapo::set_power(t, credentials(a), *change.on, err)
                                      : tapo::set_socket_power(t, credentials(a), child, *change.on, err);
        if (!ok) keep_refusal(t, a);
        else accepted(a);
        return ok;
    }

private:
    // Whether the account may be used with this plug at all; nothing is sent when it may not. A
    // password typed and not yet tried has its trial written first (nothing is sent when that fails).
    bool ready(const home::Device &dev, Account &a, std::string &err) {
        a = read_account(secrets_);
        if (a.email.empty() || a.sha1.empty()) {
            err = "the Tapo account is not set up yet; it can be added in Settings, Smart home";
            return false;
        }
        if (a.unconfirmed) {
            err = "no plug has confirmed the Tapo password typed last, so nothing is sent until it is typed again in Settings, "
                  "Smart home, Tapo account";
            return false;
        }
        if (a.refused) {
            err = "a Tapo plug did not accept the saved password, so nothing is sent until it is typed again in Settings, "
                  "Smart home, Tapo account";
            return false;
        }
        if (!tapo::owned_by(transport(dev), a.email)) {
            err = "this plug belongs to a different Tapo account";
            return false;
        }
        if (a.on_trial) {
            std::lock_guard<std::mutex> lk(g_account_mtx);
            json j = secrets_->get(kFamily);
            if (same_account(j, a) && home::trial::begin(j) && !secrets_->put(kFamily, j)) {
                err = kNotSaved;
                return false;
            }
        }
        return true;
    }

    // A plug accepted the account: its password's trial is over. When that cannot be written the
    // trial stays, and the next run asks for the password again: the safe way to be wrong.
    void accepted(const Account &a) {
        if (!secrets_ || !a.on_trial) return;
        std::lock_guard<std::mutex> lk(g_account_mtx);
        json j = secrets_->get(kFamily);
        if (same_account(j, a) && home::trial::end(j)) secrets_->put(kFamily, j);
    }

    // After a failure: when the plug refused the account (or locked itself), remember that in the
    // file, so that no later run tries it again. Returns whether it was a refusal. Should that write
    // fail, a password still on trial counts as refused in the next run all the same.
    bool keep_refusal(const tapo::Device &t, const Account &a) {
        if (!secrets_ || !tapo::login_refused(t, credentials(a))) return false;
        std::lock_guard<std::mutex> lk(g_account_mtx);
        json j = secrets_->get(kFamily);
        if (same_account(j, a)) {
            j["refused"] = true;
            secrets_->put(kFamily, j);
        }
        return true;
    }
};

}  // namespace

std::unique_ptr<home::Driver> make_tapo_driver() { return std::make_unique<TapoDriver>(); }

namespace home {

TapoAccount tapo_account() {
    const json j = secrets().get(kFamily);
    TapoAccount a;
    a.email = util::jget(j, "email", std::string());
    a.password_saved = !util::jget(j, "password_sha1", std::string()).empty();
    a.unconfirmed = !util::jget(j, "refused", false) && trial::stale(j);
    a.refused = util::jget(j, "refused", false) || a.unconfirmed;
    return a;
}

bool set_tapo_account(const std::string &email, const std::string &password) {
    const std::string e = util::trim(email);
    bool saved = false;
    {
        std::lock_guard<std::mutex> lk(g_account_mtx);
        if (e.empty()) {
            saved = secrets().put(kFamily, json::object());
        } else {
            json j = secrets().get(kFamily);
            // A different address is a different account: the old password is not kept for it.
            if (util::jget(j, "email", std::string()) != e) j = {{"email", e}};
            if (!password.empty()) {
                j["password_sha1"] = tapo::sha1_hex(password);
                j.erase("refused");
                trial::typed(j);   // the first login with it writes its trial first (ready())
            }
            saved = secrets().put(kFamily, j);
        }
    }
    tapo::forget_sessions();   // corrected details get a fresh attempt
    return saved;
}

}  // namespace home
