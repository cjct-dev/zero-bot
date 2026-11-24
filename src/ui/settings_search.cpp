// Settings > AI service > Web search: the search services in the order they are asked, and for each its key, on or
// off, its place in the order, how it stands and its use this month. Built from the services' registry
// (agent/search/providers/registry.cpp): a new service needs nothing here. A search made while a page is open (the
// agent's, or the page's own question) changes what it shows: both pages are live (MenuPage::live), their values read
// again as they change, and a service's page is built again when a row comes or goes (MenuPage::revision).
#include "ui/settings_internal.h"

#include <algorithm>
#include <string>
#include <vector>

#include "agent/search/search.h"
#include "agent/search/usage.h"
#include "core/util.h"

namespace ui {

namespace {

const SearchService *entry_of(const Settings &s, const std::string &kind) {
    for (const SearchService &e : s.search)
        if (e.kind == kind) return &e;
    return nullptr;
}

// How the service stands now, as the settings and the search layer say ("no key" without an entry): read at every tick
// of a live page, so only what is in memory (a copy of the settings, the layer's figures).
std::string state_now(const std::string &kind) {
    const Settings s = S();
    const SearchService *e = entry_of(s, kind);
    return e ? search::describe(*e) : std::string("no key");
}

// A number that changes when a service page's rows come or go by themselves: a search marks its key refused (the
// paragraph that says so), or brings the first figure of its allowance (the row that shows it). From cheap reads.
int rows_revision(const std::string &kind) {
    const Settings s = S();
    const SearchService *e = entry_of(s, kind);
    if (!e || e->key.empty()) return 0;
    const bool refused = search::state_of(*e) == search::State::KeyRefused;
    const bool said = search::usage::of(search::id_of(*e)).left >= 0;
    return 1 + (refused ? 2 : 0) + (said ? 4 : 0);
}

std::string name_of(const std::string &kind) {
    for (const search::Registered &r : search::registry())
        if (kind == r.provider->kind()) return r.name;
    return kind;
}

// The services this version knows, in the order they are asked: those of the user's list first, in its order, then
// the others in the registry's.
std::vector<std::string> kinds_in_order(const Settings &s) {
    std::vector<std::string> kinds;
    for (const SearchService &e : s.search)
        if (search::provider_for(e.kind) && std::find(kinds.begin(), kinds.end(), e.kind) == kinds.end()) kinds.push_back(e.kind);
    for (const search::Registered &r : search::registry())
        if (std::find(kinds.begin(), kinds.end(), r.provider->kind()) == kinds.end()) kinds.push_back(r.provider->kind());
    return kinds;
}

// Where the service's entry stands among the entries this version knows (from 0), and how many there are.
std::pair<int, int> place_of(const Settings &s, const std::string &kind) {
    int at = -1, count = 0;
    for (const SearchService &e : s.search) {
        if (!search::provider_for(e.kind)) continue;
        if (e.kind == kind && at < 0) at = count;
        ++count;
    }
    return {at, count};
}

std::string place_words(int at, int count) {
    static const char *const kOrdinal[] = {"first", "second", "third", "fourth", "fifth"};
    const std::string nth = at < 5 ? kOrdinal[at] : std::to_string(at + 1) + "th";
    return nth + " of " + std::to_string(count);
}

// Moves the service's entry one place earlier (-1) or later (+1), past the next entry this version knows (an entry a
// newer version wrote stays where it is among the others).
void move_service(const std::string &kind, int dir) {
    change([&](Settings &s) {
        auto &list = s.search;
        const auto at = std::find_if(list.begin(), list.end(), [&](const SearchService &e) { return e.kind == kind; });
        if (at == list.end()) return;
        for (long i = (at - list.begin()) + dir; i >= 0 && i < static_cast<long>(list.size()); i += dir)
            if (search::provider_for(list[static_cast<size_t>(i)].kind)) {
                std::swap(*at, list[static_cast<size_t>(i)]);
                return;
            }
    });
}

// What the service last said of its allowance: "812 of 1000 left, 23 Nov".
std::string said_words(const search::usage::Figures &f) {
    std::string out = std::to_string(f.left) + (f.limit >= 0 ? " of " + std::to_string(f.limit) : "") + " left";
    return f.said_at ? out + ", " + util::format_time(f.said_at, "%-d %b") : out;
}

// Asks the service what is left of its allowance, off the UI thread; the page shows the answer, or says why not.
void ask_allowance(Menu &menu, const std::string &kind) {
    menu.toast("Asking " + name_of(kind) + "…");
    background_on_page(
        menu,
        [kind] {
            tools::Context ctx;
            return search::ask_allowance(kind, ctx);
        },
        [&menu, kind](const std::string &why) {
            if (!why.empty()) { menu.problem(name_of(kind), why); return; }
            menu.toast(name_of(kind) + " answered");
            menu.refresh();
        });
}

// The rows for a service set up (its entry has a key): on or off, its place in the order, its state and its use.
void set_up_rows(Menu &menu, const SearchService &e, const std::string &name, std::vector<MenuItem> &items) {
    const std::string kind = e.kind, id = search::id_of(e);
    items.push_back(MenuItem::val("Use it", [enabled = e.enabled] { return on_off(enabled); }, nullptr, [kind](int) {
        change([&](Settings &s) {
            for (SearchService &x : s.search)
                if (x.kind == kind) { x.enabled = !x.enabled; return; }
        });
    }));
    const auto [at, count] = place_of(S(), kind);
    if (count > 1)
        items.push_back(MenuItem::val("Asked", [at = at, count = count] { return place_words(at, count); }, nullptr,
                                      [kind](int dir) { move_service(kind, dir); }));
    items.push_back(MenuItem::val("State", [kind] { return state_now(kind); }, nullptr));
    items.push_back(MenuItem::val("This month", [id] {
        const long used = search::usage::of(id).used;
        return std::to_string(used) + (used == 1 ? " search" : " searches");
    }, nullptr));
    if (search::usage::of(id).left >= 0)
        items.push_back(MenuItem::val(name + " said", [id] { return said_words(search::usage::of(id)); }, nullptr));
    const search::Provider *p = search::provider_for(kind);
    if (p && p->tells_allowance())
        items.push_back(MenuItem::action("Ask " + name + " what is left", [&menu, kind] { ask_allowance(menu, kind); }));
}

void open_service(Menu &menu, const std::string &kind) {
    const std::string name = name_of(kind);
    MenuPage page;
    page.title = name;
    page.build = [&menu, kind, name]() {
        std::vector<MenuItem> items;
        const Settings s = S();
        const SearchService *e = entry_of(s, kind);
        if (e && search::state_of(*e) == search::State::KeyRefused)
            Menu::add_paragraph(items, name + " refused this key, so it is not sent again. Type it again, or a new one, to try it.");
        items.push_back(MenuItem::val("Key", [kind] {
            const std::string key = S().search_key(kind);
            return key.empty() ? std::string("not set") : masked(key);
        }, [&menu, kind, name] {
            menu.edit_text(name + " key", "", true, [kind](const std::string &t) {
                const std::string v = util::trim(t);
                change([&](Settings &st) { st.set_search_key(kind, v); });
            }, "Each character shows for a moment as you type. Leave blank to remove the key.");
        }));
        if (e && !e->key.empty()) set_up_rows(menu, *e, name, items);
        return items;
    };
    page.live = true;
    page.revision = [kind] { return rows_revision(kind); };
    menu.push(std::move(page));
}

}  // namespace

std::string web_search_value() {
    const Settings s = S();
    std::string names;
    for (const SearchService &e : s.search) {
        if (!search::provider_for(e.kind) || e.key.empty() || !e.enabled) continue;
        if (search::state_of(e) == search::State::KeyRefused) return name_of(e.kind) + " key refused";
        names += (names.empty() ? "" : ", ") + name_of(e.kind);
    }
    return names.empty() ? "not set up" : names;
}

void open_web_search(Menu &menu) {
    MenuPage page;
    page.title = "Web search";
    page.build = [&menu]() {
        std::vector<MenuItem> items;
        Menu::add_paragraph(items, "Asked in this order: the next only when one fails, finds nothing or has used its allowance.");
        const Settings s = S();
        for (const std::string &kind : kinds_in_order(s)) {
            items.push_back(MenuItem::val(name_of(kind), [kind] { return state_now(kind); }, [&menu, kind] { open_service(menu, kind); }));
        }
        items.push_back(MenuItem::val("Today", [] {
            return std::to_string(search::usage::today()) + " of " + std::to_string(search::usage::kDailyCap) + " searches";
        }, nullptr));
        return items;
    };
    page.live = true;   // each service's state, and the day's searches
    menu.push(std::move(page));
}

}  // namespace ui
