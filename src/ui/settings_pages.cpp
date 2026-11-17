// The settings menu tree: assistant models, device options, the user's data and the service.
#include "ui/settings_pages.h"

#include <algorithm>
#include <fstream>
#include <thread>

#include "agent/llm.h"
#include "core/autostart.h"
#include "core/music_lib.h"
#include "core/settings.h"
#include "core/store.h"
#include "core/ui_queue.h"
#include "core/util.h"
#include "platform/audio.h"
#include "platform/display.h"
#include "platform/ir.h"
#include "platform/proc.h"

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

int clampi(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

// Runs slow work off the UI thread, then delivers the result back on it.
template <typename Work, typename Done>
void background(Work work, Done done) {
    std::thread([work, done]() {
        auto result = work();
        uiq::post([done, result]() { done(result); });
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
            if (!r.second.empty()) { menu.toast("Could not load models: " + r.second); return; }
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

std::string remind_text(int minutes) {
    if (minutes < 0) return "No alert";
    if (minutes == 0) return "At start";
    if (minutes >= 1440) return "1 day before";
    if (minutes >= 60) return std::to_string(minutes / 60) + " h before";
    return std::to_string(minutes) + " min before";
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
        items.push_back(MenuItem::val("Starts", [e] { return util::format_time(e.start, "%a %d %b %H:%M"); }, [&menu, e] {
            menu.edit_text("Start time", util::format_time(e.start, "%Y-%m-%d %H:%M"), false,
                           [&menu, e](const std::string &t) mutable {
                               time_t when;
                               if (!util::parse_local(t, when)) { menu.toast("Use the form 2026-03-14 18:30"); return; }
                               if (e.end) e.end += when - e.start;
                               e.start = when;
                               Store::get().update_event(e);
                           },
                           "Year-month-day hour:minute, 24-hour clock");
        }));
        items.push_back(MenuItem::val("Ends", [e] { return e.end ? util::format_time(e.end, "%a %d %b %H:%M") : std::string("not set"); },
                                      [&menu, e] {
            menu.edit_text("End time (blank for none)", e.end ? util::format_time(e.end, "%Y-%m-%d %H:%M") : "", false,
                           [&menu, e](const std::string &t) mutable {
                               time_t when = 0;
                               if (!util::trim(t).empty() && !util::parse_local(t, when)) { menu.toast("Use the form 2026-03-14 19:30"); return; }
                               e.end = when;
                               Store::get().update_event(e);
                           });
        }));
        items.push_back(MenuItem::val("Notes", [e] { return e.notes; }, [&menu, e] {
            menu.edit_text("Notes", e.notes, false, [e](const std::string &t) mutable {
                e.notes = util::trim(t);
                Store::get().update_event(e);
            });
        }));
        items.push_back(MenuItem::val("Alert", [e] { return remind_text(e.remind_min); }, nullptr, [e](int dir) mutable {
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
        bool past_header = false, future_header = false;
        for (auto &e : events) {
            const bool past = (e.end ? e.end : e.start) < now - 3600;
            if (past && !past_header) { items.push_back(MenuItem::header("Past")); past_header = true; }
            if (!past && !future_header) { items.push_back(MenuItem::header("Upcoming")); future_header = true; }
            const int id = e.id;
            items.push_back(MenuItem::val(e.title, [e] { return util::friendly_when(e.start); }, [&menu, id] { open_event(menu, id); }));
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

// ---------------------------------------------------------------- reminders
void open_reminders(Menu &menu) {
    MenuPage page;
    page.title = "Reminders and timers";
    page.build = [&menu]() {
        std::vector<MenuItem> items;
        const auto list = Store::get().reminders();
        for (auto &r : list) {
            const int id = r.id;
            items.push_back(MenuItem::val(r.text.empty() ? "Timer" : r.text, [r] { return util::friendly_when(r.due); }, [&menu, id] {
                menu.confirm("Cancel this reminder?", "Cancel reminder", [id] { Store::get().delete_reminder(id); });
            }));
        }
        if (list.empty()) items.push_back(MenuItem::header("Nothing pending. Say \"remind me in ten minutes…\""));
        else
            items.push_back(MenuItem::action("Cancel all", [&menu] {
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

// ---------------------------------------------------------------- about
int read_int(const char *path, int fallback) {
    std::ifstream f(path);
    int v;
    return f >> v ? v : fallback;
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

}  // namespace

void open_settings(Menu &menu, const SettingsHooks &hooks) {
    MenuPage page;
    page.title = "Settings";
    page.build = [&menu, hooks]() {
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
        items.push_back(text_setting("Home city", &Settings::home_city, "Used for weather when you do not name a place."));
        items.push_back(text_setting("Your name", &Settings::user_name));
        items.push_back(MenuItem::val("Units", [] { return S().units == "imperial" ? std::string("Imperial") : std::string("Metric"); }, nullptr,
                                      [](int) { S().units = S().units == "imperial" ? "metric" : "imperial"; S().save(); }));

        items.push_back(MenuItem::header("Your data"));
        items.push_back(MenuItem::val("Calendar", [] { return std::to_string(Store::get().events().size()) + " events"; },
                                      [&menu] { open_calendar(menu); }));
        items.push_back(MenuItem::val("Reminders", [] { return std::to_string(Store::get().reminders().size()) + " pending"; },
                                      [&menu] { open_reminders(menu); }));
        items.push_back(MenuItem::val("Memory", [] { return std::to_string(Store::get().memories().size()) + " saved"; },
                                      [&menu] { open_memory(menu); }));
        items.push_back(MenuItem::val("TV remote", [] { return S().tv_brand == "auto" ? std::string("any brand") : S().tv_brand; },
                                      [&menu] { open_remote(menu); }));
        items.push_back(MenuItem::val("Music library", [] { return std::to_string(MusicLibrary::get().size()) + " tracks"; }, [&menu] {
            menu.toast("Scanning " + S().eff_music_dir() + "…");
            background([] { MusicLibrary::get().refresh(S().eff_music_dir(), true); return MusicLibrary::get().size(); },
                       [&menu](size_t n) { menu.toast(std::to_string(n) + " tracks found"); menu.refresh(); });
        }));
        items.push_back(MenuItem::action("Start a new conversation", [&menu, hooks] {
            if (hooks.new_conversation) hooks.new_conversation();
            menu.close();
        }));

        items.push_back(MenuItem::header("AI service"));
        items.push_back(MenuItem::val("Address", [] { return or_dash(S().api_base); }, [&menu] {
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

        items.push_back(MenuItem::header("About"));
        items.push_back(MenuItem::action("Status and connection test", [&menu] { open_status(menu); }));
        items.push_back(MenuItem::action("Exit to launcher", [hooks] { if (hooks.exit_app) hooks.exit_app(); }));
        return items;
    };
    menu.push(std::move(page));
}

}  // namespace ui
