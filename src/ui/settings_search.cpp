// Settings > AI service > Web search: the search services in the order they are asked, and for each its key, on or
// off, its place in the order, how it stands and its use this month. Built from the services' registry
// (agent/search/providers/registry.cpp): a new service needs nothing here.
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
    const std::string kind = e.kind;
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
    items.push_back(MenuItem::val("State", [e] { return search::describe(e); }, nullptr));
    const search::usage::Figures f = search::usage::of(search::id_of(e));
    items.push_back(MenuItem::val("This month", [f] { return std::to_string(f.used) + (f.used == 1 ? " search" : " searches"); }, nullptr));
    if (f.left >= 0) items.push_back(MenuItem::val(name + " said", [f] { return said_words(f); }, nullptr));
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
            }, "Typed text is shown while you enter it. Leave blank to remove the key.");
        }));
        if (e && !e->key.empty()) set_up_rows(menu, *e, name, items);
        return items;
    };
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
            const SearchService *e = entry_of(s, kind);
            items.push_back(MenuItem::val(name_of(kind), [state = e ? search::describe(*e) : std::string("no key")] { return state; },
                                          [&menu, kind] { open_service(menu, kind); }));
        }
        items.push_back(MenuItem::val("Today", [] {
            return std::to_string(search::usage::today()) + " of " + std::to_string(search::usage::kDailyCap) + " searches";
        }, nullptr));
        return items;
    };
    menu.push(std::move(page));
}

}  // namespace ui
