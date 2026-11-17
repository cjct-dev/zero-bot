// What a name said aloud refers to: one thing, the lights of a room, or a fixed group. All the
// loose matching lives here, so the model only ever picks from a list and the code decides what
// that pick covers. A name that fits several things is reported, never guessed.
#include <algorithm>
#include <set>

#include "smarthome/internal.h"

namespace home {

namespace {

const char *const kRoomWords[] = {"Living room", "Dining room", "Guest room", "Spare room", "Kids room", "Utility room",
                                  "Bedroom", "Bathroom", "Kitchen", "Lounge", "Hallway", "Hall", "Landing", "Office",
                                  "Study", "Nursery", "Garage", "Garden", "Porch", "Patio", "Balcony", "Attic", "Loft",
                                  "Basement", "Cellar", "Conservatory", "Toilet", "Stairs"};

bool starts_with(const std::string &text, const std::string &prefix) { return text.compare(0, prefix.size(), prefix) == 0; }

bool ends_with(const std::string &text, const std::string &suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Whether `part` occurs in `whole` starting at a word ("heater" in "bed heater", not "eat").
// Both are plain() text.
bool has_word_start(const std::string &whole, const std::string &part) {
    for (size_t at = whole.find(part); at != std::string::npos; at = whole.find(part, at + 1))
        if (at == 0 || whole[at - 1] == ' ') return true;
    return false;
}

// Rooms in use, each once, in the spelling first met, sorted.
std::vector<std::string> rooms_of(const std::vector<Item> &items, bool with_a_light) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto &i : items) {
        if (i.thing.room.empty() || (with_a_light && !is_light(i.thing))) continue;
        if (seen.insert(plain(i.thing.room)).second) out.push_back(i.thing.room);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// A room is listed under its own name unless a thing is listed under that name too.
std::string room_label(const std::vector<Item> &items, const std::string &room) {
    for (const auto &i : items)
        if (plain(i.label) == plain(room)) return room + " (room)";
    return room;
}

// The fixed groups: the name each is listed under, a second name for when a thing or room is
// already listed under the first, and every phrase that means it.
struct Group {
    Scope scope;
    const char *label, *other_label;
    std::vector<const char *> phrases;
};
const Group kGroups[] = {   // in the order they are listed
    {Scope::AllLights, "all lights", "every light", {"all lights", "all the lights", "lights", "every light", "all lamps", "all the lamps"}},
    {Scope::AllSpeakers, "all speakers", "every speaker", {"all speakers", "all the speakers", "speakers", "every speaker"}},
    {Scope::Everything, "all", "everything", {"all", "everything", "all devices", "every device", "all of them", "all the devices"}},
};

bool any_light(const std::vector<Item> &items) {
    return std::any_of(items.begin(), items.end(), [](const Item &i) { return is_light(i.thing); });
}

bool any_speaker(const std::vector<Item> &items) {
    return std::any_of(items.begin(), items.end(), [](const Item &i) { return i.thing.type == "speaker"; });
}

// Whether "all" has anything to act on: things with a plain on/off switch.
bool any_switchable(const std::vector<Item> &items) {
    return std::any_of(items.begin(), items.end(), [](const Item &i) { return (i.thing.caps & kPower) != 0; });
}

Resolved room_target(const std::vector<Item> &items, const std::string &room) {
    Resolved r;
    r.scope = Scope::Room;
    for (const auto &i : items)
        if (plain(i.thing.room) == plain(room)) {
            if (r.label.empty()) r.label = i.thing.room;
            r.members.push_back(&i);
        }
    return r;
}

Resolved thing_target(const Item &item) {
    Resolved r;
    r.scope = Scope::Thing;
    r.label = item.label;
    r.members.push_back(&item);
    return r;
}

// Whether a group has anything to act on, and so is offered at all.
bool group_present(const std::vector<Item> &items, Scope scope) {
    if (scope == Scope::AllLights) return any_light(items);
    if (scope == Scope::AllSpeakers) return any_speaker(items);
    return any_switchable(items);
}

Resolved group_target(const std::vector<Item> &items, const Group &g) {
    Resolved r;
    r.scope = g.scope;
    r.label = g.label;
    for (const auto &i : items)
        if (g.scope == Scope::Everything || (g.scope == Scope::AllLights && is_light(i.thing)) ||
            (g.scope == Scope::AllSpeakers && i.thing.type == "speaker"))
            r.members.push_back(&i);
    return r;
}

// Names in the sentences handed to the model are always in double quotes, which clean_name()
// removes from every name: a name cannot close its quotes and read as part of the sentence.
std::string list_of(const std::vector<std::string> &names) {
    std::string out;
    for (const auto &n : names) out += (out.empty() ? "" : ", ") + in_quotes(n);
    return out;
}

}  // namespace

std::string in_quotes(const std::string &name) { return "\"" + name + "\""; }

std::string plain(const std::string &text) {
    std::string out;
    for (const char c : util::lower(text))
        if (isalnum(static_cast<unsigned char>(c)) || (c & 0x80)) out += c;
        else if (!out.empty() && out.back() != ' ') out += ' ';
    return util::trim(out);
}

// Names come from devices and bridges, which is to say from whoever set them up. They are shown
// on screen and handed to the model as values, so they are kept short and free of anything that
// could be mistaken for structure.
std::string clean_name(const std::string &raw) {
    std::string out;
    for (const unsigned char c : util::sanitize_utf8(raw)) {
        if (c < 0x20 || c == 0x7f) { if (!out.empty() && out.back() != ' ') out += ' '; continue; }
        if (c == '"' || c == '\\' || c == '<' || c == '>' || c == '{' || c == '}' || c == '`') continue;
        if (c == ' ' && !out.empty() && out.back() == ' ') continue;
        out += static_cast<char>(c);
    }
    return util::trim(util::truncate_utf8(util::trim(out), 40, ""));
}

std::string spoken(const Thing &thing) { return thing.alias.empty() ? thing.name : thing.alias; }

std::string infer_room(const std::string &name) {
    const std::string p = plain(name);
    for (const char *word : kRoomWords) {
        const std::string w = plain(word);
        if (starts_with(p, w) && (p.size() == w.size() || p[w.size()] == ' ')) return word;
    }
    return "";
}

bool suggests_heat(const std::string &name) {
    for (const std::string &word : util::split(plain(name), ' ')) {
        for (const char *prefix : {"heat", "radiator", "blanket", "kettle", "fryer", "straightener", "pump"})
            if (starts_with(word, prefix)) return true;
        for (const char *whole : {"iron", "oven", "fire", "fireplace", "hob", "grill", "toaster", "boiler"})
            if (word == whole) return true;
    }
    return false;
}

bool is_light(const Thing &thing) { return thing.type == "light"; }

std::vector<Item> catalog(const NetworkEntry &entry) {
    std::vector<Item> items;
    for (const auto &d : entry.devices) {
        if (!d.enabled || d.needs_setup || !d.controllable()) continue;
        for (const auto &t : d.things) {
            if (!t.enabled || spoken(t).empty() || !d.listed(t) || !allowed_type(t.type)) continue;
            Item item;
            item.device_id = d.id;
            item.thing = t;
            item.label = spoken(t);
            const auto m = d.meta.find(t.id);
            item.used = m == d.meta.end() ? 0 : m->second.used;
            items.push_back(std::move(item));
        }
    }
    // Two things with one name are told apart by their rooms where they have them.
    std::map<std::string, int> count;
    for (const auto &i : items) ++count[plain(i.label)];
    for (auto &i : items)
        if (count[plain(i.label)] > 1 && !i.thing.room.empty()) i.label += " (" + i.thing.room + ")";
    return items;
}

std::vector<std::string> target_names(const std::vector<Item> &items, bool *overflow) {
    if (overflow) *overflow = false;
    // Every value listed means one thing to resolve(): no two are the same said aloud, and none
    // is shadowed by a thing, room or group that resolve() looks at first. Things come first,
    // under their labels; a room shares a thing's name only as "<Room> (room)"; a group whose
    // name a thing or room already has is listed under its other name ("every light"), or not
    // at all.
    std::set<std::string> taken;
    for (const auto &i : items) taken.insert(plain(i.label));
    // Rooms (only those with a light: a room target means its lights) and groups stay listed
    // whatever happens; things fill the rest.
    std::vector<std::string> fixed;
    for (const auto &room : rooms_of(items, true)) {
        const std::string label = room_label(items, room);
        if (taken.insert(plain(label)).second) fixed.push_back(label);
    }
    for (const auto &room : rooms_of(items, false)) taken.insert(plain(room));   // rooms resolve before groups
    for (const auto &g : kGroups) {
        if (!group_present(items, g.scope)) continue;
        for (const char *label : {g.label, g.other_label})
            if (taken.insert(plain(label)).second) { fixed.push_back(label); break; }
    }
    if (fixed.size() > kMaxTargets) {
        fixed.erase(fixed.begin(), fixed.begin() + static_cast<long>(fixed.size() - kMaxTargets));
        if (overflow) *overflow = true;
    }

    std::vector<std::string> things;
    std::vector<time_t> used;
    std::set<std::string> seen;
    for (const auto &i : items)
        if (seen.insert(plain(i.label)).second) { things.push_back(i.label); used.push_back(i.used); }
    const size_t room_for_things = kMaxTargets - fixed.size();
    if (things.size() > room_for_things) {
        // Too many to list: the ones used most recently stay, in their usual order so that the
        // list changes as little as possible from one request to the next.
        if (overflow) *overflow = true;
        std::vector<size_t> order(things.size());
        for (size_t k = 0; k < order.size(); ++k) order[k] = k;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return used[a] > used[b]; });
        order.resize(room_for_things);
        std::sort(order.begin(), order.end());
        std::vector<std::string> kept;
        for (const size_t k : order) kept.push_back(things[k]);
        things = std::move(kept);
    }
    things.insert(things.end(), fixed.begin(), fixed.end());
    return things;
}

bool resolve(const std::vector<Item> &items, const std::string &said_raw, Resolved &out, std::string &err) {
    out = Resolved();
    const std::string said = clean_name(said_raw);
    std::string want = plain(said);
    if (starts_with(want, "the ")) want = want.substr(4);
    if (want.empty()) { err = "say which device, room or group (target)"; return false; }

    // The exact matches, in the order that makes every value target_names() lists mean what it
    // was listed as: a thing's listed name, then "<Room> (room)", then a room's name, then a
    // group, and only then a thing's other name (the maker's, or the user's own). So a plug the
    // maker's app calls "Bedroom", renamed "Bed heater", never answers to the room, and a plug
    // called "Lights" is that plug, not every light.
    auto exact_things = [&](const std::function<bool(const Item &)> &match) -> int {
        std::vector<const Item *> hits;
        for (const auto &i : items)
            if (match(i)) hits.push_back(&i);
        if (hits.empty()) return 0;
        if (hits.size() == 1) { out = thing_target(*hits[0]); return 1; }
        std::vector<std::string> names;
        bool same = true;
        for (const auto *h : hits) { names.push_back(h->label); same = same && plain(h->label) == plain(hits[0]->label); }
        err = same ? "more than one device is called " + in_quotes(said) + ". Tell the user to give one of them another name in "
                     "Settings, Smart home"
                   : in_quotes(said) + " could mean any of: " + list_of(names) + ". Ask which one";
        return -1;
    };
    if (const int found = exact_things([&](const Item &i) { return plain(i.label) == want; })) return found > 0;

    const std::vector<std::string> rooms = rooms_of(items, false);
    auto find_room = [&](const std::string &name) -> const std::string * {
        for (const auto &r : rooms)
            if (plain(r) == name) return &r;
        return nullptr;
    };
    // "Bedroom (room)" is how a room is listed when a thing has the same name.
    if (ends_with(want, " room") && ends_with(util::lower(said), "(room)"))
        if (const std::string *room = find_room(plain(said.substr(0, said.size() - 6)))) { out = room_target(items, *room); return true; }
    if (const std::string *room = find_room(want)) { out = room_target(items, *room); return true; }

    // "all" always means everything (a status of everything works in any home); the other groups
    // exist only when they have members.
    for (const auto &g : kGroups)
        if (g.scope == Scope::Everything || group_present(items, g.scope))
            for (const char *phrase : g.phrases)
                if (want == phrase) { out = group_target(items, g); return true; }

    if (const int found = exact_things([&](const Item &i) {
            return plain(i.thing.name) == want || (!i.thing.alias.empty() && plain(i.thing.alias) == want);
        }))
        return found > 0;
    // "bedroom lights", "the lights in the bedroom".
    for (const char *suffix : {" lights", " light", " lamps", " lamp", " room"})
        if (ends_with(want, suffix))
            if (const std::string *room = find_room(want.substr(0, want.size() - std::string(suffix).size()))) { out = room_target(items, *room); return true; }
    for (const char *prefix : {"lights in the ", "lights in ", "all lights in the ", "all lights in ", "all the lights in the ",
                               "the lights in the "})
        if (starts_with(want, prefix))
            if (const std::string *room = find_room(want.substr(std::string(prefix).size()))) { out = room_target(items, *room); return true; }

    // Partial. A name with a word that starts with what was said counts first ("lamp" for "Desk
    // lamp"); only when there is none does a name contained in what was said ("the desk lamp
    // please"), longest first.
    struct Candidate { std::string name, shown; const Item *item; const std::string *room; };
    std::vector<Candidate> all, found;
    std::set<std::string> seen;
    for (const auto &i : items)
        if (seen.insert(plain(i.label)).second) all.push_back({plain(i.label), i.label, &i, nullptr});
    for (const auto &r : rooms) all.push_back({plain(r), r + " (room)", nullptr, &r});
    if (want.size() >= 3)
        for (const auto &c : all)
            if (has_word_start(c.name, want)) found.push_back(c);
    if (found.empty()) {
        size_t longest = 0;
        for (const auto &c : all)
            if (c.name.size() >= 3 && has_word_start(want, c.name)) longest = std::max(longest, c.name.size());
        for (const auto &c : all)
            if (longest && c.name.size() == longest && has_word_start(want, c.name)) found.push_back(c);
    }
    if (found.size() == 1) {
        out = found[0].item ? thing_target(*found[0].item) : room_target(items, *found[0].room);
        return true;
    }
    if (found.empty()) {
        std::vector<std::string> names;
        for (const auto &c : all) {
            if (names.size() == 30) break;
            names.push_back(c.shown);
        }
        err = "there is no device called " + in_quotes(said) + ". Tell the user so; the devices are: " + list_of(names) +
              (all.size() > names.size() ? " and others" : "");
    } else {
        std::vector<std::string> names;
        for (const auto &c : found) names.push_back(c.shown);
        err = in_quotes(said) + " could mean any of: " + list_of(names) + ". Ask which one";
    }
    return false;
}

}  // namespace home
