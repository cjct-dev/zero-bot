// The settings menu tree: assistant models, device options, the user's data and the service.
#include "ui/settings_pages.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <memory>
#include <thread>

#include "agent/llm.h"
#include "core/autostart.h"
#include "core/music_lib.h"
#include "core/radio.h"
#include "core/settings.h"
#include "smarthome/drivers/tapo_driver.h"
#include "smarthome/home.h"
#include "core/store.h"
#include "core/tether.h"
#include "core/timers.h"
#include "core/ui_queue.h"
#include "core/util.h"
#include "platform/audio.h"
#include "platform/display.h"
#include "platform/ir.h"
#include "platform/proc.h"
#include "ui/key_guide.h"

namespace ui {

namespace {

Settings &S() { return Settings::get(); }

std::string or_dash(const std::string &s) { return s.empty() ? "not set" : s; }

std::string masked(const std::string &secret) {
    if (secret.empty()) return "";
    if (secret.size() <= 4) return "••••";
    return "••••" + secret.substr(secret.size() - 4);
}

std::string on_off(bool v) { return v ? "On" : "Off"; }

// Drops "https://" for display; other values pass through unchanged.
std::string short_address(const std::string &v) {
    for (const char *scheme : {"https://", "http://"})
        if (v.compare(0, strlen(scheme), scheme) == 0) return v.substr(strlen(scheme));
    return v;
}

int clampi(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

// Runs slow work off the UI thread, then delivers the result back on it. Work that throws (it
// should not) is dropped rather than taking the app down with its thread.
template <typename Work, typename Done>
void background(Work work, Done done) {
    std::thread([work, done]() {
        try {
            auto result = work();
            uiq::post([done, result]() { done(result); });
        } catch (const std::exception &e) {
            fprintf(stderr, "settings: background work failed: %s\n", e.what());
        } catch (...) {
            fprintf(stderr, "settings: background work failed\n");
        }
    }).detach();
}

// ---------------------------------------------------------------- models
void pick_model(Menu &menu, const std::string &title, const std::string &mode, std::string Settings::*field,
                bool allow_default) {
    menu.toast("Loading models…");
    background(
        [] {
            std::string err;
            auto models = llm::list_models(err);
            return std::make_pair(models, err);
        },
        [&menu, title, mode, field, allow_default](const std::pair<std::vector<llm::ModelInfo>, std::string> &r) {
            if (!menu.open()) return;
            if (!r.second.empty()) { menu.problem("Could not load models", r.second); return; }
            // Services that label what each model is for get a filtered list; others show everything.
            bool labelled = false;
            for (auto &m : r.first) labelled = labelled || !m.mode.empty();
            std::vector<std::string> options;
            const std::string same = "(same as language model)";
            if (allow_default) options.push_back(same);
            for (auto &m : r.first)
                if (!labelled || m.mode == mode) options.push_back(m.id);
            const std::string current = (S().*field).empty() && allow_default ? same : S().*field;
            menu.choose(title, options, current, [field, same](const std::string &pick) {
                S().*field = pick == same ? "" : pick;
                S().save();
            });
        });
}

// ---------------------------------------------------------------- calendar
const int kRemindSteps[] = {-1, 0, 5, 10, 15, 30, 60, 120, 1440};

// All-day events count their alert from 09:00 on the day (see alert_base).
std::string remind_text(int minutes, bool all_day = false) {
    if (minutes < 0) return "No alert";
    if (all_day) {
        if (minutes == 0) return "09:00 on the day";
        if (minutes >= 1440) return "09:00 the day before";
        if (minutes >= 60) return std::to_string(9 - minutes / 60) + ":00 on the day";
        return std::to_string(minutes) + " min before 09:00";
    }
    if (minutes == 0) return "At start";
    if (minutes >= 1440) return "1 day before";
    if (minutes >= 60) return std::to_string(minutes / 60) + " h before";
    return std::to_string(minutes) + " min before";
}

const char *const kRepeatSteps[] = {"", "daily", "weekly", "monthly", "yearly"};

std::string repeat_text(const std::string &repeat) {
    if (repeat == "daily") return "Every day";
    if (repeat == "weekly") return "Every week";
    if (repeat == "monthly") return "Every month";
    if (repeat == "yearly") return "Every year";
    return "Never";
}

time_t midnight(time_t t) {
    const util::LocalTime f = util::local_fields(t);
    return util::make_local(f.year, f.month, f.day, 0, 0);
}

bool find_event(int id, CalEvent &out) {
    for (auto &e : Store::get().events())
        if (e.id == id) { out = e; return true; }
    return false;
}

void open_event(Menu &menu, int id) {
    MenuPage page;
    page.title = "Event";
    page.build = [&menu, id]() {
        std::vector<MenuItem> items;
        CalEvent e;
        if (!find_event(id, e)) { items.push_back(MenuItem::header("This event was deleted")); return items; }
        items.push_back(MenuItem::val("Title", [e] { return e.title; }, [&menu, e] {
            menu.edit_text("Event title", e.title, false, [e](const std::string &t) mutable {
                if (util::trim(t).empty()) return;
                e.title = util::trim(t);
                Store::get().update_event(e);
            });
        }));
        items.push_back(MenuItem::val(e.repeat.empty() ? "Starts" : "First on",
                                      [e] { return e.all_day ? util::short_day(e.start) : util::short_date(e.start); }, [&menu, e] {
            menu.edit_text(e.all_day ? "Date" : "Start time", e.all_day ? util::typed_day(e.start) : util::typed_date(e.start), false,
                           [&menu, e](const std::string &t) mutable {
                               time_t when;
                               if (!util::parse_typed(t, when)) {
                                   menu.toast("Use the form " + (e.all_day ? util::typed_day(e.start) : util::typed_date(e.start)));
                                   return;
                               }
                               if (e.all_day) when = midnight(when);
                               if (e.end) e.end += when - e.start;
                               e.start = when;
                               Store::get().update_event(e);
                           },
                           e.all_day ? (util::month_first() ? "Month/day/year" : "Day/month/year") : util::typed_date_hint());
        }));
        if (!e.repeat.empty()) {
            const time_t next = next_occurrence(e, midnight(time(nullptr)));
            if (next && next != e.start)
                items.push_back(MenuItem::val("Next", [e, next] { return e.all_day ? util::short_day(next) : util::short_date(next); }, nullptr));
        }
        items.push_back(MenuItem::val("All day", [e] { return on_off(e.all_day); }, nullptr, [e](int) mutable {
            e.all_day = !e.all_day;
            if (e.all_day) { e.start = midnight(e.start); e.end = 0; }
            else e.start += 9 * 3600;   // a sensible hour to adjust from
            Store::get().update_event(e);
        }));
        items.push_back(MenuItem::val("Repeats", [e] { return repeat_text(e.repeat); }, nullptr, [e](int dir) mutable {
            const int n = sizeof(kRepeatSteps) / sizeof(kRepeatSteps[0]);
            int idx = 0;
            for (int i = 0; i < n; ++i)
                if (e.repeat == kRepeatSteps[i]) idx = i;
            e.repeat = kRepeatSteps[((idx + dir) % n + n) % n];
            Store::get().update_event(e);
        }));
        if (!e.all_day)
            items.push_back(MenuItem::val("Ends", [e] { return e.end ? util::short_date(e.end) : std::string("not set"); },
                                          [&menu, e] {
                menu.edit_text("End time (blank for none)", e.end ? util::typed_date(e.end) : "", false,
                               [&menu, e](const std::string &t) mutable {
                                   time_t when = 0;
                                   if (!util::trim(t).empty() && !util::parse_typed(t, when)) { menu.toast("Use the form " + util::typed_date(e.start + 3600)); return; }
                                   e.end = when;
                                   Store::get().update_event(e);
                               },
                               util::typed_date_hint());
            }));
        items.push_back(MenuItem::val("Notes", [e] { return e.notes; }, [&menu, e] {
            menu.edit_text("Notes", e.notes, false, [e](const std::string &t) mutable {
                e.notes = util::trim(t);
                Store::get().update_event(e);
            });
        }));
        items.push_back(MenuItem::val("Alert", [e] { return remind_text(e.remind_min, e.all_day); }, nullptr, [e](int dir) mutable {
            const int n = sizeof(kRemindSteps) / sizeof(kRemindSteps[0]);
            int idx = 0;
            for (int i = 0; i < n; ++i)
                if (kRemindSteps[i] <= e.remind_min) idx = i;
            e.remind_min = kRemindSteps[clampi(idx + dir, 0, n - 1)];
            Store::get().update_event(e);
        }));
        items.push_back(MenuItem::action("Delete event", [&menu, id] {
            menu.confirm("Delete this event?", "Delete", [&menu, id] { Store::get().delete_event(id); menu.pop(); });
        }, true));
        return items;
    };
    menu.push(std::move(page));
}

void open_calendar(Menu &menu) {
    MenuPage page;
    page.title = "Calendar";
    page.build = [&menu]() {
        std::vector<MenuItem> items;
        const auto events = Store::get().events();
        const time_t now = time(nullptr);
        // Each event once, placed by when it next happens; what is over goes underneath.
        struct Row { CalEvent e; time_t when; bool past; };
        std::vector<Row> rows;
        for (auto &e : events) {
            const time_t next = next_occurrence(e, e.all_day ? midnight(now) : now - 3600);
            const bool running = !next && e.end && e.end >= now - 3600;
            rows.push_back({e, next ? next : e.start, !next && !running});
        }
        std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
            return a.past != b.past ? !a.past : a.past ? a.when > b.when : a.when < b.when;
        });
        bool past_header = false, future_header = false;
        for (auto &row : rows) {
            if (!row.past && !future_header) { items.push_back(MenuItem::header("Upcoming")); future_header = true; }
            if (row.past && !past_header) { items.push_back(MenuItem::header("Past")); past_header = true; }
            const int id = row.e.id;
            const time_t when = row.when;
            const bool all_day = row.e.all_day;
            items.push_back(MenuItem::val(row.e.title,
                                          [when, all_day] { return all_day ? util::friendly_day(when) : util::friendly_when(when); },
                                          [&menu, id] { open_event(menu, id); }));
        }
        if (events.empty()) items.push_back(MenuItem::header("No events yet. Ask the assistant to add one."));
        items.push_back(MenuItem::header("Manage"));
        items.push_back(MenuItem::action("New event…", [&menu] {
            menu.edit_text("New event title", "", false, [&menu](const std::string &t) {
                if (util::trim(t).empty()) return;
                CalEvent e;
                e.title = util::trim(t);
                // Default to the next full hour; the user adjusts it on the event page.
                e.start = (time(nullptr) / 3600 + 1) * 3600;
                const int id = Store::get().add_event(e);
                open_event(menu, id);
            });
        }));
        if (!events.empty())
            items.push_back(MenuItem::action("Delete all events", [&menu] {
                menu.confirm("Delete every calendar event?", "Delete all", [] { Store::get().clear_events(); });
            }, true));
        return items;
    };
    menu.push(std::move(page));
}

// ---------------------------------------------------------------- reminders and timers
// "4:59 left", counting down while the page is open.
std::string time_left(time_t end) {
    const long left = static_cast<long>(end - timers::now());
    return left > 0 ? timers::clock_text(left) + " left" : std::string("ended");
}

void open_reminders(Menu &menu) {
    MenuPage page;
    page.title = "Reminders and timers";
    page.live = true;   // each timer's time left counts down
    page.revision = [] { return Store::get().timers_revision(); };   // and one that ends leaves the list
    page.build = [&menu]() {
        std::vector<MenuItem> items;
        const auto running = Store::get().timers();
        if (!running.empty()) {
            items.push_back(MenuItem::header("Timers"));
            for (auto &t : running) {
                const int id = t.id;
                const time_t end = t.due;
                // The question names the timer: the rows move when one ends, so it says which one goes.
                const std::string question = "Cancel " + util::truncate_utf8(t.text, 30) + "?";
                items.push_back(MenuItem::val(t.text, [end] { return time_left(end); }, [&menu, id, question] {
                    menu.confirm(question, "Cancel timer", [id] { Store::get().delete_timer(id); });
                }));
            }
            items.push_back(MenuItem::action("Cancel all timers", [&menu] {
                menu.confirm("Cancel every timer?", "Cancel all timers", [] { Store::get().clear_timers(); });
            }, true));
            items.push_back(MenuItem::header("Reminders"));
        }
        const auto list = Store::get().reminders();
        for (auto &r : list) {
            const int id = r.id;
            items.push_back(MenuItem::val(r.text.empty() ? "Timer" : r.text, [r] { return util::friendly_when(r.due); }, [&menu, id] {
                menu.confirm("Cancel this reminder?", "Cancel reminder", [id] { Store::get().delete_reminder(id); });
            }));
        }
        if (list.empty())
            items.push_back(MenuItem::header(running.empty() ? "Nothing pending. Say \"set a timer for ten minutes\"" : "None pending"));
        else
            items.push_back(MenuItem::action(running.empty() ? "Cancel all" : "Cancel all reminders", [&menu] {
                menu.confirm("Cancel every reminder?", "Cancel all", [] { Store::get().clear_reminders(); });
            }, true));
        return items;
    };
    menu.push(std::move(page));
}

// ---------------------------------------------------------------- memory
void open_memory_item(Menu &menu, int id) {
    MenuPage page;
    page.title = "Memory";
    page.build = [&menu, id]() {
        std::vector<MenuItem> items;
        std::string text;
        for (auto &m : Store::get().memories())
            if (m.id == id) text = m.text;
        if (text.empty()) { items.push_back(MenuItem::header("This memory was deleted")); return items; }
        // Show the whole text wrapped over a few rows, since rows are a single line.
        size_t pos = 0;
        while (pos < text.size()) {
            size_t len = std::min<size_t>(46, text.size() - pos);
            if (pos + len < text.size()) {
                const size_t space = text.rfind(' ', pos + len);
                if (space != std::string::npos && space > pos) len = space - pos + 1;
            }
            items.push_back(MenuItem::header(text.substr(pos, len)));
            pos += len;
        }
        items.push_back(MenuItem::action("Edit", [&menu, id, text] {
            menu.edit_text("Edit memory", text, false, [id](const std::string &t) {
                if (!util::trim(t).empty()) Store::get().update_memory(id, util::trim(t));
            });
        }));
        items.push_back(MenuItem::action("Delete", [&menu, id] {
            menu.confirm("Delete this memory?", "Delete", [&menu, id] { Store::get().delete_memory(id); menu.pop(); });
        }, true));
        return items;
    };
    menu.push(std::move(page));
}

void open_memory(Menu &menu) {
    MenuPage page;
    page.title = "Memory";
    page.build = [&menu]() {
        std::vector<MenuItem> items;
        const auto list = Store::get().memories();
        for (auto &m : list) {
            const int id = m.id;
            items.push_back(MenuItem::action(m.text, [&menu, id] { open_memory_item(menu, id); }));
        }
        if (list.empty()) items.push_back(MenuItem::header("Nothing remembered yet. Say \"remember that…\""));
        items.push_back(MenuItem::header("Manage"));
        items.push_back(MenuItem::action("Add a memory…", [&menu] {
            menu.edit_text("Something to remember", "", false, [](const std::string &t) {
                if (!util::trim(t).empty()) Store::get().add_memory(util::trim(t));
            });
        }));
        if (!list.empty())
            items.push_back(MenuItem::action("Forget everything", [&menu] {
                menu.confirm("Delete every memory?", "Forget everything", [] { Store::get().clear_memories(); });
            }, true));
        return items;
    };
    menu.push(std::move(page));
}

// ---------------------------------------------------------------- TV remote
void open_remote(Menu &menu) {
    MenuPage page;
    page.title = "TV remote";
    page.build = [&menu]() {
        std::vector<MenuItem> items;
        items.push_back(MenuItem::val("TV brand", [] { return S().tv_brand == "auto" ? std::string("Any (tries all)") : S().tv_brand; }, [&menu] {
            menu.choose("TV brand", ir::brands(), S().tv_brand, [](const std::string &b) { S().tv_brand = b; S().save(); });
        }));
        items.push_back(MenuItem::action("Test: send power", [&menu] {
            const std::string err = ir::send(S().tv_brand, "power", 1);
            menu.toast(err.empty() ? "Power sent" : err);
        }));
        items.push_back(MenuItem::header("Buttons learned from your remote"));
        const auto names = ir::learned();
        for (auto &name : names)
            items.push_back(MenuItem::val(name, [] { return std::string("ENTER to forget"); }, [&menu, name] {
                menu.confirm("Forget '" + name + "'?", "Forget", [name] { ir::forget(name); });
            }));
        items.push_back(MenuItem::action("Learn a button…", [&menu] {
            menu.edit_text("Button name", "", false, [&menu](const std::string &name) {
                if (util::trim(name).empty()) return;
                menu.toast("Point the remote at the top edge and press the button");
                background([name] { return ir::learn(name, 8000); },
                           [&menu, name](const std::string &err) {
                               menu.toast(err.empty() ? "Learned '" + name + "'" : err);
                               menu.refresh();
                           });
            }, "Use a standard name (power, volume_up, mute, input…) to override the built-in code, or any name of your own.");
        }));
        if (!names.empty())
            items.push_back(MenuItem::action("Forget all learned buttons", [&menu] {
                menu.confirm("Forget every learned button?", "Forget all", [] { ir::forget_all(); });
            }, true));
        return items;
    };
    menu.push(std::move(page));
}

// ---------------------------------------------------------------- radio
// The stations are the user's own file. This page shows what the app made of it, plays a station
// to try it, and says where the file is and what goes in it.
void open_radio(Menu &menu) {
    MenuPage page;
    page.title = "Radio stations";
    page.build = [&menu]() {
        std::vector<MenuItem> items;
        const std::vector<radio::Station> list = radio::stations();
        const std::string problem = radio::problem();
        if (!problem.empty()) {
            items.push_back(MenuItem::header("Problem with the file"));
            Menu::add_paragraph(items, problem + ".");
        }
        if (!list.empty()) items.push_back(MenuItem::header("ENTER plays a station"));
        for (const radio::Station &station : list) {
            const radio::Station s = station;
            items.push_back(MenuItem::val(s.name, [s] { return s.url_low.empty() ? std::string() : std::string("low data too"); }, [&menu, s] {
                audio::Music::get().play_live(radio::stream_for(s, radio::metered_link()), s.name);
                menu.toast("Playing " + s.name);
                menu.refresh();
            }));
        }
        if (audio::Music::get().live())
            items.push_back(MenuItem::action("Stop the radio", [&menu] {
                audio::Music::get().stop();
                menu.toast("Radio stopped");
                menu.refresh();
            }));
        items.push_back(MenuItem::header(list.empty() ? "No stations yet" : "Adding stations"));
        Menu::add_paragraph(items, "Add them to " + radio::shown_path() + ": a name, nicknames and a stream address each, "
                                   "and a low-data one if there is. The README shows how; radio-browser.info lists addresses.");
        // Also what makes the paragraph above reachable: the cursor stops at the last row it can select.
        items.push_back(MenuItem::action("Read the file again", [&menu] {
            menu.refresh();
            const size_t n = radio::stations().size();
            menu.toast(n ? std::to_string(n) + (n == 1 ? " station" : " stations") : std::string("No stations in the file"));
        }));
        return items;
    };
    menu.push(std::move(page));
}

// ---------------------------------------------------------------- about
int read_int(const char *path, int fallback) {
    std::ifstream f(path);
    int v;
    return f >> v ? v : fallback;
}

// ---------------------------------------------------------------- smart home
bool find_home_device(const std::string &network_id, const std::string &device_id, home::Saved &out) {
    for (const auto &d : home::entry(network_id).devices)
        if (d.id == device_id) { out = d; return true; }
    return false;
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
void run_pairing(Menu &menu, const std::string &network_id, const std::string &device_id);

// Hands what the user typed (or "" after a button) to the device's driver, then carries on with
// the next step, or reads the device once it is ready.
void submit_pairing(Menu &menu, const std::string &network_id, const std::string &device_id, const std::string &input) {
    menu.toast("Checking…");
    background(
        [network_id, device_id, input] {
            std::string err;
            const bool ok = home::pairing_submit(network_id, device_id, input, err);
            return std::make_pair(ok, err);
        },
        [&menu, network_id, device_id](const std::pair<bool, std::string> &r) {
            if (!menu.open()) return;
            if (!r.first) { menu.problem("Not set up yet", r.second); return; }
            run_pairing(menu, network_id, device_id);
        });
}

// The one-time setup a device's driver asks for, one step at a time: press a button, accept a
// prompt on the device, or type a code, password or token.
void run_pairing(Menu &menu, const std::string &network_id, const std::string &device_id) {
    const home::PairStep step = home::pairing_step(network_id, device_id);
    switch (step.kind) {
        case home::Pairing::None:
            // Read the device, and the others of its family still waiting: an account typed for
            // one Tapo plug serves every one of them.
            menu.toast("Asking the device…");
            background([network_id, device_id] { return home::refresh_after_pairing(network_id, device_id); },
                       [&menu](const std::string &summary) {
                           if (!menu.open()) return;
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
            menu.edit_text("What you call it", home::spoken(t), false, [network_id, device_id, thing_id, t](const std::string &text) {
                const std::string alias = util::trim(text);
                home::set_thing_alias(network_id, device_id, thing_id, alias == t.name ? "" : alias);
            }, "The name to say to the assistant. The maker's app calls it \"" + t.name + "\".");
        }));
        items.push_back(MenuItem::val("Room", [t, guessed] {
            if (t.room.empty()) return std::string("none");
            return guessed ? t.room + " (guessed)" : t.room;
        }, [&menu, network_id, device_id, thing_id, t] {
            menu.edit_text("Room", t.room, false, [network_id, device_id, thing_id](const std::string &text) {
                home::set_thing_room(network_id, device_id, thing_id, util::trim(text));
            }, "Lights in a room can be switched together: \"turn off the bedroom\". Leave blank for no room.");
        }));
        items.push_back(MenuItem::val("Assistant may use it", [t] { return on_off(t.enabled); }, nullptr,
                                      [network_id, device_id, thing_id, t](int) { home::set_thing_enabled(network_id, device_id, thing_id, !t.enabled); }));
        if (t.caps & home::kPower) {
            items.push_back(MenuItem::val("Ask before switching on", [t] { return on_off(t.ask_first); }, nullptr,
                                          [network_id, device_id, thing_id, t](int) { home::set_thing_ask_first(network_id, device_id, thing_id, !t.ask_first); }));
            const auto on = t.state.find("on");
            const bool is_on = on != t.state.end() && on->is_boolean() && on->get<bool>();
            items.push_back(MenuItem::val("Switch now", [is_on] { return std::string(is_on ? "On" : "Off"); }, [&menu, network_id, device_id, thing_id, is_on] {
                menu.toast(is_on ? "Switching off…" : "Switching on…");
                background([network_id, device_id, thing_id, is_on] { return home::switch_thing(network_id, device_id, thing_id, !is_on); },
                           [&menu](const std::string &result) {
                               if (!menu.open()) return;
                               if (result.compare(0, 7, "ERROR: ") == 0) menu.problem("That did not work", result.substr(7));
                               else menu.refresh();
                           });
            }));
        }
        items.push_back(MenuItem::val("Part of", [d] { return d.title(); }, [&menu, network_id, device_id] { open_home_device(menu, network_id, device_id); }));
        return items;
    };
    menu.push(std::move(page));
}

void open_home_device(Menu &menu, const std::string &network_id, const std::string &device_id) {
    MenuPage page;
    page.title = "Home device";
    page.build = [&menu, network_id, device_id]() {
        std::vector<MenuItem> items;
        home::Saved d;
        if (!find_home_device(network_id, device_id, d)) { items.push_back(MenuItem::header("This device was removed")); return items; }
        items.push_back(MenuItem::val("Name", [d] { return d.title(); }, nullptr));
        items.push_back(MenuItem::val("Type", [d] { return home::kind_label(d) + (d.model.empty() ? "" : " " + d.model); }, nullptr));
        items.push_back(MenuItem::val("Address", [d] { return d.ip; }, nullptr));
        if (!d.controllable()) {
            Menu::add_paragraph(items, "Found on this network, but Zero Bot cannot control this kind of device yet.");
        } else {
            const home::PairStep step = home::pairing_step(network_id, device_id);
            if (step.kind != home::Pairing::None) {
                Menu::add_paragraph(items, step.prompt.empty() ? "This device needs setting up first." : step.prompt);
                items.push_back(MenuItem::action("Set up…", [&menu, network_id, device_id] { run_pairing(menu, network_id, device_id); }));
            } else if (!d.problem.empty()) {
                Menu::add_paragraph(items, d.problem);
            }
            items.push_back(MenuItem::val("Assistant may use it", [d] { return on_off(d.enabled); }, nullptr,
                                          [network_id, device_id, d](int) { home::set_enabled(network_id, device_id, !d.enabled); }));
            bool header = false;
            for (const auto &t : d.things) {
                if (!d.listed(t)) continue;
                if (!header) { items.push_back(MenuItem::header("What it controls")); header = true; }
                const std::string thing_id = t.id;
                items.push_back(MenuItem::val(home::spoken(t), [d, t] { return thing_value(d, t); },
                                              [&menu, network_id, device_id, thing_id] { open_home_thing(menu, network_id, device_id, thing_id); }));
            }
            if (step.kind == home::Pairing::None)
                items.push_back(MenuItem::action("Read it again", [&menu, network_id, device_id] {
                    menu.toast("Asking the device…");
                    background([network_id, device_id] { return home::refresh(network_id, device_id); },
                               [&menu](const std::string &summary) {
                                   if (!menu.open()) return;
                                   menu.toast(summary);
                                   menu.refresh();
                               });
                }));
            if (!d.auth.empty())
                items.push_back(MenuItem::action("Unpair", [&menu, network_id, device_id] {
                    menu.confirm("Unpair this device? It will need setting up again.", "Unpair", [&menu, network_id, device_id] {
                        background([network_id, device_id] { return home::unpair(network_id, device_id); },
                                   [&menu](const std::string &why) {
                                       if (!menu.open()) return;
                                       if (!why.empty()) menu.problem("Not unpaired", why);
                                       menu.refresh();
                                   });
                    });
                }, true));
        }
        // A device that holds the pairing itself (a HomeKit accessory) stays paired to this app
        // when it is only removed from the list, and then refuses other controllers.
        const std::string remove_question = d.auth.empty() ? "Remove this device?"
                                                           : "Remove this device? It stays paired with Zero Bot: unpair it first to use it with another app.";
        items.push_back(MenuItem::action("Remove from this list", [&menu, network_id, device_id, remove_question] {
            menu.confirm(remove_question, "Remove", [&menu, network_id, device_id] { home::remove_device(network_id, device_id); menu.pop(); });
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
        if (home::tapo_account().refused)
            Menu::add_paragraph(items, "A plug did not accept the saved password, so nothing more is sent to the plugs until it "
                                       "is typed again.");
        items.push_back(MenuItem::val("E-mail", [] { return or_dash(home::tapo_account().email); }, [&menu] {
            menu.edit_text("Tapo account e-mail", home::tapo_account().email, false,
                           [](const std::string &t) { home::set_tapo_account(util::trim(t), ""); });
        }));
        items.push_back(MenuItem::val("Password", [] {
            const home::TapoAccount a = home::tapo_account();
            return std::string(!a.password_saved ? "not set" : a.refused ? "not accepted" : "saved");
        }, [&menu] {
            menu.edit_text("Tapo account password", "", true, [&menu](const std::string &t) {
                if (t.empty()) return;
                if (home::tapo_account().email.empty()) { menu.toast("Enter the e-mail first"); return; }
                home::set_tapo_account(home::tapo_account().email, t);
            }, "Typed text is shown while you enter it. Only a scrambled form is kept.");
        }));
        items.push_back(MenuItem::action("Check the plugs", [&menu, network_id] {
            if (!home::tapo_account().set()) { menu.toast("Enter the e-mail and password first"); return; }
            menu.toast("Asking the plugs…");
            background([network_id] { return home::refresh(network_id); },
                       [&menu](const std::string &summary) { if (menu.open()) menu.problem("Tapo plugs", summary); });
        }));
        if (!home::tapo_account().email.empty())
            items.push_back(MenuItem::action("Forget the account", [&menu] {
                menu.confirm("Forget the Tapo account on this device?", "Forget", [] { home::set_tapo_account("", ""); });
            }, true));
        return items;
    };
    menu.push(std::move(page));
}

// Starts a scan after the user has agreed to it on the warning page.
void run_home_scan(Menu &menu) {
    menu.toast("Looking for devices…");
    background(
        [] {
            const discovery::Network net = home::network_now(true);
            std::vector<discovery::Device> found;
            std::string err;
            bool heard = false;
            if (!home::scan(found, err, &heard)) return std::string("!") + err;
            if (found.empty())
                return std::string("!") + (heard ? "Nothing answered. This network may keep devices from seeing each other, as guest networks do."
                                                 : "This device could not hear the network's announcements. A firewall may be blocking them.");
            const int added = home::remember_scan(net, found);
            std::string summary = std::to_string(found.size()) + (found.size() == 1 ? " device found" : " devices found");
            if (added < static_cast<int>(found.size())) summary += ", " + std::to_string(added) + " new";
            // Devices that are already set up are read; the others only learn that they need it.
            return summary + ". " + home::refresh(net.id());
        },
        [&menu](const std::string &result) {
            if (!menu.open()) return;
            if (!result.empty() && result[0] == '!') menu.problem("Nothing found", result.substr(1));
            else { menu.toast(result); menu.refresh(); }
        });
}

void open_home_warning(Menu &menu) {
    MenuPage page;
    page.title = "Before you scan";
    page.build = [&menu]() {
        std::vector<MenuItem> items;
        Menu::add_paragraph(items, "Only do this on a network you own. On a hotel, office or shared network it would list other "
                                   "people's devices and may break the rules there.");
        Menu::add_paragraph(items, "The scan asks devices to identify themselves, the way a smart-home app does. It does not log in "
                                   "to anything or change anything.");
        Menu::add_paragraph(items, "What it finds is saved on this device for this network only. When you ask the assistant to "
                                   "control something, the device names are sent to your AI service.");
        items.push_back(MenuItem::action("This is my network: scan it", [&menu] { menu.pop(); run_home_scan(menu); }));
        items.push_back(MenuItem::action("Cancel", [&menu] { menu.pop(); }));
        return items;
    };
    menu.push(std::move(page));
}

// The things the assistant can use, by room; then devices still to be set up; then the rest.
void open_smart_home(Menu &menu) {
    MenuPage page;
    page.title = "Smart home";
    page.build = [&menu]() {
        std::vector<MenuItem> items;
        const discovery::Network net = home::network_now();
        if (!net.usable) {
            Menu::add_paragraph(items, net.why_not.empty() ? "There is no home network to look at right now." : net.why_not);
            return items;
        }
        const std::string id = net.id();
        items.push_back(MenuItem::header("Network: " + net.label()));
        if (!home::scanned(id)) {
            Menu::add_paragraph(items, "Nothing has been looked for on this network. A scan finds smart plugs, lights and "
                                       "similar devices so the assistant can switch them for you.");
            items.push_back(MenuItem::action("Find devices on this network…", [&menu] { open_home_warning(menu); }));
            return items;
        }
        const home::NetworkEntry e = home::entry(id);
        struct Row { std::string room, label, device_id, thing_id, value; };
        std::vector<Row> rows;
        bool any_tapo = false;
        for (const auto &d : e.devices) {
            if (!d.controllable()) continue;
            any_tapo = any_tapo || d.kind == "tapo";
            if (d.needs_setup) continue;
            for (const auto &t : d.things)
                if (d.listed(t)) rows.push_back({t.room, home::spoken(t), d.id, t.id, thing_value(d, t)});
        }
        std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
            if (a.room.empty() != b.room.empty()) return b.room.empty();   // things without a room last
            return a.room != b.room ? a.room < b.room : a.label < b.label;
        });
        std::string shown_room = "\x01";
        for (const auto &r : rows) {
            if (r.room != shown_room) { items.push_back(MenuItem::header(r.room.empty() ? "Not in a room" : r.room)); shown_room = r.room; }
            const std::string did = r.device_id, tid = r.thing_id, value = r.value;
            items.push_back(MenuItem::val(r.label, [value] { return value; }, [&menu, id, did, tid] { open_home_thing(menu, id, did, tid); }));
        }
        // Devices with nothing listed above: those waiting to be set up, never read, or whose
        // things have all gone from their last answer (a strip or bridge that listed nothing).
        // Their page is the only way to read them again or remove them.
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
        items.push_back(MenuItem::header("Manage"));
        if (any_tapo)
            items.push_back(MenuItem::val("Tapo account", [] {
                const home::TapoAccount a = home::tapo_account();
                return a.refused ? std::string("check password") : a.set() ? a.email : std::string("needed");
            }, [&menu, id] { open_tapo_account(menu, id); }));
        items.push_back(MenuItem::action("Scan again", [&menu] { run_home_scan(menu); }));
        items.push_back(MenuItem::action("Forget this network", [&menu, id] {
            menu.confirm("Forget every device saved for this network?", "Forget", [id] { home::forget_network(id); });
        }, true));
        return items;
    };
    menu.push(std::move(page));
}

void open_status(Menu &menu) {
    menu.toast("Checking…");
    background(
        [] {
            std::string text = "Zero Bot " ZB_VERSION "\n";
            const proc::Result wifi = proc::run({"nmcli", "-t", "-f", "ACTIVE,SSID,SIGNAL", "dev", "wifi"}, "", 5000);
            std::string net = "Wi-Fi: not connected";
            for (auto &line : util::split(wifi.out, '\n'))
                if (line.compare(0, 4, "yes:") == 0) {
                    const auto f = util::split(line, ':');
                    net = "Wi-Fi: " + (f.size() > 1 ? f[1] : "") + (f.size() > 2 ? " (" + f[2] + "%)" : "");
                }
            text += net + "\n";
            const int batt = read_int("/sys/class/power_supply/bq27220-0/capacity", -1);
            if (batt >= 0) text += "Battery: " + std::to_string(batt) + "%\n";
            text += "Time zone: " + (S().timezone.empty() ? util::system_timezone() + " (system)" : S().timezone) + "\n";
            std::string err;
            const auto models = llm::list_models(err);
            text += err.empty() ? "AI service: reachable, " + std::to_string(models.size()) + " models" : "AI service: " + err;
            text += "\nMusic: " + std::to_string(MusicLibrary::get().size()) + " tracks";
            return text;
        },
        [&menu](const std::string &text) { if (menu.open()) menu.message("Status", text); });
}

// ---------------------------------------------------------------- sections
// The rows under one section's header in the full list of settings.
std::vector<MenuItem> section_rows(std::vector<MenuItem> all, const std::string &section) {
    std::vector<MenuItem> rows;
    bool inside = false;
    for (MenuItem &row : all) {
        if (row.kind == MenuItem::Kind::Header) inside = row.label == section;
        else if (inside) rows.push_back(std::move(row));
    }
    return rows;
}

// What a section holds, in a few words, for the top page (a section not named here shows none).
std::string section_about(const std::string &section) {
    if (section == "Assistant") return "models, voice, name";
    if (section == "Device") return "sound, screen, phone, place";
    if (section == "Your data") return "calendar, timers, smart home";
    if (section == "AI service") return "address and keys";
    if (section == "About") return "status, setup guide, exit";
    return "";
}

}  // namespace

// ---------------------------------------------------------------- first-run guide
// Each step is a menu page pushed on top of the previous one, so ESC steps back naturally.
namespace {

struct Setup {
    Menu &menu;
    std::function<void()> on_finish;
    size_t base_depth = 0;

    void finish() {
        S().setup_done = true;
        S().save();
        while (menu.depth() > base_depth) menu.pop();
        if (on_finish) on_finish();
    }

    MenuItem edit(const std::string &label, std::string Settings::*field, const std::string &help, bool secret = false) {
        Menu &m = menu;
        return MenuItem::val(label, [field, secret] {
            const std::string &v = S().*field;
            return v.empty() ? std::string("not set") : (secret ? masked(v) : short_address(v));
        }, [&m, label, field, help, secret] {
            m.edit_text(label, secret ? "" : S().*field, secret, [field](const std::string &t) {
                if (util::trim(t).empty()) return;
                S().*field = util::trim(t);
                S().save();
            }, help);
        });
    }

    // Step 6: optional extras, then done.
    void step_extras() {
        std::shared_ptr<Setup> self = keep;
        MenuPage page;
        page.title = "Setup 5 of 5: extras";
        page.build = [self]() {
            std::vector<MenuItem> items;
            Menu::add_paragraph(items, "All optional. You can change these later in settings (SPACE).");
            items.push_back(MenuItem::val("Speak replies", [] { return on_off(S().speak_replies); }, nullptr,
                                          [](int) { S().speak_replies = !S().speak_replies; S().save(); }));
            if (autostart::supported())
                items.push_back(MenuItem::val("Start at power-on", [] { return on_off(autostart::enabled()); }, nullptr,
                                              [](int) { autostart::set(!autostart::enabled()); }));
            items.push_back(self->edit("Web search key", &Settings::search_key,
                                       "A Tavily API key lets the assistant search the web. Leave it out to do without.", true));
            items.push_back(MenuItem::action("Finish", [self] { self->finish(); }));
            return items;
        };
        menu.push(std::move(page));
    }

    // Step 5: about the user.
    void step_you() {
        std::shared_ptr<Setup> self = keep;
        Menu &m = menu;
        MenuPage page;
        page.title = "Setup 4 of 5: about you";
        page.build = [self, &m]() {
            std::vector<MenuItem> items;
            Menu::add_paragraph(items, "These help with greetings, weather and times.");
            items.push_back(self->edit("Your name", &Settings::user_name, "What should the assistant call you?"));
            items.push_back(self->edit("Home city", &Settings::home_city, "Used for weather when you do not name a place."));
            items.push_back(self->edit("Home country", &Settings::home_country, "Tells apart cities that share a name."));
            items.push_back(MenuItem::val("Time zone", [] { return S().timezone.empty() ? util::system_timezone() : S().timezone; }, [&m] {
                m.edit_text("Time zone", S().timezone, false, [&m](const std::string &t) {
                    const std::string zone = util::trim(t);
                    if (!zone.empty() && !util::timezone_exists(zone)) { m.toast("Unknown zone. Example: Europe/Lisbon"); return; }
                    S().timezone = zone;
                    S().save();
                    util::apply_timezone(zone);
                }, "A zone such as Europe/London. Leave blank to follow the system clock. Later you can just say \"I'm in Tokyo\".");
            }));
            items.push_back(MenuItem::val("Dates", [] { return S().month_first() ? std::string("Month first") : std::string("Day first"); }, nullptr,
                                          [](int) { S().date_order = S().month_first() ? "dmy" : "mdy"; util::set_month_first(S().month_first()); S().save(); }));
        items.push_back(MenuItem::val("Units", [] { return S().units == "imperial" ? std::string("Imperial") : std::string("Metric"); }, nullptr,
                                          [](int) { S().units = S().units == "imperial" ? "metric" : "imperial"; S().save(); }));
            items.push_back(MenuItem::action("Continue", [self] { self->step_extras(); }));
            return items;
        };
        menu.push(std::move(page));
    }

    // Step 4: models, pre-filled from what the service offers.
    void step_models() {
        std::shared_ptr<Setup> self = keep;
        Menu &m = menu;
        MenuPage page;
        page.title = "Setup 3 of 5: models";
        page.build = [self, &m]() {
            std::vector<MenuItem> items;
            Menu::add_paragraph(items, "Chosen from your service. Change any that look wrong.");
            items.push_back(MenuItem::val("Language model", [] { return or_dash(S().llm_model); },
                                          [&m] { pick_model(m, "Language model", "chat", &Settings::llm_model, false); }));
            items.push_back(MenuItem::val("Speech to text", [] { return or_dash(S().stt_model); },
                                          [&m] { pick_model(m, "Speech to text", "audio_transcription", &Settings::stt_model, false); }));
            items.push_back(MenuItem::val("Text to speech", [] { return or_dash(S().tts_model); },
                                          [&m] { pick_model(m, "Text to speech", "audio_speech", &Settings::tts_model, false); }));
            items.push_back(MenuItem::action("Continue", [self] { self->step_you(); }));
            return items;
        };
        menu.push(std::move(page));
    }

    // Step 3: the AI service. "Test and continue" proves the address and key before moving on.
    void step_service() {
        std::shared_ptr<Setup> self = keep;
        Menu &m = menu;
        MenuPage page;
        page.title = "Setup 2 of 5: AI service";
        page.build = [self, &m]() {
            std::vector<MenuItem> items;
            Menu::add_paragraph(items, "Enter the address and key of an OpenAI-compatible service.");
            items.push_back(self->edit("Address", &Settings::api_base, "The service's base address, ending in /v1."));
            items.push_back(self->edit("API key", &Settings::api_key, "Typed text is shown while you enter it.", true));
            items.push_back(MenuItem::action("Test and continue", [self, &m] {
                if (!S().configured()) { m.toast("Enter both the address and the key first"); return; }
                m.toast("Contacting the service…");
                background(
                    [] {
                        std::string err;
                        auto models = llm::list_models(err);
                        return std::make_pair(models, err);
                    },
                    [self, &m](const std::pair<std::vector<llm::ModelInfo>, std::string> &r) {
                        if (!r.second.empty()) { m.problem("That did not work", r.second); return; }
                        Settings &st = S();
                        for (auto &model : r.first) {
                            if (st.llm_model.empty() && (model.mode == "chat" || model.mode.empty())) st.llm_model = model.id;
                            if (st.stt_model.empty() && model.mode == "audio_transcription") st.stt_model = model.id;
                            if (st.tts_model.empty() && model.mode == "audio_speech") st.tts_model = model.id;
                        }
                        st.save();
                        m.toast("Connected: " + std::to_string(r.first.size()) + " models available");
                        self->step_models();
                    });
            }));
            items.push_back(MenuItem::action("Skip this step", [self] { self->step_models(); }));
            return items;
        };
        menu.push(std::move(page));
    }

    // Step 2: is there an internet connection?
    void step_internet() {
        std::shared_ptr<Setup> self = keep;
        Menu &m = menu;
        MenuPage page;
        page.title = "Setup 1 of 5: internet";
        page.build = [self, &m]() {
            std::vector<MenuItem> items;
            if (tether::online()) {
                Menu::add_paragraph(items, tether::via_phone() ? "Connected through your phone over Bluetooth."
                                                                : "Connected to the internet.");
                items.push_back(MenuItem::action("Continue", [self] { self->step_service(); }));
                return items;
            }
            Menu::add_paragraph(items, "No internet yet. Join Wi-Fi in the device's own Settings app, or use a paired phone "
                                       "with Bluetooth tethering switched on.");
            items.push_back(MenuItem::action("Check again", [&m] { m.refresh(); }));
            items.push_back(MenuItem::action("Use my phone over Bluetooth", [&m] {
                m.toast("Connecting to your phone…");
                background([] { return tether::connect(); }, [&m](const std::string &err) {
                    m.toast(err.empty() ? "Online through your phone" : "Could not connect: " + err);
                    m.refresh();
                });
            }));
            items.push_back(MenuItem::action("Continue without internet", [self] { self->step_service(); }));
            return items;
        };
        menu.push(std::move(page));
    }

    void step_welcome() {
        std::shared_ptr<Setup> self = keep;
        MenuPage page;
        page.title = "Welcome to Zero Bot";
        page.hint = "ENTER select   ·   ESC back";
        page.build = [self]() {
            std::vector<MenuItem> items;
            Menu::add_paragraph(items, "A voice assistant for your pocket. This short guide connects it to an AI service "
                                       "and sets a few preferences. It takes about two minutes.");
            items.push_back(MenuItem::action("Start setup", [self] { self->step_internet(); }));
            items.push_back(MenuItem::action("Skip for now", [self] { self->finish(); }));
            return items;
        };
        menu.push(std::move(page));
    }

    std::shared_ptr<Setup> keep;   // set by open_setup; keeps the guide alive while its pages exist
};

}  // namespace

void open_setup(Menu &menu, std::function<void()> on_finish) {
    auto setup = std::make_shared<Setup>(Setup{menu, nullptr, menu.depth(), nullptr});
    // Finishing drops the self-reference so the guide is freed once its pages are gone.
    std::weak_ptr<Setup> weak = setup;
    setup->on_finish = [weak, on_finish] {
        if (auto s = weak.lock()) s->keep.reset();
        if (on_finish) on_finish();
    };
    setup->keep = setup;
    setup->step_welcome();
}

void open_settings(Menu &menu, const SettingsHooks &hooks) {
    // Every row of settings, under a header for each section. The top page lists the sections, and
    // each opens a page of the rows under its header: a new header here is a new section there.
    const std::function<std::vector<MenuItem>()> rows = [&menu, hooks]() {
        std::vector<MenuItem> items;
        auto text_setting = [&menu](const std::string &label, std::string Settings::*field, const std::string &help = "") {
            return MenuItem::val(label, [field] { return or_dash(S().*field); }, [&menu, label, field, help] {
                menu.edit_text(label, S().*field, false, [field](const std::string &t) { S().*field = util::trim(t); S().save(); }, help);
            });
        };

        items.push_back(MenuItem::header("Assistant"));
        items.push_back(MenuItem::val("Language model", [] { return or_dash(S().eff_llm_model()); },
                                      [&menu] { pick_model(menu, "Language model", "chat", &Settings::llm_model, false); }));
        items.push_back(MenuItem::val("Vision model", [] { return S().vision_model.empty() ? std::string("same") : S().vision_model; },
                                      [&menu] { pick_model(menu, "Vision model", "chat", &Settings::vision_model, true); }));
        items.push_back(MenuItem::val("Speech to text", [] { return or_dash(S().stt_model); },
                                      [&menu] { pick_model(menu, "Speech to text", "audio_transcription", &Settings::stt_model, false); }));
        items.push_back(MenuItem::val("Text to speech", [] { return or_dash(S().tts_model); },
                                      [&menu] { pick_model(menu, "Text to speech", "audio_speech", &Settings::tts_model, false); }));
        items.push_back(text_setting("Voice", &Settings::tts_voice, "The voice name your speech service expects."));
        items.push_back(MenuItem::val("Speak replies", [] { return on_off(S().speak_replies); }, nullptr,
                                      [](int) { S().speak_replies = !S().speak_replies; S().save(); }));
        items.push_back(MenuItem::val("Fast replies", [] { return on_off(S().fast_mode); }, nullptr,
                                      [](int) { S().fast_mode = !S().fast_mode; S().save(); }));
        items.push_back(text_setting("Assistant name", &Settings::agent_name));

        items.push_back(MenuItem::header("Device"));
        items.push_back(MenuItem::val("Volume", [] { return std::to_string(S().volume) + "%"; }, nullptr, [](int dir) {
            S().volume = clampi(S().volume + dir * 5, 0, 100);
            audio::set_volume(S().volume);
            S().save();
        }));
        items.push_back(MenuItem::val("Brightness", [] { return std::to_string(S().brightness) + "%"; }, nullptr, [](int dir) {
            S().brightness = clampi(S().brightness + dir * 10, 10, 100);
            display::set_brightness(S().brightness);
            S().save();
        }));
        items.push_back(MenuItem::val("Microphone gain", [] { return std::to_string(S().mic_gain) + "%"; }, nullptr, [](int dir) {
            S().mic_gain = clampi(S().mic_gain + dir * 20, 20, 300);
            audio::set_mic_gain(S().mic_gain);
            S().save();
        }));
        items.push_back(MenuItem::val("Time zone", [] { return S().timezone.empty() ? util::system_timezone() : S().timezone; }, [&menu] {
            menu.edit_text("Time zone", S().timezone, false, [&menu](const std::string &t) {
                const std::string zone = util::trim(t);
                if (!zone.empty() && !util::timezone_exists(zone)) { menu.toast("Unknown zone. Example: Europe/Lisbon"); return; }
                S().timezone = zone;
                S().save();
                util::apply_timezone(zone);
            }, "A zone such as Europe/Lisbon or Asia/Tokyo. Leave blank to follow the system. You can also just tell the assistant where you are.");
        }));
        if (autostart::supported())
            items.push_back(MenuItem::val("Start at power-on", [] { return on_off(autostart::enabled()); }, nullptr, [&menu](int) {
                const bool want = !autostart::enabled();
                if (!autostart::set(want)) menu.toast("Could not change the start-up setting");
                else menu.toast(want ? "Opens when the device is switched on" : "The launcher opens first again");
            }));
        items.push_back(MenuItem::val("Internet via phone", [] {
            return std::string(tether::via_phone() ? "Connected" : "ENTER to connect");
        }, [&menu] {
            if (tether::via_phone()) {
                background([] { tether::disconnect(); return 0; }, [&menu](int) { menu.toast("Phone link closed"); menu.refresh(); });
                return;
            }
            menu.toast("Connecting to your phone over Bluetooth…");
            background([] { return tether::connect(); }, [&menu](const std::string &err) {
                menu.toast(err.empty() ? "Online through your phone" : "Could not connect: " + err);
                menu.refresh();
            });
        }));
        items.push_back(MenuItem::val("Use phone when offline", [] { return on_off(S().phone_fallback); }, nullptr,
                                      [](int) { S().phone_fallback = !S().phone_fallback; S().save(); }));
        items.push_back(MenuItem::val("Microphone", [] { return S().mic_builtin ? std::string("Built-in") : std::string("System default"); }, nullptr,
                                      [](int) { S().mic_builtin = !S().mic_builtin; S().save(); audio::set_mic_builtin(S().mic_builtin); }));
        items.push_back(MenuItem::val("Flip camera image", [] { return on_off(S().camera_flip); }, nullptr,
                                      [](int) { S().camera_flip = !S().camera_flip; S().save(); }));
        items.push_back(text_setting("Home city", &Settings::home_city, "Used for weather when you do not name a place."));
        items.push_back(text_setting("Home country", &Settings::home_country, "Tells apart cities that share a name."));
        items.push_back(text_setting("Your name", &Settings::user_name));
        items.push_back(MenuItem::val("Dates", [] { return S().month_first() ? std::string("Month first") : std::string("Day first"); }, nullptr,
                                  [](int) { S().date_order = S().month_first() ? "dmy" : "mdy"; util::set_month_first(S().month_first()); S().save(); }));
        items.push_back(MenuItem::val("Units", [] { return S().units == "imperial" ? std::string("Imperial") : std::string("Metric"); }, nullptr,
                                      [](int) { S().units = S().units == "imperial" ? "metric" : "imperial"; S().save(); }));

        items.push_back(MenuItem::header("Your data"));
        items.push_back(MenuItem::val("Calendar", [] { return std::to_string(Store::get().events().size()) + " events"; },
                                      [&menu] { open_calendar(menu); }));
        items.push_back(MenuItem::val("Reminders and timers", [] {
            const size_t running = Store::get().timers().size();
            return (running ? std::to_string(running) + (running == 1 ? " timer, " : " timers, ") : std::string()) +
                   std::to_string(Store::get().reminders().size()) + " pending";
        }, [&menu] { open_reminders(menu); }));
        items.push_back(MenuItem::val("Memory", [] { return std::to_string(Store::get().memories().size()) + " saved"; },
                                      [&menu] { open_memory(menu); }));
        items.push_back(MenuItem::val("TV remote", [] { return S().tv_brand == "auto" ? std::string("any brand") : S().tv_brand; },
                                      [&menu] { open_remote(menu); }));
        items.push_back(MenuItem::val("Smart home", [] {
            const size_t n = home::thing_count();
            return n ? std::to_string(n) + (n == 1 ? " device" : " devices") : std::string("set up");
        }, [&menu] { open_smart_home(menu); }));
        items.push_back(MenuItem::val("Music library", [] { return std::to_string(MusicLibrary::get().size()) + " tracks"; }, [&menu] {
            menu.toast("Scanning " + S().eff_music_dir() + "…");
            background([] { MusicLibrary::get().refresh(S().eff_music_dir(), true); return MusicLibrary::get().size(); },
                       [&menu](size_t n) { menu.toast(std::to_string(n) + " tracks found"); menu.refresh(); });
        }));
        items.push_back(MenuItem::val("Radio stations", [] {
            const size_t n = radio::stations().size();
            return n ? std::to_string(n) + (n == 1 ? " station" : " stations") : std::string("none");
        }, [&menu] { open_radio(menu); }));
        items.push_back(MenuItem::action("Start a new conversation", [&menu, hooks] {
            if (hooks.new_conversation) hooks.new_conversation();
            menu.close();
        }));

        items.push_back(MenuItem::header("AI service"));
        items.push_back(MenuItem::val("Address", [] { return or_dash(short_address(S().api_base)); }, [&menu] {
            menu.edit_text("Service address", S().api_base, false, [](const std::string &t) { S().api_base = util::trim(t); S().save(); },
                           "The base address of an OpenAI-compatible service, ending in /v1.");
        }));
        items.push_back(MenuItem::val("API key", [] { return S().api_key.empty() ? std::string("not set") : masked(S().api_key); }, [&menu] {
            menu.edit_text("API key", "", true, [&menu](const std::string &t) {
                if (util::trim(t).empty()) { menu.toast("Key left unchanged"); return; }
                S().api_key = util::trim(t);
                S().save();
            }, "Typed text is shown while you enter it. Leave blank to keep the current key.");
        }));
        items.push_back(MenuItem::val("Web search key", [] { return S().search_key.empty() ? std::string("not set") : masked(S().search_key); }, [&menu] {
            menu.edit_text("Web search key", "", true, [](const std::string &t) { S().search_key = util::trim(t); S().save(); },
                           "A Tavily API key enables the web search tool. Leave blank to turn web search off.");
        }));

        items.push_back(MenuItem::val("Compress audio", [] { return on_off(S().compress_audio); }, nullptr,
                                      [](int) { S().compress_audio = !S().compress_audio; S().save(); }));

        items.push_back(MenuItem::header("About"));
        items.push_back(MenuItem::action("Status and connection test", [&menu] { open_status(menu); }));
        items.push_back(MenuItem::action("Run the setup guide again", [hooks] { if (hooks.run_setup) hooks.run_setup(); }));
        items.push_back(MenuItem::action("Exit to launcher", [hooks] { if (hooks.exit_app) hooks.exit_app(); }));
        return items;
    };
    MenuPage page;
    page.title = "Settings";
    page.build = [&menu, rows]() {
        std::vector<MenuItem> items;
        for (const MenuItem &row : rows()) {
            if (row.kind != MenuItem::Kind::Header) continue;
            const std::string section = row.label;
            items.push_back(MenuItem::val(section, [section] { return section_about(section); }, [&menu, rows, section] {
                MenuPage part;
                part.title = section;
                part.build = [rows, section]() { return section_rows(rows(), section); };
                menu.push(std::move(part));
            }));
        }
        items.push_back(MenuItem::val(kKeyGuideTitle, [] { return std::string("what each key does"); },
                                      [&menu] { open_key_guide(menu, true); }));
        return items;
    };
    menu.push(std::move(page));
}

void open_key_guide(Menu &menu, bool from_settings) {
    if (menu.open() && menu.title() == kKeyGuideTitle) return;
    MenuPage page;
    page.title = kKeyGuideTitle;
    page.reading = true;
    std::vector<MenuItem> items;
    const std::vector<keyguide::Section> &sections = keyguide::sections();
    for (const keyguide::Section &s : sections) {
        if (from_settings && &s == &sections.back()) page.top = static_cast<int>(items.size());   // the keys for settings
        items.push_back(MenuItem::header(s.title));
        for (const keyguide::Row &r : s.rows) items.push_back(MenuItem::pair(r.keys, r.action));
    }
    page.build = [items]() { return items; };
    menu.push(std::move(page));
}

}  // namespace ui
