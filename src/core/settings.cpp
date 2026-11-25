#include "core/settings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <set>

namespace {

std::mutex g_mtx;            // guards g_current and g_version
Settings g_current;
uint64_t g_version = 0;      // counts changes, so that an older save never overwrites a newer one
std::mutex g_file_mtx;       // one save at a time; guards g_written
uint64_t g_written = 0;      // the version on disk

std::string settings_path() { return util::config_dir() + "/settings.json"; }

// The search service the old "search_key" and ZB_SEARCH_KEY have always meant.
constexpr const char *kFirstSearchKind = "tavily";
std::atomic<uint64_t> g_typings{0};   // keys typed in this run (SearchService::typed)

json search_to_json(const std::vector<SearchService> &list) {
    json out = json::array();
    for (const SearchService &s : list) {
        json e = s.more.is_object() ? s.more : json::object();
        e["kind"] = s.kind;
        e["key"] = s.key;
        e["enabled"] = s.enabled;
        if (!s.refused.empty()) e["refused"] = s.refused;
        out.push_back(std::move(e));
    }
    return out;
}

// The services in `j`: its "search" list (the first entry of each kind), else one Tavily entry for an older
// file's "search_key", else `fallback`.
std::vector<SearchService> search_from_json(const json &j, std::vector<SearchService> fallback) {
    using util::jget;
    const auto list = j.find("search");
    if (list == j.end() || !list->is_array()) {
        const std::string old = jget(j, "search_key", std::string());
        if (old.empty()) return fallback;
        return {SearchService{kFirstSearchKind, old}};
    }
    std::vector<SearchService> out;
    std::set<std::string> kinds;
    for (const json &e : *list) {
        if (!e.is_object() || !kinds.insert(jget(e, "kind", std::string())).second) continue;
        SearchService s{jget(e, "kind", std::string()), jget(e, "key", std::string()), jget(e, "enabled", true), e,
                        jget(e, "refused", std::string())};
        for (const char *known : {"kind", "key", "enabled", "refused"}) s.more.erase(known);
        out.push_back(std::move(s));
    }
    return out;
}

// Whether two lists of search services hold the same entries, field for field, in the same order.
bool same_services(const std::vector<SearchService> &a, const std::vector<SearchService> &b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const SearchService &x, const SearchService &y) {
        return x.kind == y.kind && x.key == y.key && x.enabled == y.enabled && x.more == y.more && x.refused == y.refused &&
               x.typed == y.typed;
    });
}

json place_to_json(const HomePlace &p) {
    return p.known() ? json{{"lat", p.lat}, {"lon", p.lon}, {"zone", p.zone}} : json();
}

// The home city's place as saved (`fallback` when the file has none): coordinates on the globe, else not known.
HomePlace place_from_json(const json &j, const HomePlace &fallback) {
    using util::jget;
    const auto it = j.find("home_place");
    if (it == j.end()) return fallback;
    if (!it->is_object()) return {};
    HomePlace p{jget(*it, "lat", double(NAN)), jget(*it, "lon", double(NAN)), jget(*it, "zone", std::string())};
    if (!(std::fabs(p.lat) <= 90 && std::fabs(p.lon) <= 180)) return {};   // NAN is neither
    return p;
}

// Writes the settings of version `version`, unless a newer version is already on disk.
void write(const json &j, uint64_t version) {
    std::lock_guard<std::mutex> lk(g_file_mtx);
    if (version <= g_written) return;
    if (util::save_json(settings_path(), j)) g_written = version;
}

}  // namespace

const Settings Settings::get() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_current;
}

Settings Settings::update(const std::function<void(Settings &)> &change, bool save_now) {
    Settings after;
    json j;
    uint64_t version;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        const std::vector<SearchService> search_before = g_current.search;
        const std::string city_before = g_current.home_city, country_before = g_current.home_country;
        change(g_current);
        if (!same_services(search_before, g_current.search)) ++g_current.search_revision;
        // The home city's place belongs to the name it was looked up for.
        if (g_current.home_city != city_before || g_current.home_country != country_before) g_current.home_place = HomePlace();
        version = ++g_version;
        after = g_current;
        if (save_now) j = g_current.to_json();
    }
    if (save_now) write(j, version);
    return after;
}

void Settings::save() {
    json j;
    uint64_t version;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        j = g_current.to_json();
        version = g_version;
    }
    write(j, version);
}

json Settings::to_json() const {
    return {
        {"api_base", api_base}, {"api_key", api_key}, {"llm_model", llm_model}, {"vision_model", vision_model},
        {"stt_model", stt_model}, {"tts_model", tts_model}, {"tts_voice", tts_voice}, {"speak_replies", speak_replies}, {"fast_mode", fast_mode}, {"compress_audio", compress_audio}, {"phone_fallback", phone_fallback}, {"setup_done", setup_done},
        {"search", search_to_json(search)}, {"volume", volume}, {"brightness", brightness}, {"screen_off_min", screen_off_min}, {"mic_gain", mic_gain}, {"dynamic_art", dynamic_art},
        {"agent_name", agent_name}, {"user_name", user_name}, {"home_city", home_city}, {"home_country", home_country}, {"home_place", place_to_json(home_place)}, {"date_order", date_order}, {"camera_flip", camera_flip}, {"mic_builtin", mic_builtin}, {"timezone", timezone},
        {"units", units}, {"tv_brand", tv_brand}, {"music_dir", music_dir},
    };
}

void Settings::from_json(const json &j) {
    using util::jget;
    api_base = jget(j, "api_base", api_base);
    api_key = jget(j, "api_key", api_key);
    llm_model = jget(j, "llm_model", llm_model);
    vision_model = jget(j, "vision_model", vision_model);
    stt_model = jget(j, "stt_model", stt_model);
    tts_model = jget(j, "tts_model", tts_model);
    tts_voice = jget(j, "tts_voice", tts_voice);
    speak_replies = jget(j, "speak_replies", speak_replies);
    fast_mode = jget(j, "fast_mode", fast_mode);
    compress_audio = jget(j, "compress_audio", compress_audio);
    phone_fallback = jget(j, "phone_fallback", phone_fallback);
    setup_done = jget(j, "setup_done", setup_done);
    search = search_from_json(j, search);
    volume = jget(j, "volume", volume);
    brightness = jget(j, "brightness", brightness);
    screen_off_min = std::max(0, std::min(24 * 60, jget(j, "screen_off_min", screen_off_min)));
    mic_gain = jget(j, "mic_gain", mic_gain);
    const std::string art = jget(j, "dynamic_art", dynamic_art);
    dynamic_art = art == "off" || art == "plugged" ? art : "on";
    agent_name = jget(j, "agent_name", agent_name);
    user_name = jget(j, "user_name", user_name);
    home_city = jget(j, "home_city", home_city);
    home_country = jget(j, "home_country", home_country);
    home_place = place_from_json(j, home_place);
    date_order = jget(j, "date_order", date_order);
    camera_flip = jget(j, "camera_flip", camera_flip);
    mic_builtin = jget(j, "mic_builtin", mic_builtin);
    timezone = jget(j, "timezone", timezone);
    units = jget(j, "units", units);
    tv_brand = jget(j, "tv_brand", tv_brand);
    music_dir = jget(j, "music_dir", music_dir);
}

void Settings::load() {
    Settings s;
    s.from_json(util::load_json(settings_path(), json::object()));
    // Environment overrides make development on a PC possible without a settings file.
    if (const char *v = getenv("ZB_API_BASE"); v && *v) s.api_base = v;
    if (const char *v = getenv("ZB_API_KEY"); v && *v) s.api_key = v;
    if (const char *v = getenv("ZB_SEARCH_KEY"); v && *v) {   // a key set, not typed: a refusal kept beside it stands
        const auto is_tavily = [](const SearchService &e) { return e.kind == kFirstSearchKind; };
        if (std::none_of(s.search.begin(), s.search.end(), is_tavily)) s.search.push_back({kFirstSearchKind});
        std::find_if(s.search.begin(), s.search.end(), is_tavily)->key = v;
    }
    if (const char *v = getenv("ZB_FAST_MODE"); v && *v) s.fast_mode = *v != '0';
    if (const char *v = getenv("ZB_DATE_ORDER"); v && *v) s.date_order = v;
    if (s.date_order != "mdy") s.date_order = "dmy";
    uint64_t version;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        s.search_revision = g_current.search_revision + 1;
        g_current = s;
        version = ++g_version;
    }
    {
        std::lock_guard<std::mutex> lk(g_file_mtx);   // what was just read is what the file holds
        g_written = std::max(g_written, version);
    }
    util::set_month_first(s.month_first());
    util::apply_timezone(s.timezone);
}

static std::string strip_slash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

std::string Settings::eff_api_base() const { return strip_slash(api_base); }
std::string Settings::eff_api_key() const { return api_key; }
std::string Settings::eff_llm_model() const { return llm_model; }
std::string Settings::eff_vision_model() const { return vision_model.empty() ? eff_llm_model() : vision_model; }
std::string Settings::eff_music_dir() const { return music_dir.empty() ? util::home_dir() + "/Music" : music_dir; }

std::string Settings::search_key(const std::string &kind) const {
    for (const SearchService &s : search)
        if (s.kind == kind) return s.key;
    return "";
}

void Settings::set_search_key(const std::string &kind, const std::string &key) {
    for (SearchService &s : search)
        if (s.kind == kind) {
            s.key = key;
            s.refused.clear();
            s.typed = ++g_typings;
            return;
        }
    if (!key.empty()) search.push_back({kind, key, true, json(), "", ++g_typings});
}

std::vector<std::string> Settings::secret_values() const {
    std::vector<std::string> out;
    if (!api_key.empty()) out.push_back(api_key);
    for (const SearchService &s : search)
        if (!s.key.empty()) out.push_back(s.key);
    return out;
}
