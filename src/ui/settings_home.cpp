// Settings > Your data > Smart home: what the assistant can use, by room; each thing and device;
// pairing; the Tapo account; the scan and the warning before it.
#include "ui/settings_internal.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/result.h"
#include "core/util.h"
#include "smarthome/drivers/tapo_driver.h"
#include "smarthome/home.h"

namespace ui {

// ---------------------------------------------------------------- smart home
namespace {

bool find_home_device(const std::string &network_id, const std::string &device_id, home::Saved &out) {
    for (const auto &d : home::entry(network_id).devices)
        if (d.id == device_id) { out = d; return true; }
    return false;
}

// A change the smart-home file could not take (a full store) is said, not lost without a word.
void say_if_unsaved(Menu &menu, bool saved) {
    if (!saved) menu.problem("Not saved", home::kNotSaved);
}

// What a thing is doing, as last read, for a list.
std::string thing_value(const home::Saved &d, const home::Thing &t) {
    if (!d.enabled || !t.enabled) return "not used";
    const auto on = t.state.find("on");
    if (on != t.state.end() && on->is_boolean()) return on->get<bool>() ? "on" : "off";
    const auto target = t.state.find("target_c");
    if (target != t.state.end() && target->is_number()) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%.1f°C", target->get<double>());
        return buf;
    }
    return t.type;
}

void open_home_device(Menu &menu, const std::string &network_id, const std::string &device_id);
void run_pairing(Menu &menu, const std::string &network_id, const std::string &device_id, const home::PairStep &step);

// Hands what the user typed (or "" after a button) to the device's driver, then carries on with
// the next step, or reads the device once it is ready. The next step is asked for off the UI
// thread too: it looks at the network (home::pairing_step).
void submit_pairing(Menu &menu, const std::string &network_id, const std::string &device_id, const std::string &input) {
    struct Outcome {
        bool ok = false;
        std::string err;
        home::PairStep next;
    };
    menu.toast("Checking…");
    background_on_page(
        menu,
        [network_id, device_id, input] {
            Outcome r;
            r.ok = home::pairing_submit(network_id, device_id, input, r.err);
            if (r.ok) r.next = home::pairing_step(network_id, device_id);
            return r;
        },
        [&menu, network_id, device_id](const Outcome &r) {
            if (!r.ok) { menu.problem("Not set up yet", r.err); return; }
            run_pairing(menu, network_id, device_id, r.next);
        });
}

// The one-time setup a device's driver asks for, one step at a time (`step`, as home::pairing_step
// gave it): press a button, accept a prompt on the device, or type a code, password or token.
void run_pairing(Menu &menu, const std::string &network_id, const std::string &device_id, const home::PairStep &step) {
    switch (step.kind) {
        case home::Pairing::None:
            // Read the device, and the others of its family still waiting: an account typed for
            // one Tapo plug serves every one of them.
            menu.toast("Asking the device…");
            background_on_page(menu, [network_id, device_id] { return home::refresh_after_pairing(network_id, device_id); },
                               [&menu](const std::string &summary) {
                                   menu.toast(summary);
                                   menu.refresh();
                               });
            return;
        case home::Pairing::PressButton:
        case home::Pairing::AcceptPrompt: {
            MenuPage page;
            page.title = "Set up";
            page.build = [&menu, network_id, device_id, step]() {
                std::vector<MenuItem> items;
                Menu::add_paragraph(items, step.prompt);
                const char *done = step.kind == home::Pairing::PressButton ? "I have pressed it" : "I have accepted it";
                items.push_back(MenuItem::action(done, [&menu, network_id, device_id] {
                    menu.pop();
                    submit_pairing(menu, network_id, device_id, "");
                }));
                items.push_back(MenuItem::action("Cancel", [&menu] { menu.pop(); }));
                return items;
            };
            menu.push(std::move(page));
            return;
        }
        default:   // a code, a password or a token
            menu.edit_text(step.field.empty() ? std::string("Code") : step.field, "", step.kind != home::Pairing::EnterCode,
                           [&menu, network_id, device_id](const std::string &text) {
                               if (text.empty()) return;
                               submit_pairing(menu, network_id, device_id, text);
                           },
                           step.prompt);
            return;
    }
}

void open_home_thing(Menu &menu, const std::string &network_id, const std::string &device_id, const std::string &thing_id) {
    MenuPage page;
    page.title = "Home device";
    page.build = [&menu, network_id, device_id, thing_id]() {
        std::vector<MenuItem> items;
        home::Saved d;
        const home::Thing *found = find_home_device(network_id, device_id, d) ? d.thing(thing_id) : nullptr;
        if (!found) { items.push_back(MenuItem::header("This device was removed")); return items; }
        const home::Thing t = *found;
        const auto meta = d.meta.find(thing_id);
        const bool room_typed = meta != d.meta.end() && meta->second.room_by_user;
        // A room taken from the start of the name is shown as a guess for the user to correct.
        const bool guessed = !room_typed && !t.room.empty() && t.room == home::infer_room(t.name);
        items.push_back(MenuItem::val("Called", [t] { return home::spoken(t); }, [&menu, network_id, device_id, thing_id, t] {
            menu.edit_text("What you call it", home::spoken(t), false, [&menu, network_id, device_id, thing_id, t](const std::string &text) {
                const std::string alias = util::trim(text);
                say_if_unsaved(menu, home::set_thing_alias(network_id, device_id, thing_id, alias == t.name ? "" : alias));
            }, "The name to say to the assistant. The maker's app calls it \"" + t.name + "\".");
        }));
        items.push_back(MenuItem::val("Room", [t, guessed] {
            if (t.room.empty()) return std::string("none");
            return guessed ? t.room + " (guessed)" : t.room;
        }, [&menu, network_id, device_id, thing_id, t] {
            menu.edit_text("Room", t.room, false, [&menu, network_id, device_id, thing_id](const std::string &text) {
                say_if_unsaved(menu, home::set_thing_room(network_id, device_id, thing_id, util::trim(text)));
            }, "Lights in a room can be switched together: \"turn off the bedroom\". Leave blank for no room.");
        }));
        items.push_back(MenuItem::val("Assistant may use it", [t] { return on_off(t.enabled); }, nullptr,
                                      [&menu, network_id, device_id, thing_id, t](int) {
                                          say_if_unsaved(menu, home::set_thing_enabled(network_id, device_id, thing_id, !t.enabled));
                                      }));
        if (t.caps & home::kPower) {
            items.push_back(MenuItem::val("Ask before switching on", [t] { return on_off(t.ask_first); }, nullptr,
                                          [&menu, network_id, device_id, thing_id, t](int) {
                                              say_if_unsaved(menu, home::set_thing_ask_first(network_id, device_id, thing_id, !t.ask_first));
                                          }));
            const auto on = t.state.find("on");
            const bool is_on = on != t.state.end() && on->is_boolean() && on->get<bool>();
            items.push_back(MenuItem::val("Switch now", [is_on] { return std::string(is_on ? "On" : "Off"); }, [&menu, network_id, device_id, thing_id, is_on] {
                menu.toast(is_on ? "Switching off…" : "Switching on…");
                background_on_page(menu, [network_id, device_id, thing_id, is_on] { return home::switch_thing(network_id, device_id, thing_id, !is_on); },
                                   [&menu](const std::string &outcome) {
                                       const std::string why = result::words(outcome);   // without its "ERROR: ", when it has one
                                       if (why != outcome) menu.problem("That did not work", why);
                                       else menu.refresh();
                                   });
            }));
        }
        items.push_back(MenuItem::val("Part of", [d] { return d.title(); }, [&menu, network_id, device_id] { open_home_device(menu, network_id, device_id); }));
        return items;
    };
    menu.push(std::move(page));
}

// What a device's driver still needs before it can be used (home::pairing_step), for its page. Asking
// looks at the network the device is on, which can mean the first full look (`iw`, seconds when the
// router is slow to answer), so it is asked in the background each time the page is drawn: the page
// shows the last answer meanwhile (none before the first), and is drawn again when it changed.
struct PairingNeeds {
    std::optional<home::PairStep> step;
    bool asking = false;
};

const home::PairStep *pairing_needs(Menu &menu, const std::shared_ptr<PairingNeeds> &needs, const std::string &network_id,
                                    const std::string &device_id) {
    if (!needs->asking) {
        needs->asking = true;
        const unsigned long page = menu.on_top();   // the device's page, being drawn
        background([network_id, device_id] { return home::pairing_step(network_id, device_id); },
                   [&menu, needs, page](const home::PairStep &step) {
                       needs->asking = false;
                       const bool changed = !needs->step || needs->step->kind != step.kind || needs->step->prompt != step.prompt;
                       needs->step = step;
                       if (changed && menu.showing(page)) menu.refresh();
                   });
    }
    return needs->step ? &*needs->step : nullptr;
}

// What a device the app can control still needs (`step`, nullptr while it is asked), whether the
// assistant may use it, its things, and reading it again or unpairing it.
void controllable_rows(Menu &menu, const std::string &network_id, const std::string &device_id, const home::Saved &d,
                       const home::PairStep *step, std::vector<MenuItem> &items) {
    if (!step) {
        Menu::add_paragraph(items, "Checking whether it needs setting up…");
    } else if (step->kind != home::Pairing::None) {
        Menu::add_paragraph(items, step->prompt.empty() ? "This device needs setting up first." : step->prompt);
        items.push_back(MenuItem::action("Set up…", [&menu, network_id, device_id, now = *step] { run_pairing(menu, network_id, device_id, now); }));
    } else if (!d.problem.empty()) {
        Menu::add_paragraph(items, d.problem);
    }
    items.push_back(MenuItem::val("Assistant may use it", [d] { return on_off(d.enabled); }, nullptr,
                                  [&menu, network_id, device_id, d](int) {
                                      say_if_unsaved(menu, home::set_enabled(network_id, device_id, !d.enabled));
                                  }));
    bool header = false;
    for (const auto &t : d.things) {
        if (!d.listed(t)) continue;
        if (!header) { items.push_back(MenuItem::header("What it controls")); header = true; }
        const std::string thing_id = t.id;
        items.push_back(MenuItem::val(home::spoken(t), [d, t] { return thing_value(d, t); },
                                      [&menu, network_id, device_id, thing_id] { open_home_thing(menu, network_id, device_id, thing_id); }));
    }
    if (step && step->kind == home::Pairing::None)
        items.push_back(MenuItem::action("Read it again", [&menu, network_id, device_id] {
            menu.toast("Asking the device…");
            background_on_page(menu, [network_id, device_id] { return home::refresh(network_id, device_id); },
                               [&menu](const std::string &summary) {
                                   menu.toast(summary);
                                   menu.refresh();
                               });
        }));
    if (!d.auth.empty())
        items.push_back(MenuItem::action("Unpair", [&menu, network_id, device_id] {
            menu.confirm("Unpair this device? It will need setting up again.", "Unpair", [&menu, network_id, device_id] {
                background_on_page(menu, [network_id, device_id] { return home::unpair(network_id, device_id); },
                                   [&menu](const std::string &why) {
                                       if (!why.empty()) menu.problem("Not unpaired", why);
                                       menu.refresh();
                                   });
            });
        }, true));
}

void open_home_device(Menu &menu, const std::string &network_id, const std::string &device_id) {
    auto needs = std::make_shared<PairingNeeds>();
    MenuPage page;
    page.title = "Home device";
    page.build = [&menu, network_id, device_id, needs]() {
        std::vector<MenuItem> items;
        home::Saved d;
        if (!find_home_device(network_id, device_id, d)) { items.push_back(MenuItem::header("This device was removed")); return items; }
        items.push_back(MenuItem::val("Name", [d] { return d.title(); }, nullptr));
        items.push_back(MenuItem::val("Type", [d] { return home::kind_label(d) + (d.model.empty() ? "" : " " + d.model); }, nullptr));
        items.push_back(MenuItem::val("Address", [d] { return d.ip; }, nullptr));
        if (!d.controllable()) Menu::add_paragraph(items, "Found on this network, but Zero Bot cannot control this kind of device yet.");
        else controllable_rows(menu, network_id, device_id, d, pairing_needs(menu, needs, network_id, device_id), items);
        // A device that holds the pairing itself (a HomeKit accessory) stays paired to this app
        // when it is only removed from the list, and then refuses other controllers.
        const std::string remove_question = d.auth.empty() ? "Remove this device?"
                                                           : "Remove this device? It stays paired with Zero Bot: unpair it first to use it with another app.";
        items.push_back(MenuItem::action("Remove from this list", [&menu, network_id, device_id, remove_question] {
            menu.confirm(remove_question, "Remove", [&menu, network_id, device_id] {
                if (!home::remove_device(network_id, device_id)) { say_if_unsaved(menu, false); return; }
                menu.pop();
            });
        }, true));
        return items;
    };
    menu.push(std::move(page));
}

// The account every Tapo plug obeys, kept as the Tapo driver's secret.
void open_tapo_account(Menu &menu, const std::string &network_id) {
    MenuPage page;
    page.title = "Tapo account";
    page.build = [&menu, network_id]() {
        std::vector<MenuItem> items;
        Menu::add_paragraph(items, "Tapo plugs only obey the account they were set up with. The e-mail and a scrambled form of the "
                                   "password are kept on this device and used on your own network only.");
        if (home::tapo_account().unconfirmed)
            Menu::add_paragraph(items, "No plug has confirmed the password typed last (Zero Bot stopped before it knew), so "
                                       "nothing more is sent to the plugs until it is typed again.");
        else if (home::tapo_account().refused)
            Menu::add_paragraph(items, "A plug did not accept the saved password, so nothing more is sent to the plugs until it "
                                       "is typed again.");
        items.push_back(MenuItem::val("E-mail", [] { return or_dash(home::tapo_account().email); }, [&menu] {
            menu.edit_text("Tapo account e-mail", home::tapo_account().email, false,
                           [&menu](const std::string &t) { say_if_unsaved(menu, home::set_tapo_account(util::trim(t), "")); });
        }));
        items.push_back(MenuItem::val("Password", [] {
            const home::TapoAccount a = home::tapo_account();
            return std::string(!a.password_saved ? "not set" : a.unconfirmed ? "not confirmed" : a.refused ? "not accepted" : "saved");
        }, [&menu] {
            menu.edit_text("Tapo account password", "", true, [&menu](const std::string &t) {
                if (t.empty()) return;
                if (home::tapo_account().email.empty()) { menu.toast("Enter the e-mail first"); return; }
                say_if_unsaved(menu, home::set_tapo_account(home::tapo_account().email, t));
            }, "Typed text is shown while you enter it. Only a scrambled form is kept.");
        }));
        items.push_back(MenuItem::action("Check the plugs", [&menu, network_id] {
            if (!home::tapo_account().set()) { menu.toast("Enter the e-mail and password first"); return; }
            menu.toast("Asking the plugs…");
            background_on_page(menu, [network_id] { return home::refresh(network_id); },
                               [&menu](const std::string &summary) { menu.problem("Tapo plugs", summary); });
        }));
        if (!home::tapo_account().email.empty())
            items.push_back(MenuItem::action("Forget the account", [&menu] {
                menu.confirm("Forget the Tapo account on this device?", "Forget",
                             [&menu] { say_if_unsaved(menu, home::set_tapo_account("", "")); });
            }, true));
        return items;
    };
    menu.push(std::move(page));
}

// Starts a scan of `shown`, the network the page showed when the user agreed to it (the warning
// page, or Scan again on a network scanned before). It is that network's scan or none: the home
// layer refuses it once the device is elsewhere, and drops what it heard when the network changed
// while it listened.
void run_home_scan(Menu &menu, const discovery::Network &shown) {
    menu.toast("Looking for devices…");
    background_on_page(
        menu,
        [shown] {
            std::vector<discovery::Device> found;
            std::string err;
            bool heard = false;
            if (!home::scan(shown, found, err, &heard)) return std::string("!") + err;
            if (found.empty())
                return std::string("!") + (heard ? "Nothing answered. This network may keep devices from seeing each other, as guest networks do."
                                                 : "This device could not hear the network's announcements. A firewall may be blocking them.");
            const int added = home::remember_scan(shown, found);
            if (added < 0) return std::string("!") + "The devices found could not be saved. " + home::kNotSaved;
            std::string summary = std::to_string(found.size()) + (found.size() == 1 ? " device found" : " devices found");
            if (added < static_cast<int>(found.size())) summary += ", " + std::to_string(added) + " new";
            // Devices that are already set up are read; the others only learn that they need it.
            return summary + ". " + home::refresh(shown.id());
        },
        [&menu](const std::string &result) {
            if (!result.empty() && result[0] == '!') menu.problem("Nothing found", result.substr(1));
            else { menu.toast(result); menu.refresh(); }
        });
}

void open_home_warning(Menu &menu, const discovery::Network &shown) {
    MenuPage page;
    page.title = "Before you scan";
    page.build = [&menu, shown]() {
        std::vector<MenuItem> items;
        items.push_back(MenuItem::header("Network: " + shown.label()));
        Menu::add_paragraph(items, "Only do this on a network you own. On a hotel, office or shared network it would list other "
                                   "people's devices and may break the rules there.");
        Menu::add_paragraph(items, "The scan asks devices to identify themselves, the way a smart-home app does. It does not log in "
                                   "to anything or change anything.");
        Menu::add_paragraph(items, "What it finds is saved on this device for this network only. When you ask the assistant to "
                                   "control something, the device names are sent to your AI service.");
        items.push_back(MenuItem::action("This is my network: scan it", [&menu, shown] { menu.pop(); run_home_scan(menu, shown); }));
        items.push_back(MenuItem::action("Cancel", [&menu] { menu.pop(); }));
        return items;
    };
    menu.push(std::move(page));
}

// The things the assistant can use on a scanned network, by room (those without one last).
void thing_rows(Menu &menu, const home::NetworkEntry &e, std::vector<MenuItem> &items) {
    struct Row { std::string room, label, device_id, thing_id, value; };
    std::vector<Row> rows;
    for (const auto &d : e.devices) {
        if (!d.controllable() || d.needs_setup) continue;
        for (const auto &t : d.things)
            if (d.listed(t)) rows.push_back({t.room, home::spoken(t), d.id, t.id, thing_value(d, t)});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if (a.room.empty() != b.room.empty()) return b.room.empty();   // things without a room last
        return a.room != b.room ? a.room < b.room : a.label < b.label;
    });
    std::string shown_room = "\x01";
    const std::string id = e.id;
    for (const auto &r : rows) {
        if (r.room != shown_room) { items.push_back(MenuItem::header(r.room.empty() ? "Not in a room" : r.room)); shown_room = r.room; }
        const std::string did = r.device_id, tid = r.thing_id, value = r.value;
        items.push_back(MenuItem::val(r.label, [value] { return value; }, [&menu, id, did, tid] { open_home_thing(menu, id, did, tid); }));
    }
}

// Devices with nothing listed by thing_rows: those waiting to be set up, never read, or whose things
// have all gone from their last answer (a strip or bridge that listed nothing), and those the app
// cannot control. Their page is the only way to read them again or remove them.
void unlisted_rows(Menu &menu, const home::NetworkEntry &e, std::vector<MenuItem> &items) {
    const std::string id = e.id;
    bool header = false;
    for (const auto &d : e.devices) {
        if (!d.controllable() || (!d.needs_setup && d.any_listed())) continue;
        if (!header) { items.push_back(MenuItem::header("To set up")); header = true; }
        const std::string did = d.id;
        items.push_back(MenuItem::val(d.title(), [d] { return std::string(d.needs_setup ? "set up" : "not ready"); },
                                      [&menu, id, did] { open_home_device(menu, id, did); }));
    }
    header = false;
    for (const auto &d : e.devices) {
        if (d.controllable()) continue;
        if (!header) { items.push_back(MenuItem::header("Found, not controllable yet")); header = true; }
        const std::string did = d.id;
        items.push_back(MenuItem::val(d.title(), [d] { return home::kind_label(d); }, [&menu, id, did] { open_home_device(menu, id, did); }));
    }
}

// The Tapo account (when the network has a Tapo device), a new scan, and forgetting the network.
void manage_rows(Menu &menu, const discovery::Network &net, const home::NetworkEntry &e, std::vector<MenuItem> &items) {
    const std::string id = e.id;
    items.push_back(MenuItem::header("Manage"));
    if (std::any_of(e.devices.begin(), e.devices.end(), [](const home::Saved &d) { return d.controllable() && d.kind == "tapo"; }))
        items.push_back(MenuItem::val("Tapo account", [] {
            const home::TapoAccount a = home::tapo_account();
            return a.refused ? std::string("check password") : a.set() ? a.email : std::string("needed");
        }, [&menu, id] { open_tapo_account(menu, id); }));
    items.push_back(MenuItem::action("Scan again", [&menu, net] { run_home_scan(menu, net); }));
    items.push_back(MenuItem::action("Forget this network", [&menu, id] {
        menu.confirm("Forget every device saved for this network?", "Forget", [&menu, id] { say_if_unsaved(menu, home::forget_network(id)); });
    }, true));
}

}  // namespace

// The things the assistant can use, by room; then devices still to be set up; then the rest. The
// network is read off the UI thread when the page opens: the first full look runs `iw` and may wait
// for the router's address, seconds in which keys, redraws and the alarm would stand still.
void open_smart_home(Menu &menu) {
    struct Shown {
        bool known = false;
        discovery::Network net;
    };
    auto shown = std::make_shared<Shown>();
    MenuPage page;
    page.title = "Smart home";
    page.build = [&menu, shown]() {
        std::vector<MenuItem> items;
        if (!shown->known) {
            Menu::add_paragraph(items, "Looking at the network…");
            return items;
        }
        const discovery::Network net = shown->net;
        if (!net.usable) {
            Menu::add_paragraph(items, net.why_not.empty() ? "There is no home network to look at right now." : net.why_not);
            return items;
        }
        const std::string id = net.id();
        items.push_back(MenuItem::header("Network: " + net.label()));
        if (!home::scanned(id)) {
            Menu::add_paragraph(items, "Nothing has been looked for on this network. A scan finds smart plugs, lights and "
                                       "similar devices so the assistant can switch them for you.");
            items.push_back(MenuItem::action("Find devices on this network…", [&menu, net] { open_home_warning(menu, net); }));
            return items;
        }
        const home::NetworkEntry e = home::entry(id);
        thing_rows(menu, e, items);
        unlisted_rows(menu, e, items);
        manage_rows(menu, net, e, items);
        return items;
    };
    menu.push(std::move(page));
    const unsigned long opened = menu.on_top();
    background([] { return home::network_now(); }, [&menu, shown, opened](const discovery::Network &net) {
        shown->net = net;
        shown->known = true;
        if (menu.showing(opened)) menu.refresh();
    });
}

}  // namespace ui
