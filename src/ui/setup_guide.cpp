// The first-run guide: connection, AI service, models, a few personal details.
#include "ui/settings_pages.h"
#include "ui/settings_internal.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/tether.h"
#include "core/util.h"
#include "ui/models.h"

namespace ui {

// ---------------------------------------------------------------- first-run guide
// Each step is a menu page pushed on top of the previous one, so ESC steps back naturally.
namespace {

// A reply that comes back after the user has left the page that asked for it (on to the next step,
// back with ESC, out of the guide, or on and back again) is dropped (background_on_page, Menu::showing),
// as is a list of models once the service's address or key has changed (models::fetch): run late, "Test
// and continue" reopened a finished guide on the models page, whose Continue then used the self-reference
// finish() had already let go.
struct Setup {
    Menu &menu;
    std::function<void()> on_finish;
    size_t base_depth = 0;
    bool finished = false;

    void finish() {
        if (finished) return;
        finished = true;
        change([](Settings &s) { s.setup_done = true; });
        while (menu.depth() > base_depth) menu.pop();
        if (on_finish) on_finish();
    }

    // Every page of the guide is marked as one: SPACE does not close it (Menu::in_guide).
    void show(MenuPage page) {
        page.guide = true;
        menu.push(std::move(page));
    }

    // A text setting, read with `get` and changed with `set`; an empty entry leaves it as it was.
    MenuItem edit(const std::string &label, std::function<std::string(const Settings &)> get,
                  std::function<void(Settings &, const std::string &)> set, const std::string &help, bool secret = false) {
        Menu &m = menu;
        return MenuItem::val(label, [get, secret] {
            const std::string v = get(S());
            return v.empty() ? std::string("not set") : (secret ? masked(v) : short_address(v));
        }, [&m, label, get, set, help, secret] {
            m.edit_text(label, secret ? "" : get(S()), secret, [set](const std::string &t) {
                const std::string v = util::trim(t);
                if (v.empty()) return;
                change([&](Settings &s) { set(s, v); });
            }, help);
        });
    }

    MenuItem edit(const std::string &label, std::string Settings::*field, const std::string &help, bool secret = false) {
        return edit(label, [field](const Settings &s) { return s.*field; },
                    [field](Settings &s, const std::string &v) { s.*field = v; }, help, secret);
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
                                          [](int) { change([](Settings &s) { s.speak_replies = !s.speak_replies; }); }));
            const search::Registered &web = asked_search();
            const std::string kind = web.provider->kind();
            items.push_back(self->edit("Web search key", [kind](const Settings &s) { return s.search_key(kind); },
                                       [kind](Settings &s, const std::string &v) { s.set_search_key(kind, v); },
                                       "With a key from " + web.name + " the assistant can search the web. Leave it out to do without; more "
                                       "search services can be set up later in settings.", true));
            items.push_back(MenuItem::action("Finish", [self] { self->finish(); }));
            return items;
        };
        show(std::move(page));
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
                    change([&zone](Settings &s) { s.timezone = zone; });
                    util::apply_timezone(zone);
                }, "A zone such as Europe/London. Leave blank to follow the system clock. Later you can just say \"I'm in Tokyo\".");
            }));
            items.push_back(MenuItem::val("Dates", [] { return S().month_first() ? std::string("Month first") : std::string("Day first"); }, nullptr,
                                          [](int) { util::set_month_first(change([](Settings &s) { s.date_order = s.month_first() ? "dmy" : "mdy"; }).month_first()); }));
        items.push_back(MenuItem::val("Units", [] { return S().units == "imperial" ? std::string("Imperial") : std::string("Metric"); }, nullptr,
                                          [](int) { change([](Settings &s) { s.units = s.units == "imperial" ? "metric" : "imperial"; }); }));
            items.push_back(MenuItem::action("Continue", [self] { self->step_extras(); }));
            return items;
        };
        show(std::move(page));
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
        show(std::move(page));
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
                const unsigned long page = m.on_top();
                models::fetch([self, &m, page](const models::List &list, const std::string &err) {
                    if (!m.showing(page)) return;   // the user left the step (on, back, or on and back again) or finished
                    if (!err.empty()) { m.problem("That did not work", err); return; }
                    change([&list](Settings &st) { models::choose(st, list); });
                    m.toast("Connected: " + std::to_string(list.size()) + " models available");
                    self->step_models();
                });
            }));
            items.push_back(MenuItem::action("Skip this step", [self] { self->step_models(); }));
            return items;
        };
        show(std::move(page));
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
                // Said on this page only: not over another, nor on the chat after the guide.
                background_on_page(m, [] { return tether::connect(); }, [&m](const std::string &err) {
                    m.toast(err.empty() ? "Online through your phone" : "Could not connect: " + err);
                    m.refresh();
                });
            }));
            items.push_back(MenuItem::action("Continue without internet", [self] { self->step_service(); }));
            return items;
        };
        show(std::move(page));
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
        show(std::move(page));
    }

    std::shared_ptr<Setup> keep;   // set by open_setup; keeps the guide alive while its pages exist
};

}  // namespace

void open_setup(Menu &menu, std::function<void()> on_finish) {
    auto setup = std::make_shared<Setup>(Setup{menu, nullptr, menu.depth(), false, nullptr});
    // Finishing drops the self-reference so the guide is freed once its pages are gone.
    std::weak_ptr<Setup> weak = setup;
    setup->on_finish = [weak, on_finish] {
        if (auto s = weak.lock()) s->keep.reset();
        if (on_finish) on_finish();
    };
    setup->keep = setup;
    setup->step_welcome();
}

}  // namespace ui
