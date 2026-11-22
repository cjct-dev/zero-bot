// The settings menu: the top page and its sections (Assistant, Device, Your data, AI service, About),
// the status page and the key guide. Your data's pages are in settings_data.cpp and settings_home.cpp,
// the first-run guide in setup_guide.cpp.
#include "ui/settings_pages.h"
#include "ui/settings_internal.h"

#include <string>
#include <vector>

#include "core/activity.h"
#include "core/music_lib.h"
#include "core/radio.h"
#include "core/store.h"
#include "core/tether.h"
#include "core/util.h"
#include "platform/audio.h"
#include "platform/display.h"
#include "platform/net_status.h"
#include "platform/power.h"
#include "smarthome/home.h"
#include "ui/key_guide.h"
#include "ui/models.h"
#include "ui/screen_idle.h"

namespace ui {

// ---------------------------------------------------------------- models
void pick_model(Menu &menu, const std::string &title, const std::string &mode, std::string Settings::*field,
                bool allow_default) {
    menu.toast("Loading models…");
    models::fetch([&menu, title, mode, field, allow_default](const models::List &list, const std::string &err) {
        if (!menu.open()) return;
        if (!err.empty()) { menu.problem("Could not load models", err); return; }
        // The models labelled for this use, else those not labelled for another, else all of them
        // (models::offered: the user is choosing by hand here, and is never offered nothing).
        std::vector<std::string> options;
        const std::string same = "(same as language model)";
        if (allow_default) options.push_back(same);
        for (const std::string &id : models::offered(list, mode)) options.push_back(id);
        const std::string now = S().*field;
        const std::string current = now.empty() && allow_default ? same : now;
        menu.choose(title, options, current, [field, same](const std::string &pick) {
            change([&](Settings &s) { s.*field = pick == same ? "" : pick; });
        });
    });
}

namespace {

// ---------------------------------------------------------------- about
void open_status(Menu &menu) {
    menu.toast("Checking…");
    background(
        [] {
            std::string text = "Zero Bot " ZB_VERSION "\n";
            const net_status::WiFi wifi = net_status::wifi(5000);
            text += (wifi.connected ? "Wi-Fi: " + wifi.ssid + (wifi.has_signal ? " (" + wifi.signal + "%)" : "") : std::string("Wi-Fi: not connected")) + "\n";
            const int batt = power::battery().percent;
            if (batt >= 0) text += "Battery: " + std::to_string(batt) + "%\n";
            text += "Time zone: " + (S().timezone.empty() ? util::system_timezone() + " (system)" : S().timezone) + "\n";
            std::string err;
            const models::List list = models::list(err);
            text += err.empty() ? "AI service: reachable, " + std::to_string(list.size()) + " models" : "AI service: " + err;
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

// A row for a setting typed in as text.
MenuItem text_setting(Menu &menu, const std::string &label, std::string Settings::*field, const std::string &help = "") {
    return MenuItem::val(label, [field] { return or_dash(S().*field); }, [&menu, label, field, help] {
        menu.edit_text(label, S().*field, false, [field](const std::string &t) { const std::string v = util::trim(t); change([&](Settings &s) { s.*field = v; }); }, help);
    });
}

// The rows under each section's header.
void assistant_rows(Menu &menu, std::vector<MenuItem> &items) {
    items.push_back(MenuItem::val("Language model", [] { return or_dash(S().eff_llm_model()); },
                                  [&menu] { pick_model(menu, "Language model", "chat", &Settings::llm_model, false); }));
    items.push_back(MenuItem::val("Vision model", [] { return S().vision_model.empty() ? std::string("same") : S().vision_model; },
                                  [&menu] { pick_model(menu, "Vision model", "chat", &Settings::vision_model, true); }));
    items.push_back(MenuItem::val("Speech to text", [] { return or_dash(S().stt_model); },
                                  [&menu] { pick_model(menu, "Speech to text", "audio_transcription", &Settings::stt_model, false); }));
    items.push_back(MenuItem::val("Text to speech", [] { return or_dash(S().tts_model); },
                                  [&menu] { pick_model(menu, "Text to speech", "audio_speech", &Settings::tts_model, false); }));
    items.push_back(text_setting(menu, "Voice", &Settings::tts_voice, "The voice name your speech service expects."));
    items.push_back(MenuItem::val("Speak replies", [] { return on_off(S().speak_replies); }, nullptr,
                                  [](int) { change([](Settings &s) { s.speak_replies = !s.speak_replies; }); }));
    items.push_back(MenuItem::val("Fast replies", [] { return on_off(S().fast_mode); }, nullptr,
                                  [](int) { change([](Settings &s) { s.fast_mode = !s.fast_mode; }); }));
    items.push_back(text_setting(menu, "Assistant name", &Settings::agent_name));
}

void device_rows(Menu &menu, std::vector<MenuItem> &items) {
    items.push_back(MenuItem::val("Volume", [] { return std::to_string(S().volume) + "%"; }, nullptr, [](int dir) {
        const int volume = change([dir](Settings &s) { s.volume = clampi(s.volume + dir * 5, 0, 100); }).volume;
        audio::set_volume_async(volume);   // the mixer command takes up to 3 s
    }));
    items.push_back(MenuItem::val("Brightness", [] { return std::to_string(S().brightness) + "%"; }, nullptr, [](int dir) {
        display::set_brightness(change([dir](Settings &s) { s.brightness = clampi(s.brightness + dir * 10, 10, 100); }).brightness);
    }));
    // Against burn-in: after this long unused the screen goes off; any key wakes it (screen_idle.h).
    items.push_back(MenuItem::val("Screen off after", [] { return screen::off_label(S().screen_off_min); }, nullptr, [](int dir) {
        change([dir](Settings &s) { s.screen_off_min = screen::step_off_choice(s.screen_off_min, dir); });
    }));
    items.push_back(MenuItem::val("Microphone gain", [] { return std::to_string(S().mic_gain) + "%"; }, nullptr, [](int dir) {
        audio::set_mic_gain(change([dir](Settings &s) { s.mic_gain = clampi(s.mic_gain + dir * 20, 20, 300); }).mic_gain);
    }));
    items.push_back(MenuItem::val("Time zone", [] { return S().timezone.empty() ? util::system_timezone() : S().timezone; }, [&menu] {
        menu.edit_text("Time zone", S().timezone, false, [&menu](const std::string &t) {
            const std::string zone = util::trim(t);
            if (!zone.empty() && !util::timezone_exists(zone)) { menu.toast("Unknown zone. Example: Europe/Lisbon"); return; }
            change([&zone](Settings &s) { s.timezone = zone; });
            util::apply_timezone(zone);
        }, "A zone such as Europe/Lisbon or Asia/Tokyo. Leave blank to follow the system. You can also just tell the assistant where you are.");
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
                                  [](int) { change([](Settings &s) { s.phone_fallback = !s.phone_fallback; }); }));
    items.push_back(MenuItem::val("Microphone", [] { return S().mic_builtin ? std::string("Built-in") : std::string("System default"); }, nullptr,
                                  [](int) { audio::set_mic_builtin(change([](Settings &s) { s.mic_builtin = !s.mic_builtin; }).mic_builtin); }));
    items.push_back(MenuItem::val("Flip camera image", [] { return on_off(S().camera_flip); }, nullptr,
                                  [](int) { change([](Settings &s) { s.camera_flip = !s.camera_flip; }); }));
    items.push_back(text_setting(menu, "Home city", &Settings::home_city, "Used for weather when you do not name a place."));
    items.push_back(text_setting(menu, "Home country", &Settings::home_country, "Tells apart cities that share a name."));
    items.push_back(text_setting(menu, "Your name", &Settings::user_name));
    items.push_back(MenuItem::val("Dates", [] { return S().month_first() ? std::string("Month first") : std::string("Day first"); }, nullptr,
                              [](int) { util::set_month_first(change([](Settings &s) { s.date_order = s.month_first() ? "dmy" : "mdy"; }).month_first()); }));
    items.push_back(MenuItem::val("Units", [] { return S().units == "imperial" ? std::string("Imperial") : std::string("Metric"); }, nullptr,
                                  [](int) { change([](Settings &s) { s.units = s.units == "imperial" ? "metric" : "imperial"; }); }));
}

void data_rows(Menu &menu, const SettingsHooks &hooks, std::vector<MenuItem> &items) {
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
    // DIAGNOSTICS (temporary, ZB-051)
    items.push_back(MenuItem::val("Saved conversations", [] {
        const size_t n = activity::saved_count();
        return n ? std::to_string(n) + " saved" : std::string("none");
    }, [&menu] { open_saved_conversations(menu); }));
    items.push_back(MenuItem::action("Start a new conversation", [&menu, hooks] {
        if (hooks.new_conversation) hooks.new_conversation();
        menu.close();
    }));
}

void service_rows(Menu &menu, std::vector<MenuItem> &items) {
    items.push_back(MenuItem::val("Address", [] { return or_dash(short_address(S().api_base)); }, [&menu] {
        menu.edit_text("Service address", S().api_base, false, [](const std::string &t) { const std::string v = util::trim(t); change([&v](Settings &s) { s.api_base = v; }); },
                       "The base address of an OpenAI-compatible service, ending in /v1.");
    }));
    items.push_back(MenuItem::val("API key", [] { return S().api_key.empty() ? std::string("not set") : masked(S().api_key); }, [&menu] {
        menu.edit_text("API key", "", true, [&menu](const std::string &t) {
            const std::string v = util::trim(t);
            if (v.empty()) { menu.toast("Key left unchanged"); return; }
            change([&v](Settings &s) { s.api_key = v; });
        }, "Typed text is shown while you enter it. Leave blank to keep the current key.");
    }));
    items.push_back(MenuItem::val("Web search key", [] { return S().search_key.empty() ? std::string("not set") : masked(S().search_key); }, [&menu] {
        menu.edit_text("Web search key", "", true, [](const std::string &t) { const std::string v = util::trim(t); change([&v](Settings &s) { s.search_key = v; }); },
                       "A Tavily API key enables the web search tool. Leave blank to turn web search off.");
    }));

    items.push_back(MenuItem::val("Compress audio", [] { return on_off(S().compress_audio); }, nullptr,
                                  [](int) { change([](Settings &s) { s.compress_audio = !s.compress_audio; }); }));
}

void about_rows(Menu &menu, const SettingsHooks &hooks, std::vector<MenuItem> &items) {
    items.push_back(MenuItem::action("Status and connection test", [&menu] { open_status(menu); }));
    items.push_back(MenuItem::action("Run the setup guide again", [hooks] { if (hooks.run_setup) hooks.run_setup(); }));
    items.push_back(MenuItem::action("Exit to launcher", [hooks] { if (hooks.exit_app) hooks.exit_app(); }));
}

}  // namespace

void open_settings(Menu &menu, const SettingsHooks &hooks) {
    // Every row of settings, under a header for each section. The top page lists the sections, and
    // each opens a page of the rows under its header: a new header here is a new section there.
    const std::function<std::vector<MenuItem>()> rows = [&menu, hooks]() {
        std::vector<MenuItem> items;
        items.push_back(MenuItem::header("Assistant"));
        assistant_rows(menu, items);
        items.push_back(MenuItem::header("Device"));
        device_rows(menu, items);
        items.push_back(MenuItem::header("Your data"));
        data_rows(menu, hooks, items);
        items.push_back(MenuItem::header("AI service"));
        service_rows(menu, items);
        items.push_back(MenuItem::header("About"));
        about_rows(menu, hooks, items);
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
    const std::vector<keyguide::Section> &sections = keyguide::sections(menu.in_guide());
    for (const keyguide::Section &s : sections) {
        if (from_settings && &s == &sections.back()) page.top = static_cast<int>(items.size());   // the keys for settings
        items.push_back(MenuItem::header(s.title));
        for (const keyguide::Row &r : s.rows) items.push_back(MenuItem::pair(r.keys, r.action));
    }
    page.build = [items]() { return items; };
    menu.push(std::move(page));
}

}  // namespace ui
