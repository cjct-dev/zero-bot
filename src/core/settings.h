// User settings, persisted as JSON in ~/.config/zero-bot/settings.json (mode 0600).
#pragma once
#include <mutex>
#include <string>

#include "core/util.h"

struct Settings {
    // Speech + language services (any OpenAI-compatible endpoint).
    std::string api_base;        // e.g. https://host/v1
    std::string api_key;
    std::string llm_model;
    std::string vision_model;    // empty = same as llm_model
    std::string stt_model;
    std::string tts_model;
    std::string tts_voice = "alloy";
    bool speak_replies = true;
    bool setup_done = false;     // has the first-run guide been completed or skipped?
    bool phone_fallback = true;  // try the paired phone over Bluetooth when there is no connection
    bool compress_audio = true;  // send and receive speech in compressed form (smaller, same words)
    bool fast_mode = true;       // ask the model to skip its private reasoning step (quicker replies)

    // Web search (Tavily-compatible).
    std::string search_key;

    // Device
    int volume = 70;
    int brightness = 80;
    int mic_gain = 100;

    // Personalisation
    std::string agent_name = "Zero";
    std::string user_name;
    std::string home_city;
    std::string date_order = "dmy";   // "dmy" (3/4 is 3 April) or "mdy" (3/4 is March 4)
    bool month_first() const { return date_order == "mdy"; }
    bool camera_flip = true;          // turn photos the right way up (the sensor is mounted inverted)
    std::string home_country;   // tells same-named cities apart (London, Canada)
    std::string timezone;        // IANA name; empty = system
    std::string units = "metric";
    std::string tv_brand = "auto";
    std::string music_dir;       // empty = ~/Music

    static Settings &get();
    void load();
    void save();

    // Values with defaults applied.
    std::string eff_api_base() const;
    std::string eff_api_key() const;
    std::string eff_llm_model() const;
    std::string eff_vision_model() const;
    std::string eff_music_dir() const;
    bool configured() const { return !eff_api_base().empty() && !eff_api_key().empty(); }

    json to_json() const;
    void from_json(const json &j);

};
