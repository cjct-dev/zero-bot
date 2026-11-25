// Dusk and night for the pixel-art scenes (ZB-132): whether it is night where the user is, worked out from
// the sun, with no network and no setting of its own. Night runs from 45 minutes after sunset to 45 minutes
// before sunrise, and the scenes move from one palette to the other over 20 minutes centred on each change.
//
// Where the sun is reckoned for, best first:
//   - the home city, from its coordinates (Settings::home_place), which the app looks up once by name
//     (tools::locate_home), unless the user has set the app's time zone to another zone with a city of its
//     own (travelling: the set_timezone tool), which says the user is not at home;
//   - the time zone's own city, from the zone database's table (zone1970.tab, else zone.tab), read once;
//   - fixed hours, night from 18:00 to 06:00 by the app's clock, where neither is known (Etc/UTC has no city).
// Pure: no LVGL and no network. Definitions in dusk.cpp.
#pragma once
#include <ctime>
#include <map>
#include <string>

struct Settings;

namespace dusk {

constexpr int kNightAfterSunsetMin = 45;    // night begins this long after sunset, and ends this long before sunrise
constexpr int kBlendMin = 20;               // each change takes this long, centred on that moment
constexpr int kDuskHour = 18, kDawnHour = 6;   // fixed hours: night from 18:00 to 06:00, by the app's clock

// Where the sun is reckoned for.
struct Where {
    enum class From { Hours, Zone, Home };
    From from = From::Hours;     // Hours: no coordinates, night by the clock
    double lat = 0, lon = 0;     // degrees, north and east positive (Zone and Home)
    std::string zone;            // the app's time zone (its IANA name), whose city Zone is
};

// The scenes' lighting at a moment.
struct Light {
    // The night palette is the nearer (blend at least a half): from 45 minutes after sunset to 45 minutes
    // before sunrise. For what switches rather than blends (the sky, the stars, lit windows).
    bool night = false;
    // How much of the night palette: 0 all dusk, 1 all night; in between only within ten minutes either side
    // of a change. Where the night is shorter than the change (a few days near polar day), it never reaches 1.
    float blend = 0;
};

Light light_at(time_t now, const Where &where);

// The light now, where the settings say the user is: what the scenes call, about once a second. Reads the
// settings' snapshot and the clock; the zone table is read once, on the first call that needs it.
Light now_light();
Where where_now();
Where where_for(const Settings &s);

// The sun on one day at a place: when its upper edge crosses the horizon, refraction included (NOAA's
// solar calculator, after Meeus: within a minute or so away from the poles). The day is the place's own
// (local mean time: the date at noon there). Where the sun does not set that day (polar day), rise and set
// are taken 12 hours either side of noon, so no night falls; where it does not rise (polar night), both
// at noon, where it comes nearest the horizon, so the hour and a half round noon is dusk, as the twilight
// there is.
struct Sun {
    enum class Day { Normal, PolarDay, PolarNight };
    Day day = Day::Normal;
    time_t rise = 0, noon = 0, set = 0;
};
Sun sun_on(int year, int month, int day, double lat, double lon);

// A zone table's coordinates by zone name: each line "codes<TAB>±DDMM±DDDMM<TAB>Zone[<TAB>comment]" (or
// ±DDMMSS±DDDMMSS), as zone1970.tab and zone.tab are written; comments and lines that do not read are
// passed over.
struct Coords {
    double lat = 0, lon = 0;
};
std::map<std::string, Coords> parse_zone_table(const std::string &text);
// One ISO 6709 pair as the tables write it ("+513030-0000731"); false for anything else.
bool parse_coords(const std::string &text, Coords &out);
// The zone's coordinates from the system's tables (zone1970.tab, then zone.tab for the zones it leaves out,
// such as Europe/Oslo), read once; false for a zone they do not list (Etc/UTC, UTC).
bool zone_coords(const std::string &zone, Coords &out);

}  // namespace dusk
