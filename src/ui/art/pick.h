// Which scene of the art plays for what plays (ZB-059, ZB-133): a song's by its genre, a station's by its tags, and
// the city in its weather. No LVGL: tests/unit/art_test.cpp checks every rule. App::playing (app_status.cpp) asks.
#pragma once
#include <ctime>
#include <string>
#include <vector>

namespace art {

// The scene for a song of this genre (as the music index keeps it: one of ID3v1's names, or any text): "city" for
// chill, lo-fi and ambient, "electronic", "rock", "jazz" (and blues), "classical", and "pop" for anything else or
// none. Compared in lower case with "-", "_" and "." as spaces: first ID3v1's names, each in its family; else the
// first family, in that order, with a word that appears in it ("lo-fi hip hop" is chill, "symphonic metal" rock,
// "jazz rock" jazz, "pop rock" rock).
std::string scene_for_genre(const std::string &genre);

// The scene for a station with these tags: the city for one tagged focus, lofi, study, chill, ambient or downtempo
// (ignoring case), else the radio's mast.
std::string scene_for_station(const std::vector<std::string> &tags);

// The city's weather: rain or clear, chosen as the city comes on and kept until the city has played for 30 minutes
// in all with it, over as many times as it takes; the time it comes on after that, chosen again, rain one time in
// three (from the clock). It never changes while the city plays, nor from one time to the next within minutes.
class Weather {
public:
    static constexpr unsigned long kKeepMs = 30 * 60 * 1000ul;
    // The scene to play for `scene` (App::playing's) at `now` (the main loop's clock, ms), `clock` the wall clock that
    // picks the weather: the city ("city") in its weather, "city" or "city_rain"; any other as it is.
    std::string follow(const std::string &scene, unsigned long now, time_t clock);

private:
    bool chosen_ = false, rain_ = false, playing_ = false;
    unsigned long played_ = 0;   // the city's time with this weather
    unsigned long last_ = 0;     // the last call's `now`
};

}  // namespace art
