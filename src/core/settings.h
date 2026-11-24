// User settings, persisted as JSON in ~/.config/zero-bot/settings.json (mode 0600).
//
// Threads: the UI thread, the agent's worker, speech and background jobs all read the settings,
// and the UI and the agent's tools both change them. So nobody holds the shared copy: get() hands
// out a snapshot, copied under a lock, that cannot be written to and stays as it was for as long
// as it is used; every change goes through update(), made under the same lock, then saved.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/util.h"

// A web search service the user has set up (agent/search/). Generic fields only: what a kind of
// service needs is its provider's business. An entry of a kind this version does not know (a newer
// version wrote it) is kept as it is and passed over. One entry per kind: the settings page, the
// allowance and the search all know a service by its kind (a file with two of a kind keeps the first).
struct SearchService {
    std::string kind;            // as the providers' registry names it: "tavily"
    std::string key;
    bool enabled = true;
    json more;                   // fields this version does not read, written back as they came
    // Set when the service refused the key: a print of that key (never the key), so the key is not sent
    // again, by this run or a later one, until it is typed again (set_search_key clears it); another key
    // put in its place is not refused. Saved as "refused" (agent/search/search.cpp).
    std::string refused;
    // Which typing of the key this is, in this run only (0: as read): each set_search_key gives a new one, so
    // that a refusal of the key as sent before it was typed again marks nothing. Not saved.
    uint64_t typed = 0;
};

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

    // Web search: the services in the order they are tried. Saved as "search"; a file with the old
    // "search_key" (always a Tavily key) is read as one Tavily entry. ZB_SEARCH_KEY is the Tavily entry's
    // key, wherever it stands in the list (never another service's: a Tavily key must not go to Brave).
    std::vector<SearchService> search;
    std::string search_key(const std::string &kind) const;   // the key of the entry of that kind, "" if none
    // Sets that key, adding an entry of the kind at the end when there is none (and the key is not empty).
    // A key typed is a key to try: whatever the service refused before is forgotten.
    void set_search_key(const std::string &kind, const std::string &key);
    // Counts the changes to `search` in this run (any field of an entry, an entry added or removed, the order),
    // in memory only: what search keeps of an answer (its cache) is used only under the count it was made under.
    uint64_t search_revision = 0;

    // Device
    int volume = 70;
    int brightness = 80;
    int screen_off_min = 5;      // minutes unused before the screen goes off (against burn-in); 0 = never
    int mic_gain = 100;

    // Personalisation
    std::string agent_name = "Zero";
    std::string user_name;
    std::string home_city;
    std::string date_order = "dmy";   // "dmy" (3/4 is 3 April) or "mdy" (3/4 is March 4)
    bool month_first() const { return date_order == "mdy"; }
    bool mic_builtin = true;          // always record from the device's own microphone
    bool camera_flip = true;          // turn photos the right way up (the sensor is mounted inverted)
    std::string home_country;   // tells same-named cities apart (London, Canada)
    std::string timezone;        // IANA name; empty = system
    std::string units = "metric";
    std::string tv_brand = "auto";
    std::string music_dir;       // empty = ~/Music

    // The settings now, as a copy (cheap: a few dozen fields). Hold it for as long as one piece of
    // work needs consistent settings; call get() again for newer ones.
    static const Settings get();
    // Changes the settings: `change` runs under the lock (keep it to assigning fields: no I/O, no
    // other locks, no get() or update()), then the result is written to disk unless `save_now` is
    // false. Returns the settings as they are after the change. Saves made at the same time on
    // several threads leave the newest settings on disk.
    static Settings update(const std::function<void(Settings &)> &change, bool save_now = true);
    static void save();   // writes changes made with update(..., false) (the volume keys save after a pause)
    static void load();   // reads the file (and the ZB_* overrides) into the shared settings

    // Values with defaults applied.
    std::string eff_api_base() const;
    std::string eff_api_key() const;
    std::string eff_llm_model() const;
    std::string eff_vision_model() const;
    std::string eff_music_dir() const;
    bool configured() const { return !eff_api_base().empty() && !eff_api_key().empty(); }
    // Every key the settings hold (the AI service's and each search service's), those set: what a saved
    // conversation blanks out, wherever it appears.
    std::vector<std::string> secret_values() const;

    json to_json() const;
    void from_json(const json &j);

};
