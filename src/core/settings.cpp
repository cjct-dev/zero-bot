#include "core/settings.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <mutex>

namespace {

std::mutex g_mtx;            // guards g_current and g_version
Settings g_current;
uint64_t g_version = 0;      // counts changes, so that an older save never overwrites a newer one
std::mutex g_file_mtx;       // one save at a time; guards g_written
uint64_t g_written = 0;      // the version on disk

std::string settings_path() { return util::config_dir() + "/settings.json"; }

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
        change(g_current);
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
        {"search_key", search_key}, {"volume", volume}, {"brightness", brightness}, {"screen_off_min", screen_off_min}, {"mic_gain", mic_gain},
        {"agent_name", agent_name}, {"user_name", user_name}, {"home_city", home_city}, {"home_country", home_country}, {"date_order", date_order}, {"camera_flip", camera_flip}, {"mic_builtin", mic_builtin}, {"timezone", timezone},
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
    search_key = jget(j, "search_key", search_key);
    volume = jget(j, "volume", volume);
    brightness = jget(j, "brightness", brightness);
    screen_off_min = std::max(0, std::min(24 * 60, jget(j, "screen_off_min", screen_off_min)));
    mic_gain = jget(j, "mic_gain", mic_gain);
    agent_name = jget(j, "agent_name", agent_name);
    user_name = jget(j, "user_name", user_name);
    home_city = jget(j, "home_city", home_city);
    home_country = jget(j, "home_country", home_country);
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
    if (const char *v = getenv("ZB_SEARCH_KEY"); v && *v) s.search_key = v;
    if (const char *v = getenv("ZB_FAST_MODE"); v && *v) s.fast_mode = *v != '0';
    if (const char *v = getenv("ZB_DATE_ORDER"); v && *v) s.date_order = v;
    if (s.date_order != "mdy") s.date_order = "dmy";
    uint64_t version;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
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
