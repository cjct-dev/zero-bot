#include "core/settings.h"

#include <cstdlib>

Settings &Settings::get() {
    static Settings s;
    return s;
}

static std::string settings_path() { return util::config_dir() + "/settings.json"; }

json Settings::to_json() const {
    return {
        {"api_base", api_base}, {"api_key", api_key}, {"llm_model", llm_model}, {"vision_model", vision_model},
        {"stt_model", stt_model}, {"tts_model", tts_model}, {"tts_voice", tts_voice}, {"speak_replies", speak_replies}, {"fast_mode", fast_mode}, {"compress_audio", compress_audio}, {"phone_fallback", phone_fallback}, {"setup_done", setup_done},
        {"search_key", search_key}, {"volume", volume}, {"brightness", brightness}, {"mic_gain", mic_gain},
        {"agent_name", agent_name}, {"user_name", user_name}, {"home_city", home_city}, {"home_country", home_country}, {"date_order", date_order}, {"camera_flip", camera_flip}, {"timezone", timezone},
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
    mic_gain = jget(j, "mic_gain", mic_gain);
    agent_name = jget(j, "agent_name", agent_name);
    user_name = jget(j, "user_name", user_name);
    home_city = jget(j, "home_city", home_city);
    home_country = jget(j, "home_country", home_country);
    date_order = jget(j, "date_order", date_order);
    camera_flip = jget(j, "camera_flip", camera_flip);
    timezone = jget(j, "timezone", timezone);
    units = jget(j, "units", units);
    tv_brand = jget(j, "tv_brand", tv_brand);
    music_dir = jget(j, "music_dir", music_dir);
}

void Settings::load() {
    from_json(util::load_json(settings_path(), json::object()));
    // Environment overrides make development on a PC possible without a settings file.
    if (const char *v = getenv("ZB_API_BASE"); v && *v) api_base = v;
    if (const char *v = getenv("ZB_API_KEY"); v && *v) api_key = v;
    if (const char *v = getenv("ZB_SEARCH_KEY"); v && *v) search_key = v;
    if (const char *v = getenv("ZB_FAST_MODE"); v && *v) fast_mode = *v != '0';
    if (const char *v = getenv("ZB_DATE_ORDER"); v && *v) date_order = v;
    if (date_order != "mdy") date_order = "dmy";
    util::set_month_first(month_first());
    util::apply_timezone(timezone);
}

void Settings::save() { util::save_json(settings_path(), to_json()); }

static std::string strip_slash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

std::string Settings::eff_api_base() const { return strip_slash(api_base); }
std::string Settings::eff_api_key() const { return api_key; }
std::string Settings::eff_llm_model() const { return llm_model; }
std::string Settings::eff_vision_model() const { return vision_model.empty() ? eff_llm_model() : vision_model; }
std::string Settings::eff_music_dir() const { return music_dir.empty() ? util::home_dir() + "/Music" : music_dir; }
