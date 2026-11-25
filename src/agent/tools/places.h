// Places by name, for the weather and the time tools and the home city's place (the scenes' dusk and night):
// the geocoding service, and the time-zone database's countries of several zones. Definitions in places.cpp.
#pragma once
#include <string>

#include "agent/tools.h"

namespace tools {

struct Place {
    std::string name, country, timezone;
    double lat = 0, lon = 0;
};

// The place a name means, preferring one in `country` when given; false with `err` set when none is found.
// `unknown`, when given, says whether the service answered and lists no such place (not: no answer).
bool geocode(const std::string &query, Place &out, std::string &err, Context &ctx, const std::string &country = "",
             bool *unknown = nullptr);

// How a look-up of the home city went (locate_home).
enum class Located { Found, Unknown, Unreached };

// The home city's coordinates and zone (Settings::home_place, for core/dusk.h), looked up as the weather tool
// looks the city up: its name is all the geocoding service is sent, and `country` only chooses among the places it
// lists. Kept only while the settings' home city and country are still these (a city changed meanwhile is looked up
// in its turn). Unknown: the service answered and knows no such place, or gave one off the globe.
Located locate_home(const std::string &city, const std::string &country, Context &ctx);

// Whether `place` names a country of several time zones ("Australia", "US"), and the error that says so.
bool several_zones(std::string place);
std::string several_zones_error(const std::string &place);

}  // namespace tools
