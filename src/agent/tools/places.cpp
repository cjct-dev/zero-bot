// Places by name, and the time zones they are in (places.h).
#include "agent/tools/places.h"
#include "agent/tools/common.h"

#include <cmath>

#include <sys/stat.h>

#include "agent/experiment.h"
#include "agent/tools/web.h"
#include "core/settings.h"
#include "net/http.h"

namespace tools {

namespace {

// True when a geocoding hit lies in the country or region the user named ("canada", "UK", "texas").
bool in_region(const json &r, const std::string &region_in) {
    std::string region = util::lower(util::trim(region_in));
    if (region.empty()) return false;
    static const std::pair<const char *, const char *> kAliases[] = {
        {"uk", "gb"}, {"britain", "gb"}, {"great britain", "gb"}, {"usa", "us"}, {"america", "us"},
        {"united states of america", "us"}, {"uae", "ae"}, {"holland", "nl"},
    };
    for (const auto &alias : kAliases)
        if (region == alias.first) region = alias.second;
    return util::lower(jget(r, "country_code", std::string())) == region ||
           (region.size() > 2 && (util::contains_ci(jget(r, "country", std::string()), region) ||
                                  util::contains_ci(jget(r, "admin1", std::string()), region)));
}

}  // namespace

// `country` narrows a bare city name to one country; it is only a preference, so a city that
// does not exist there still resolves to its best-known namesake.
bool geocode(const std::string &query, Place &out, std::string &err, Context &ctx, const std::string &country, bool *unknown) {
    const std::string base = "https://geocoding-api.open-meteo.com/v1/search?language=en&count=";
    json j = fetch_json(experiment::web(base + (country.empty() ? "1" : "10") + "&name=" + http::url_encode(query)), err, ctx);
    if (!err.empty()) return false;
    size_t pick = 0;
    if (!country.empty() && j.contains("results") && j["results"].is_array())
        for (size_t i = 0; i < j["results"].size(); ++i)
            if (in_region(j["results"][i], country)) { pick = i; break; }
    // Spoken requests arrive as "paris france" or "portland oregon", which the service does not
    // match as one name. Search for the leading words and prefer the hit in the region named.
    std::string name = query;
    for (char &c : name)
        if (c == ',') c = ' ';
    name = util::trim(name);
    // A region is a word or three ("new south wales"); a long string of words would otherwise cost
    // one more request per word.
    int tries = 0;
    for (size_t cut = name.rfind(' '); (!j.contains("results") || j["results"].empty()) && cut != std::string::npos && cut > 0 &&
                                       tries < 3;
         cut = name.rfind(' ', cut - 1), ++tries) {
        const std::string region = util::trim(name.substr(cut + 1));
        j = fetch_json(experiment::web(base + "8&name=" + http::url_encode(util::trim(name.substr(0, cut)))), err, ctx);
        if (!err.empty()) return false;
        if (!j.contains("results") || !j["results"].is_array()) continue;
        for (size_t i = 0; i < j["results"].size(); ++i) {
            if (in_region(j["results"][i], region)) { pick = i; break; }
        }
    }
    if (!j.contains("results") || !j["results"].is_array() || j["results"].empty()) {
        err = "could not find a place called '" + query + "'. No weather or time is available for it; ask the user "
              "which town or city they mean";
        if (unknown) *unknown = true;
        return false;
    }
    const json &r = j["results"][pick];
    // Names from the network are data for the model: no control characters, and short.
    out.name = util::truncate_utf8(clean_text(jget(r, "name", query)), 80);
    out.country = util::truncate_utf8(clean_text(jget(r, "country", std::string())), 60);
    out.timezone = jget(r, "timezone", std::string());
    out.lat = jget(r, "latitude", 0.0);
    out.lon = jget(r, "longitude", 0.0);
    return true;
}

Located locate_home(const std::string &city, const std::string &country, Context &ctx) {
    Place p;
    std::string err;
    bool unknown = false;
    if (!geocode(city, p, err, ctx, country, &unknown)) return unknown ? Located::Unknown : Located::Unreached;
    if (!(std::fabs(p.lat) <= 90 && std::fabs(p.lon) <= 180)) return Located::Unknown;
    const HomePlace place{p.lat, p.lon, util::timezone_exists(p.timezone) ? p.timezone : ""};
    Settings::update([&](Settings &s) {
        if (s.home_city == city && s.home_country == country) s.home_place = place;
    });
    return Located::Found;
}

// "Australia", "Brazil", "Canada", "US": the zone database has a folder of that name because the
// country spans several zones. Neither one zone nor a town that happens to share the name will do.
bool several_zones(std::string place) {
    if (place.empty() || place.find("..") != std::string::npos || place[0] == '/') return false;
    auto is_dir = [](const std::string &name) {
        struct stat st;
        return stat(("/usr/share/zoneinfo/" + name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    };
    if (is_dir(place)) return true;
    place = util::lower(place);
    if (place.size() == 2) return is_dir(util::lower(place) == "us" ? "US" : place);
    place[0] = static_cast<char>(toupper(static_cast<unsigned char>(place[0])));
    return is_dir(place);
}

std::string several_zones_error(const std::string &place) {
    return fail(place + " has more than one time zone; ask the user which city or state they mean");
}

}  // namespace tools
