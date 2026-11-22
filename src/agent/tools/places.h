// Places by name, for the weather and the time tools: the geocoding service, and the time-zone database's
// countries of several zones. Definitions in places.cpp.
#pragma once
#include <string>

#include "agent/tools.h"

namespace tools {

struct Place {
    std::string name, country, timezone;
    double lat = 0, lon = 0;
};

// The place a name means, preferring one in `country` when given; false with `err` set when none is found.
bool geocode(const std::string &query, Place &out, std::string &err, Context &ctx, const std::string &country = "");

// Whether `place` names a country of several time zones ("Australia", "US"), and the error that says so.
bool several_zones(std::string place);
std::string several_zones_error(const std::string &place);

}  // namespace tools
