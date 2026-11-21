// Settings > Your data: the calendar and its events, reminders and timers, memories, the TV remote,
// saved conversations and radio stations.
#include "ui/settings_internal.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <string>
#include <vector>

#include "core/activity.h"
#include "core/radio.h"
#include "core/store.h"
#include "core/timers.h"
#include "core/util.h"
#include "platform/audio.h"
#include "platform/ir.h"

namespace ui {

// ---------------------------------------------------------------- calendar
namespace {

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

}  // namespace

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
namespace {

// "4:59 left", counting down while the page is open.
std::string time_left(time_t end) {
    const long left = static_cast<long>(end - timers::now());
    return left > 0 ? timers::clock_text(left) + " left" : std::string("ended");
}

}  // namespace

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
namespace {

constexpr size_t kMemoryRowChars = 46;   // characters of a memory's text on one row of its page

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
            size_t len = std::min<size_t>(kMemoryRowChars, text.size() - pos);
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

}  // namespace

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
            menu.choose("TV brand", ir::brands(), S().tv_brand, [](const std::string &b) { change([&b](Settings &s) { s.tv_brand = b; }); });
        }));
        items.push_back(MenuItem::action("Test: send power", [&menu] {
            // With "Any" every brand's code goes out in turn, which takes about half a second.
            const std::string brand = S().tv_brand;
            background([brand] { return ir::send(brand, "power", 1); },
                       [&menu](const std::string &err) { menu.toast(err.empty() ? "Power sent" : err); });
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

// ---------------------------------------------------------------- saved conversations
// DIAGNOSTICS (temporary, ZB-051; core/activity.h): what SPACE twice on the chat screen has saved,
// and a way to delete it all.
void open_saved_conversations(Menu &menu) {
    MenuPage page;
    page.title = "Saved conversations";
    page.build = [&menu]() {
        std::vector<MenuItem> items;
        const size_t n = activity::saved_count();
        items.push_back(MenuItem::header(n ? std::to_string(n) + (n == 1 ? " conversation saved" : " conversations saved")
                                           : std::string("None saved")));
        if (n)
            items.push_back(MenuItem::action("Delete saved conversations", [&menu] {
                menu.confirm("Delete every saved conversation?", "Delete all", [] { activity::delete_saved(); });
            }, true));
        Menu::add_paragraph(items, "SPACE twice on the chat screen saves the conversation so far, to help diagnose and improve "
                                   "the assistant. Nothing is saved any other way. This is temporary and will be removed.");
        Menu::add_paragraph(items, "Kept in ~/.local/share/zero-bot/conversations, the newest " + std::to_string(activity::kMaxSaved) +
                                       " at most.");
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
                // Checked first, as the assistant does: a playlist file is followed to its stream,
                // and a station that will not play says why here rather than failing quietly.
                menu.toast("Trying " + s.name + "…");
                radio::check_async(s, [&menu, s](const radio::Check &c) {
                    if (!c.error.empty()) {
                        std::string why = c.error;   // "the station refused ...": a sentence of its own here
                        why[0] = static_cast<char>(toupper(static_cast<unsigned char>(why[0])));
                        if (menu.open()) menu.problem(s.name + " did not play", why + ".");
                        return;
                    }
                    audio::Music::get().play_live(c.url, s.name);
                    if (menu.open()) { menu.toast("Playing " + s.name); menu.refresh(); }
                });
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

}  // namespace ui
