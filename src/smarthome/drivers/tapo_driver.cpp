// TP-Link Tapo plugs and strips on the smart-home driver contract. All protocol work is in
// smarthome/tapo.{h,cpp}; this file maps it onto things, changes and the pairing steps.
//
// A plug locks itself after a few rejected logins, so a login is never retried. Each call to the
// transport says whether the plug refused the account (`refused`); this driver then keeps that for
// every plug of the account in this run's memory (home::trial::refuse: the same-run protection, which
// nothing can fail) and in the family secret ("refused": the durable one, for later runs, which a full
// store can refuse, and then says so). A password just typed is on trial (driver.h home::trial) until a
// plug accepts it: the trial is written before the first login with it, and nothing is sent when that
// write fails, so a refusal that could not be recorded (a full store, the app stopped) still stops a
// later run; for a password a plug had accepted before, such a refusal is held by the memory alone, and
// a later run would try it once more. Its first login is made by one call at a time (g_first_try).
// The account e-mail is checked against the owner a plug announces before any login, and a plug saved
// without a MAC is never switched (kNoIdentity).
#include "smarthome/drivers/tapo_driver.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>

#include "core/util.h"
#include "smarthome/drivers/support.h"
#include "smarthome/home.h"
#include "smarthome/tapo.h"

namespace {

using home::json;

const char *const kFamily = "tapo";
const char *const kNotSaved = "the Tapo account could not be saved on this device (its storage may be full), so nothing was sent "
                              "to the plug";
const char *const kRefusalNotNoted = "; that refusal could not be noted on this device (its storage may be full)";
const char *const kNoIdentity = "no hardware address is saved for this Tapo plug, so Zero Bot cannot make sure that the plug "
                                "at its address is still this one; scan the network again in Settings, Smart home";

// The account is read, changed and written back by the settings page and by every plug's reads at
// once (up to four threads): one change at a time, so that none undoes another (a refusal kept by
// one thread must not be overwritten by another's trial).
std::mutex g_account_mtx;

// A password on trial goes to one plug at a time: the call that may send it takes this before it
// reads the account (ready(), pair_password()) and keeps it until the outcome is kept (accepted(),
// failed()), not through the reads that follow. Every plug of the account would otherwise be sent a
// password that may be wrong at once, up to four of them; the others wait, and then see whether it
// was refused.
std::mutex g_first_try;

struct Account {
    std::string email, sha1;
    // Which typing of a password this is (typed_password()): the outcome of a login still on its way
    // with the same password typed before is not this one's.
    long long generation = 0;
    bool refused = false;
    bool unconfirmed = false;   // tried by an earlier run that never learnt whether it worked: counts as refused
    bool on_trial = false;      // typed, and not yet accepted by a plug
};

// What tells one saved account from another in this run's memory of refusals.
std::string print_of(const Account &a) { return a.email + "\n" + a.sha1 + "\n" + std::to_string(a.generation); }

Account read_account(const home::Secrets *s) {
    Account a;
    if (!s) return a;
    const json j = s->get(kFamily);
    a.email = util::jget(j, "email", std::string());
    a.sha1 = util::jget(j, "password_sha1", std::string());
    a.generation = util::jget(j, "generation", 0LL);
    const bool kept = util::jget(j, "refused", false) || home::trial::refused(kFamily, print_of(a));
    a.unconfirmed = !kept && home::trial::stale(j);   // a refusal kept says more
    a.refused = kept || a.unconfirmed;
    a.on_trial = j.contains("trial");
    return a;
}

// A password just typed, in place of the one before and its refusal: on trial (driver.h), its trial
// begun as well when its first login follows at once (pair_password()). A new generation, even for the
// same password typed again.
void typed_password(json &account, const std::string &password, bool first_login_now) {
    account["password_sha1"] = tapo::sha1_hex(password);
    account["generation"] = util::jget(account, "generation", 0LL) + 1;
    account.erase("refused");
    home::trial::typed(account);
    if (first_login_now) home::trial::begin(account);
}

// The account as saved with `email`. A different address is a different account: the old password is
// not kept for it, but the count of passwords typed is, so that no later one shares a generation with
// one whose login may still be on its way.
json with_email(const json &account, const std::string &email) {
    if (util::jget(account, "email", std::string()) == email) return account;
    json fresh = {{"email", email}};
    if (account.contains("generation")) fresh["generation"] = account["generation"];
    return fresh;
}

// Whether `j` still holds the account `a` was read from (it may have been typed again meanwhile, even
// the same password: a new generation).
bool same_account(const json &j, const Account &a) {
    return util::jget(j, "email", std::string()) == a.email && util::jget(j, "password_sha1", std::string()) == a.sha1 &&
           util::jget(j, "generation", 0LL) == a.generation;
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

// The MAC a plug was saved under: the record's own, else the one its id was made from ("tapo:<MAC>"
// when discovery heard no device id). Twelve zeros, which an earlier version could save, are none.
std::string saved_mac(const home::Device &d) { return support::saved_mac(d, "tapo:"); }

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
    t.mac = saved_mac(d);   // the plug discovery saved: another plug of the account at its address is refused
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
        if (step.kind == home::Pairing::EnterCode) return pair_email(dev, input, err);
        if (step.kind == home::Pairing::Password) return pair_password(dev, input, err);
        return true;
    }

    bool enumerate(home::Device &dev, std::vector<home::Thing> &out, std::string &err) override {
        out.clear();
        Account a;
        std::unique_lock<std::mutex> first;   // held while the password is on trial (ready())
        if (!ready(dev, a, first, err)) return false;
        const tapo::Device t = transport(dev);
        const tapo::Credentials c = credentials(a);
        json info;
        bool refused = false;
        if (!tapo::get_info(t, c, info, err, refused)) { failed(refused, a, first, err); return false; }
        accepted(a, first);   // the outcome, before a strip's sockets are read (one request or more)
        const std::string name = util::jget(info, "name", std::string());
        if (!name.empty()) dev.name = name;   // the shared layer cleans it
        const std::string model = util::jget(info, "model", std::string());
        if (!model.empty()) dev.model = model;
        std::string device_type = info_of(dev, "device_type");
        if (device_type.empty()) device_type = util::jget(info, "type", std::string());
        const std::string type = type_for(device_type);
        if (type.empty()) { err = "Zero Bot cannot control this kind of Tapo device."; return false; }
        if (util::jget(info, "strip", false)) {
            json sockets;
            if (!tapo::list_sockets(t, c, sockets, err, refused)) { failed(refused, a, first, err); return false; }
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
        std::unique_lock<std::mutex> first;   // held while the password is on trial (ready())
        if (!ready(dev, a, first, err)) return false;
        const tapo::Device t = transport(dev);
        const tapo::Credentials c = credentials(a);
        bool refused = false;
        const bool strip = std::any_of(things.begin(), things.end(),
                                       [](const home::Thing &th) { return !util::jget(th.ref, "child", std::string()).empty(); });
        if (strip) {
            json sockets;
            if (!tapo::list_sockets(t, c, sockets, err, refused)) { failed(refused, a, first, err); return false; }
            accepted(a, first);
            for (auto &th : things) {
                const std::string child = util::jget(th.ref, "child", std::string());
                for (const auto &s : sockets)
                    if (util::jget(s, "id", std::string()) == child) th.state = {{"on", util::jget(s, "on", false)}};
            }
            return true;
        }
        json info;
        if (!tapo::get_info(t, c, info, err, refused)) { failed(refused, a, first, err); return false; }
        accepted(a, first);
        json energy;
        std::string why;
        const bool metered = detail && std::any_of(things.begin(), things.end(), [](const home::Thing &th) {
            return (th.caps & home::kSensor) != 0;
        }) && tapo::get_energy(t, c, energy, why, refused) && energy.is_object();
        if (refused) {   // the readings are extra, but a refusal on the way is kept and said as any other is
            err = why;
            failed(refused, a, first, err);
            return false;
        }
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
        // Without a saved MAC the transport has nothing to ask the plug at the address for: another
        // plug of the account given that address would take the login, and be switched instead.
        if (saved_mac(dev).empty()) { err = kNoIdentity; return false; }
        Account a;
        std::unique_lock<std::mutex> first;   // held while the password is on trial (ready())
        if (!ready(dev, a, first, err)) return false;
        const tapo::Device t = transport(dev);
        const std::string child = util::jget(thing.ref, "child", std::string());
        bool refused = false;
        const bool ok = child.empty() ? tapo::set_power(t, credentials(a), *change.on, err, refused)
                                      : tapo::set_socket_power(t, credentials(a), child, *change.on, err, refused);
        if (!ok) failed(refused, a, first, err);
        else accepted(a, first);
        return ok;
    }

private:
    // The e-mail step: checked against the owner the plug announces (nothing is sent to it). Another
    // account starts afresh: the saved password belongs to the old one and is never tried with the
    // new address, so that account's password is asked for next.
    bool pair_email(const home::Device &dev, const std::string &input, std::string &err) {
        const std::string email = util::trim(input);
        if (email.size() < 3 || email.size() > 254 || email.find('@') == std::string::npos) {
            err = "That does not look like an e-mail address.";
            return false;
        }
        if (!tapo::owned_by(transport(dev), email)) {
            err = "This plug was set up with a different Tapo account. Check the address; capitals count.";
            return false;
        }
        std::lock_guard<std::mutex> lk(g_account_mtx);
        if (!secrets_->put(kFamily, with_email(secrets_->get(kFamily), email))) {
            err = "The e-mail address could not be saved on this device; its storage may be full.";
            return false;
        }
        tapo::forget_sessions();
        return true;
    }

    // The password step: saved with its trial begun, then one login to check it, its first try (under
    // g_first_try). When that write fails, nothing is sent; a refusal is kept, so that it is never
    // tried again by itself.
    bool pair_password(const home::Device &dev, const std::string &input, std::string &err) {
        if (input.empty()) { err = "Type the password."; return false; }
        std::unique_lock<std::mutex> first(g_first_try);
        {
            std::lock_guard<std::mutex> lk(g_account_mtx);
            json j = secrets_->get(kFamily);
            typed_password(j, input, true);
            if (!secrets_->put(kFamily, j)) {
                err = "The password could not be saved on this device (its storage may be full), so it was not sent to the plug.";
                return false;
            }
        }
        home::trial::forgive(kFamily);
        tapo::forget_sessions();
        const Account a = read_account(secrets_);
        const tapo::Device t = transport(dev);
        json info;
        std::string why;
        bool refused = false;
        if (tapo::get_info(t, credentials(a), info, why, refused)) {
            accepted(a, first);
            return true;
        }
        const Refusal r = keep_refusal(refused, a);
        if (r == Refusal::None) err = "The password is saved, but the plug did not answer: " + why;
        else err = std::string("The plug did not accept this password. Capitals count; type it again.") +
                   (r == Refusal::NotNoted ? " That refusal could not be noted on this device (its storage may be full)." : "");
        return false;
    }

    // Whether the account may be used with this plug at all; nothing is sent when it may not. Decided
    // under g_first_try, which `first` keeps while the password is on trial: the caller hands it to
    // accepted() or failed(), which let go of it once the outcome is kept. A password typed and not yet
    // tried has its trial written first (nothing is sent when that fails).
    bool ready(const home::Device &dev, Account &a, std::unique_lock<std::mutex> &first, std::string &err) {
        first = std::unique_lock<std::mutex>(g_first_try);
        a = read_account(secrets_);
        if (!a.on_trial) first.unlock();   // a password a plug has taken may go to all of them at once
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

    // A plug accepted the account: its password's trial is over, and so is the first try (`first` is
    // let go): the other plugs may go ahead while this one is read further. When the trial's end cannot
    // be written the trial stays, and the next run asks for the password again: the safe way to be wrong.
    void accepted(const Account &a, std::unique_lock<std::mutex> &first) {
        if (secrets_ && a.on_trial) {
            std::lock_guard<std::mutex> lk(g_account_mtx);
            json j = secrets_->get(kFamily);
            if (same_account(j, a) && home::trial::end(j)) secrets_->put(kFamily, j);
        }
        if (first.owns_lock()) first.unlock();
    }

    enum class Refusal { None, Kept, NotNoted };

    // When the call that sent the account says the plug refused it (or locked itself in answer to it;
    // a plug locked before it was sent anything is no refusal of the account, tapo.h): kept in this
    // run's memory, so that no plug is sent it again in this run whatever the file does, and in the
    // file, so that no later run is either. NotNoted when the file could not take it: a password still
    // on trial counts as refused in the next run all the same (its trial stays), but one a plug had
    // taken before is held by the memory alone, and would be tried once more after a restart. The
    // refusal is the account that was sent (`a`, its generation too): one typed again meanwhile, even the
    // same password, is not marked, and gets the try it was typed for.
    Refusal keep_refusal(bool refused, const Account &a) {
        if (!secrets_ || !refused) return Refusal::None;
        home::trial::refuse(kFamily, print_of(a));
        std::lock_guard<std::mutex> lk(g_account_mtx);
        json j = secrets_->get(kFamily);
        if (!same_account(j, a)) return Refusal::Kept;   // typed again meanwhile: the new one is not refused
        j["refused"] = true;
        return secrets_->put(kFamily, j) ? Refusal::Kept : Refusal::NotNoted;
    }

    // A request failed: a refusal is kept (keep_refusal), `err` says when it could not be noted, and
    // the first try is over.
    void failed(bool refused, const Account &a, std::unique_lock<std::mutex> &first, std::string &err) {
        if (keep_refusal(refused, a) == Refusal::NotNoted) err += kRefusalNotNoted;
        if (first.owns_lock()) first.unlock();
    }
};

}  // namespace

std::unique_ptr<home::Driver> make_tapo_driver() { return std::make_unique<TapoDriver>(); }

namespace home {

TapoAccount tapo_account() {
    const Account read = read_account(&secrets());
    TapoAccount a;
    a.email = read.email;
    a.password_saved = !read.sha1.empty();
    a.unconfirmed = read.unconfirmed;
    a.refused = read.refused;
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
            json j = with_email(secrets().get(kFamily), e);
            if (!password.empty()) typed_password(j, password, false);   // its first login writes its trial first (ready())
            saved = secrets().put(kFamily, j);
        }
    }
    if (saved && !password.empty()) trial::forgive(kFamily);
    tapo::forget_sessions();   // corrected details get a fresh attempt
    return saved;
}

}  // namespace home
