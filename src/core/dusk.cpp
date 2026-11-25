// Dusk and night from the sun (dusk.h).
#include "core/dusk.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "core/settings.h"
#include "core/util.h"

namespace dusk {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kHorizon = -0.833;           // the sun's upper edge on the horizon, refraction included (degrees)
constexpr double kJdAtEpoch = 2440587.5;      // the Julian day of 1970-01-01 at 0h UTC
constexpr long kDay = 86400;
constexpr const char *kZoneDir = "/usr/share/zoneinfo/";

double rad(double deg) { return deg * kPi / 180; }
double deg(double rad) { return rad * 180 / kPi; }

// Where the sun is at a moment (its Julian day): its declination in degrees and the equation of time in
// minutes (NOAA's solar calculator).
struct SunPos {
    double decl, eot;
};

SunPos sun_pos(double jd) {
    const double t = (jd - 2451545.0) / 36525.0;   // Julian centuries since J2000
    const double l0 = std::fmod(280.46646 + t * (36000.76983 + t * 0.0003032), 360.0);   // mean longitude
    const double m = 357.52911 + t * (35999.05029 - 0.0001537 * t);                       // mean anomaly
    const double e = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);                  // the orbit's eccentricity
    const double centre = std::sin(rad(m)) * (1.914602 - t * (0.004817 + 0.000014 * t)) +
                          std::sin(rad(2 * m)) * (0.019993 - 0.000101 * t) + std::sin(rad(3 * m)) * 0.000289;
    const double omega = 125.04 - 1934.136 * t;
    const double lambda = l0 + centre - 0.00569 - 0.00478 * std::sin(rad(omega));         // apparent longitude
    const double eps0 = 23 + (26 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60) / 60;
    const double eps = eps0 + 0.00256 * std::cos(rad(omega));                              // obliquity
    const double y = std::tan(rad(eps / 2)) * std::tan(rad(eps / 2));
    const double eot = 4 * deg(y * std::sin(2 * rad(l0)) - 2 * e * std::sin(rad(m)) +
                               4 * e * y * std::sin(rad(m)) * std::cos(2 * rad(l0)) - 0.5 * y * y * std::sin(4 * rad(l0)) -
                               1.25 * e * e * std::sin(2 * rad(m)));
    return {deg(std::asin(std::sin(rad(eps)) * std::sin(rad(lambda)))), eot};
}

// How far from noon the sun rises, as an hour angle in degrees, at a latitude and declination: 180 where it
// stays up all day, 0 where it stays down.
double rise_angle(double lat, double decl, Sun::Day &day) {
    const double c = (std::sin(rad(kHorizon)) - std::sin(rad(lat)) * std::sin(rad(decl))) /
                     (std::cos(rad(lat)) * std::cos(rad(decl)));
    if (c <= -1) { day = Sun::Day::PolarDay; return 180; }
    if (c >= 1) { day = Sun::Day::PolarNight; return 0; }
    day = Sun::Day::Normal;
    return deg(std::acos(c));
}

// Minutes after 0h UTC of the date (`jd0`, its Julian day at 0h) when the sun is at noon (side 0), rises
// (-1) or sets (+1) at the place. Worked out again with the sun as it is at the answer, as NOAA does, twice.
double event_minutes(double jd0, double lat, double lon, int side, Sun::Day &day) {
    double minutes = 720 - 4 * lon;
    for (int pass = 0; pass < 3; ++pass) {
        const SunPos p = sun_pos(jd0 + minutes / 1440);
        const double angle = rise_angle(lat, p.decl, day);
        minutes = 720 - 4 * lon - p.eot + side * 4 * angle;
    }
    return minutes;
}

Sun sun_on_day(long days, double lat, double lon) {
    lat = std::max(-89.9, std::min(89.9, lat));   // at a pole every direction is south (or north): no hour angle
    const double jd0 = days + kJdAtEpoch;
    Sun sun;
    Sun::Day ignored;
    const auto at = [days](double minutes) { return static_cast<time_t>(days * kDay + std::llround(minutes * 60)); };
    sun.noon = at(event_minutes(jd0, lat, lon, 0, sun.day));
    sun.rise = at(event_minutes(jd0, lat, lon, -1, ignored));
    sun.set = at(event_minutes(jd0, lat, lon, 1, ignored));
    return sun;
}

// How much of the night palette at `now`, for the night that falls `kNightAfterSunsetMin` after `set` and
// ends as long before `rise`: rising over kBlendMin centred on its start, falling over kBlendMin centred on
// its end. A night shorter than the change never reaches 1; one that does not fall at all gives 0.
float night_share(time_t now, time_t set, time_t rise) {
    const double half = kBlendMin * 30.0, start = set + kNightAfterSunsetMin * 60.0, end = rise - kNightAfterSunsetMin * 60.0;
    const double t = static_cast<double>(now);
    const double share = std::min(t - (start - half), (end + half) - t) / (2 * half);
    return static_cast<float>(std::max(0.0, std::min(1.0, share)));
}

// Fixed hours, by the app's clock: night from kDuskHour to kDawnHour, each change centred on the hour.
float clock_share(time_t now) {
    const util::LocalTime f = util::local_fields(now);
    const double minute = f.hour * 60 + f.minute + (now % 60) / 60.0, half = kBlendMin / 2.0;
    const double evening = (minute - (kDuskHour * 60 - half)) / kBlendMin, morning = ((kDawnHour * 60 + half) - minute) / kBlendMin;
    return static_cast<float>(std::max(0.0, std::min(1.0, std::max(evening, morning))));
}

// One half of a table's coordinates, "+5130" or "-0000731": a sign, the degrees in `digits` digits, minutes,
// and seconds when given.
bool parse_angle(const std::string &text, size_t digits, double &out) {
    if ((text.size() != digits + 3 && text.size() != digits + 5) || (text[0] != '+' && text[0] != '-')) return false;
    for (size_t i = 1; i < text.size(); ++i)
        if (!isdigit(static_cast<unsigned char>(text[i]))) return false;
    const int degrees = atoi(text.substr(1, digits).c_str()), minutes = atoi(text.substr(1 + digits, 2).c_str());
    const int seconds = text.size() > digits + 3 ? atoi(text.substr(3 + digits).c_str()) : 0;
    if (minutes > 59 || seconds > 59) return false;
    out = (text[0] == '-' ? -1 : 1) * (degrees + minutes / 60.0 + seconds / 3600.0);
    return true;
}

const std::map<std::string, Coords> &zone_table() {
    static const std::map<std::string, Coords> table = [] {
        std::map<std::string, Coords> out;
        std::string text;
        if (util::read_file(std::string(kZoneDir) + "zone1970.tab", text)) out = parse_zone_table(text);
        // zone.tab has one zone per country: those zone1970.tab merges into another country's (Europe/Oslo
        // into Europe/Berlin) keep their own city.
        if (util::read_file(std::string(kZoneDir) + "zone.tab", text))
            for (const auto &entry : parse_zone_table(text)) out.insert(entry);
        return out;
    }();
    return table;
}

// The app's time zone for the settings' `timezone`: the one set when it exists (as util::apply_timezone),
// else the system's, and its city's coordinates. Worked out once for each value of the setting (reading the
// zone's file and the table): the system's zone as it was then.
struct AppZone {
    std::string name;
    bool set_by_user = false;
    bool located = false;
    Coords at;
};

AppZone app_zone(const std::string &setting) {
    static std::mutex mtx;
    static std::map<std::string, AppZone> known;
    std::lock_guard<std::mutex> lk(mtx);
    const auto it = known.find(setting);
    if (it != known.end()) return it->second;
    AppZone z;
    z.set_by_user = util::timezone_exists(setting);
    z.name = z.set_by_user ? setting : util::system_timezone();
    z.located = zone_coords(z.name, z.at);
    return known[setting] = z;
}

}  // namespace

Light light_at(time_t now, const Where &where) {
    float blend;
    if (where.from == Where::From::Hours) {
        blend = clock_share(now);
    } else {
        // The place's own day (local mean time); the night before it and the one after.
        const long day = static_cast<long>(std::floor((static_cast<double>(now) + where.lon * 240) / kDay));
        const Sun before = sun_on_day(day - 1, where.lat, where.lon), today = sun_on_day(day, where.lat, where.lon),
                  after = sun_on_day(day + 1, where.lat, where.lon);
        blend = std::max(night_share(now, before.set, today.rise), night_share(now, today.set, after.rise));
    }
    return {blend >= 0.5f, blend};
}

Where where_for(const Settings &s) {
    const AppZone zone = app_zone(s.timezone);
    // A zone the user set, with a city of its own and not the home city's, says the user is elsewhere (travelling).
    // One with no city (Etc/UTC) says nothing of where they are.
    const bool elsewhere = zone.set_by_user && zone.located && zone.name != s.home_place.zone;
    if (s.home_place.known() && !elsewhere) return {Where::From::Home, s.home_place.lat, s.home_place.lon, zone.name};
    if (zone.located) return {Where::From::Zone, zone.at.lat, zone.at.lon, zone.name};
    return {Where::From::Hours, 0, 0, zone.name};
}

Where where_now() { return where_for(Settings::get()); }

Light now_light() { return light_at(time(nullptr), where_now()); }

Sun sun_on(int year, int month, int day, double lat, double lon) {
    struct tm date = {};
    date.tm_year = year - 1900;
    date.tm_mon = month - 1;
    date.tm_mday = day;
    return sun_on_day(static_cast<long>(timegm(&date) / kDay), lat, lon);
}

std::map<std::string, Coords> parse_zone_table(const std::string &text) {
    std::map<std::string, Coords> out;
    for (const std::string &line : util::split(text, '\n')) {
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> fields = util::split(line, '\t');
        Coords c;
        if (fields.size() >= 3 && !fields[2].empty() && parse_coords(fields[1], c)) out.emplace(fields[2], c);
    }
    return out;
}

bool parse_coords(const std::string &text, Coords &out) {
    const size_t split = text.find_first_of("+-", 1);
    if (split == std::string::npos) return false;
    Coords c;
    if (!parse_angle(text.substr(0, split), 2, c.lat) || !parse_angle(text.substr(split), 3, c.lon)) return false;
    if (std::fabs(c.lat) > 90 || std::fabs(c.lon) > 180) return false;
    out = c;
    return true;
}

bool zone_coords(const std::string &zone, Coords &out) {
    const auto &table = zone_table();
    const auto it = table.find(zone);
    if (it == table.end()) return false;
    out = it->second;
    return true;
}

}  // namespace dusk
