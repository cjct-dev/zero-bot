// Unit tests for core/dusk: sunrise and sunset against an almanac's times (timeanddate.com, to the
// minute, for the places' usual coordinates), polar day and night, the night's edges and the blend, fixed
// hours by the clock across a change of the clocks, the zone tables' coordinates, where the sun is reckoned
// for from the settings, and the home city's place in the settings (saved, forgotten when the city changes).
// No network; the system's zone tables are read for a few zones. Usage: dusk_test STATE_DIR.
#include "core/dusk.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

#include "core/settings.h"
#include "core/util.h"

using dusk::Light;
using dusk::Sun;
using dusk::Where;

namespace {

int g_passed = 0, g_failed = 0;

void check(bool ok, const std::string &what) {
    if (ok) ++g_passed;
    else { ++g_failed; printf("FAIL %s\n", what.c_str()); }
}

time_t utc(int y, int mo, int d, int h, int mi, int s = 0) {
    struct tm t = {};
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_sec = s;
    return timegm(&t);
}

std::string stamp(time_t t) {
    char buf[32];
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tmv);
    return buf;
}

constexpr long kMin = 60;
constexpr double kMaxErrorMin = 2.5;   // the almanac rounds to the minute; NOAA's formula is good to about one
double g_worst = 0;                    // the largest difference from the almanac, in minutes

void near_time(time_t got, time_t want, const std::string &what) {
    const double off = std::fabs(static_cast<double>(got - want)) / kMin;
    if (off > g_worst) g_worst = off;
    check(off <= kMaxErrorMin, what + ": " + stamp(got) + ", the almanac " + stamp(want));
}

void near(double got, double want, double within, const std::string &what) {
    check(std::fabs(got - want) <= within, what + ": " + std::to_string(got) + ", expected " + std::to_string(want));
}

struct Place {
    const char *name;
    double lat, lon;
};
const Place kSouthampton{"Southampton", 50.9, -1.4};
const Place kSydney{"Sydney", -33.867, 151.217};
const Place kTromso{"Tromso", 69.65, 18.95};

Where at(const Place &p) { return {Where::From::Home, p.lat, p.lon, ""}; }

// Each almanac row: the place's date, and its sunrise and sunset in UTC (the almanac lists local time).
void test_almanac() {
    struct Row {
        const Place &place;
        int y, m, d;
        time_t rise, set;
    } rows[] = {
        {kSouthampton, 2026, 12, 1, utc(2026, 12, 1, 7, 46), utc(2026, 12, 1, 16, 2)},        // GMT
        {kSouthampton, 2026, 12, 21, utc(2026, 12, 21, 8, 5), utc(2026, 12, 21, 16, 1)},
        {kSouthampton, 2026, 12, 31, utc(2026, 12, 31, 8, 8), utc(2026, 12, 31, 16, 8)},
        {kSouthampton, 2026, 6, 1, utc(2026, 6, 1, 3, 57), utc(2026, 6, 1, 20, 10)},          // BST: 04:57, 21:10
        {kSouthampton, 2026, 6, 21, utc(2026, 6, 21, 3, 51), utc(2026, 6, 21, 20, 23)},
        // Either side of the clocks going forward (29 March 2026, 01:00 UTC): 05:50 GMT, then 06:48 BST.
        {kSouthampton, 2026, 3, 28, utc(2026, 3, 28, 5, 50), utc(2026, 3, 28, 18, 31)},
        {kSouthampton, 2026, 3, 29, utc(2026, 3, 29, 5, 48), utc(2026, 3, 29, 18, 33)},
        {kSouthampton, 2026, 3, 30, utc(2026, 3, 30, 5, 46), utc(2026, 3, 30, 18, 34)},
        // Sydney in summer (AEDT, UTC+11: 05:37, 19:50 and 05:40, 20:05) and winter (AEST: 06:59, 16:53). Its
        // morning is the evening before in UTC.
        {kSydney, 2026, 12, 1, utc(2026, 11, 30, 18, 37), utc(2026, 12, 1, 8, 50)},
        {kSydney, 2026, 12, 21, utc(2026, 12, 20, 18, 40), utc(2026, 12, 21, 9, 5)},
        {kSydney, 2026, 6, 21, utc(2026, 6, 20, 20, 59), utc(2026, 6, 21, 6, 53)},
        // Tromso in March (CET): the sun moves slowly at the horizon this far north, the hardest case.
        {kTromso, 2026, 3, 1, utc(2026, 3, 1, 6, 10), utc(2026, 3, 1, 15, 44)},
        {kTromso, 2026, 3, 15, utc(2026, 3, 15, 5, 6), utc(2026, 3, 15, 16, 41)},
    };
    for (const Row &r : rows) {
        const Sun sun = dusk::sun_on(r.y, r.m, r.d, r.place.lat, r.place.lon);
        const std::string day = std::string(r.place.name) + " " + std::to_string(r.y) + "-" + std::to_string(r.m) + "-" +
                                std::to_string(r.d);
        check(sun.day == Sun::Day::Normal, day + ": the sun rises and sets");
        near_time(sun.rise, r.rise, day + " sunrise");
        near_time(sun.set, r.set, day + " sunset");
        check(sun.rise < sun.noon && sun.noon < sun.set, day + ": noon between the two");
    }
}

// Tromso: the midnight sun (about 20 May to 22 July) and the polar night (about 27 November to 15 January).
void test_polar() {
    const Sun summer = dusk::sun_on(2026, 6, 21, kTromso.lat, kTromso.lon);
    check(summer.day == Sun::Day::PolarDay, "Tromso at midsummer: the sun does not set");
    near(static_cast<double>(summer.set - summer.rise) / 3600, 24, 0.05, "Tromso at midsummer: rise and set a day apart");
    const Sun winter = dusk::sun_on(2026, 12, 21, kTromso.lat, kTromso.lon);
    check(winter.day == Sun::Day::PolarNight, "Tromso at midwinter: the sun does not rise");
    check(winter.rise == winter.noon && winter.set == winter.noon, "Tromso at midwinter: rise and set at noon");
    check(dusk::sun_on(2026, 5, 10, kTromso.lat, kTromso.lon).day == Sun::Day::Normal &&
              dusk::sun_on(2026, 5, 25, kTromso.lat, kTromso.lon).day == Sun::Day::PolarDay &&
              dusk::sun_on(2026, 7, 30, kTromso.lat, kTromso.lon).day == Sun::Day::Normal,
          "Tromso: the midnight sun begins in May and is over by the end of July");
    check(dusk::sun_on(2026, 11, 20, kTromso.lat, kTromso.lon).day == Sun::Day::Normal &&
              dusk::sun_on(2026, 12, 5, kTromso.lat, kTromso.lon).day == Sun::Day::PolarNight &&
              dusk::sun_on(2027, 1, 20, kTromso.lat, kTromso.lon).day == Sun::Day::Normal,
          "Tromso: the polar night begins late in November and is over by late January");
    check(dusk::sun_on(2026, 12, 21, -kTromso.lat, kTromso.lon).day == Sun::Day::PolarDay,
          "as far south, the December sun does not set");

    // Polar day: never night, midnight included (22:00 UTC is midnight in Tromso in summer).
    for (time_t t = utc(2026, 6, 21, 0, 0); t < utc(2026, 6, 22, 0, 0); t += 10 * kMin)
        if (dusk::light_at(t, at(kTromso)).blend != 0) { check(false, "Tromso at midsummer, " + stamp(t) + ": not dusk"); break; }
    // Polar night: night but for the hour and a half round noon, where the sun comes nearest the horizon.
    const Light midnight = dusk::light_at(utc(2026, 12, 21, 23, 0), at(kTromso));
    check(midnight.night && midnight.blend == 1, "Tromso at midwinter, midnight: night");
    const Light noon = dusk::light_at(winter.noon, at(kTromso));
    check(!noon.night && noon.blend == 0, "Tromso at midwinter, noon: dusk");
    check(dusk::light_at(winter.noon - 60 * kMin, at(kTromso)).blend == 1, "Tromso at midwinter, an hour before noon: night");
    check(dusk::light_at(winter.noon - 45 * kMin, at(kTromso)).night, "Tromso at midwinter, 45 minutes before noon: night begins to end");
    check(!dusk::light_at(winter.noon - 44 * kMin, at(kTromso)).night, "Tromso at midwinter, 44 minutes before noon: dusk");
}

// The blend, minute by minute, from `from` for `days` days at a place: at most a change's worth between two minutes.
bool smooth_at(double lat, double lon, time_t from, int days) {
    const Where w{Where::From::Zone, lat, lon, ""};
    float last = dusk::light_at(from, w).blend;
    for (time_t t = from + kMin; t < from + days * 86400; t += kMin) {
        const float now = dusk::light_at(t, w).blend;
        if (std::fabs(now - last) > 1.0 / dusk::kBlendMin + 1e-4) return false;
        last = now;
    }
    return true;
}

// Every place through a year, the poles included: a blend from 0 to 1, and night exactly when it is past a half;
// minute by minute where the days change most (the polar night and day beginning, the date line), no jump.
void test_everywhere() {
    const double lats[] = {90, 89.95, 78.2, 69.65, 66.56, 50.9, 0, -33.9, -66.56, -78, -90};
    const double lons[] = {-180, -1.4, 18.95, 151.2, 180};
    bool sane = true, consistent = true;
    for (double lat : lats)
        for (double lon : lons)
            for (time_t t = utc(2026, 1, 1, 0, 0); t < utc(2027, 1, 1, 0, 0); t += 7 * 3600 + 13 * kMin) {
                const Light l = dusk::light_at(t, {Where::From::Zone, lat, lon, ""});
                if (!(l.blend >= 0 && l.blend <= 1)) sane = false;
                if (l.night != (l.blend >= 0.5f)) consistent = false;
            }
    check(sane, "everywhere through 2026: a blend from 0 to 1");
    check(consistent, "everywhere: night when the blend is at least a half");
    check(smooth_at(kTromso.lat, kTromso.lon, utc(2026, 11, 24, 0, 0), 5), "Tromso as the polar night begins: no jump");
    check(smooth_at(kTromso.lat, kTromso.lon, utc(2026, 5, 16, 0, 0), 5), "Tromso as the midnight sun begins: no jump");
    check(smooth_at(kSouthampton.lat, kSouthampton.lon, utc(2026, 12, 20, 0, 0), 3), "Southampton in December: no jump");
    check(smooth_at(kSydney.lat, kSydney.lon, utc(2026, 12, 20, 0, 0), 3), "Sydney in December: no jump");
    check(smooth_at(0, 180, utc(2026, 3, 19, 0, 0), 3) && smooth_at(0, -180, utc(2026, 3, 19, 0, 0), 3), "at the date line: no jump");
    check(smooth_at(89.95, 0, utc(2026, 3, 17, 0, 0), 6), "at the pole as the sun comes up for the summer: no jump");
}

// The night's edges at Southampton on 21 December: 45 minutes after sunset and before sunrise, each change
// over 20 minutes centred there.
void test_edges() {
    const Sun today = dusk::sun_on(2026, 12, 21, kSouthampton.lat, kSouthampton.lon);
    const Sun tomorrow = dusk::sun_on(2026, 12, 22, kSouthampton.lat, kSouthampton.lon);
    const Where w = at(kSouthampton);
    const time_t start = today.set + 45 * kMin, end = tomorrow.rise - 45 * kMin;
    auto blend = [&w](time_t t) { return dusk::light_at(t, w).blend; };
    auto night = [&w](time_t t) { return dusk::light_at(t, w).night; };
    check(blend(today.set) == 0 && blend(start - 10 * kMin) == 0, "at sunset and until 35 minutes after: dusk");
    near(blend(start - 5 * kMin), 0.25, 1e-3, "40 minutes after sunset: a quarter of the way");
    near(blend(start), 0.5, 1e-3, "45 minutes after sunset: half way");
    check(!night(start - 1) && night(start), "night begins 45 minutes after sunset");
    near(blend(start + 5 * kMin), 0.75, 1e-3, "50 minutes after sunset: three quarters");
    check(blend(start + 10 * kMin) == 1 && blend(utc(2026, 12, 22, 0, 0)) == 1, "from 55 minutes after sunset: night");
    check(blend(end - 10 * kMin) == 1, "55 minutes before sunrise: still night");
    near(blend(end), 0.5, 1e-3, "45 minutes before sunrise: half way");
    check(night(end) && !night(end + 1), "night ends 45 minutes before sunrise");
    near(blend(end + 5 * kMin), 0.25, 1e-3, "40 minutes before sunrise: a quarter");
    check(blend(end + 10 * kMin) == 0 && blend(tomorrow.rise) == 0 && blend(tomorrow.noon) == 0, "from 35 minutes before sunrise: dusk");
    // The same night seen from either side of midnight UTC, and the morning before it.
    check(blend(utc(2026, 12, 21, 3, 0)) == 1 && blend(utc(2026, 12, 21, 12, 0)) == 0, "the night before, and noon");
    // Sydney: a night that is the day before in UTC.
    const Where sydney = at(kSydney);
    check(dusk::light_at(utc(2026, 12, 21, 3, 0), sydney).blend == 0, "Sydney at 14:00 in December: dusk");
    check(dusk::light_at(utc(2026, 12, 21, 13, 0), sydney).night, "Sydney at midnight in December: night");
}

// Fixed hours, by the app's clock: night from 18:00 to 06:00, the same hours either side of the clocks changing.
void test_hours() {
    util::apply_timezone("Europe/London");
    const Where hours;
    auto blend = [&hours](time_t t) { return dusk::light_at(t, hours).blend; };
    check(blend(utc(2026, 3, 28, 12, 0)) == 0 && blend(utc(2026, 3, 28, 17, 50)) == 0, "fixed hours: dusk until 17:50");
    near(blend(utc(2026, 3, 28, 18, 0)), 0.5, 1e-3, "fixed hours: half way at 18:00 GMT");
    check(dusk::light_at(utc(2026, 3, 28, 18, 0), hours).night, "fixed hours: night from 18:00");
    check(blend(utc(2026, 3, 28, 18, 10)) == 1 && blend(utc(2026, 3, 29, 0, 30)) == 1, "fixed hours: night after 18:10");
    // 29 March: the clocks go forward at 01:00 UTC. Morning and evening move with them.
    check(blend(utc(2026, 3, 29, 1, 30)) == 1, "fixed hours: night through the change of the clocks");
    near(blend(utc(2026, 3, 29, 5, 0)), 0.5, 1e-3, "fixed hours: half way at 06:00 BST (05:00 UTC)");
    check(blend(utc(2026, 3, 29, 5, 10)) == 0 && blend(utc(2026, 3, 29, 16, 50)) == 0, "fixed hours: dusk from 06:10 to 17:50 BST");
    near(blend(utc(2026, 3, 29, 17, 0)), 0.5, 1e-3, "fixed hours: half way at 18:00 BST (17:00 UTC)");
    // 25 October: back at 01:00 UTC.
    near(blend(utc(2026, 10, 25, 6, 0)), 0.5, 1e-3, "fixed hours: half way at 06:00 GMT after the clocks go back");
    near(blend(utc(2026, 10, 24, 5, 0)), 0.5, 1e-3, "fixed hours: half way at 06:00 BST the day before");
    bool smooth = true;
    for (time_t t = utc(2026, 3, 28, 12, 0); t < utc(2026, 3, 30, 12, 0); t += kMin)
        if (std::fabs(blend(t) - blend(t + kMin)) > 1.0 / dusk::kBlendMin + 1e-4) smooth = false;
    for (time_t t = utc(2026, 10, 24, 12, 0); t < utc(2026, 10, 26, 12, 0); t += kMin)
        if (std::fabs(blend(t) - blend(t + kMin)) > 1.0 / dusk::kBlendMin + 1e-4) smooth = false;
    check(smooth, "fixed hours: no jump across the clocks' changes");
    // The sun's own times are moments, not clock times: at Southampton the night either side of the change.
    bool sun_smooth = true;
    for (time_t t = utc(2026, 3, 28, 12, 0); t < utc(2026, 3, 30, 12, 0); t += kMin)
        if (std::fabs(dusk::light_at(t, at(kSouthampton)).blend - dusk::light_at(t + kMin, at(kSouthampton)).blend) >
            1.0 / dusk::kBlendMin + 1e-4)
            sun_smooth = false;
    check(sun_smooth, "Southampton: no jump across the clocks' change");
    check(dusk::light_at(utc(2026, 3, 29, 1, 30), at(kSouthampton)).night, "Southampton: night as the clocks change");
    util::apply_timezone("");
}

void test_tables() {
    dusk::Coords c;
    check(dusk::parse_coords("+513030-0000731", c), "seconds given: read");
    near(c.lat, 51.50833, 1e-4, "+513030: latitude");
    near(c.lon, -0.12528, 1e-4, "-0000731: longitude");
    check(dusk::parse_coords("-3352+15113", c), "minutes only: read");
    near(c.lat, -33.86667, 1e-4, "-3352: latitude");
    near(c.lon, 151.21667, 1e-4, "+15113: longitude");
    for (const char *bad : {"", "+5130", "5130-00007", "+5130-0007", "+513-00007", "+5160+00000", "+5130+00060", "+9130+00000",
                            "+5130+18100", "+5130+0000a", "+51300+00000", "+5130 +00000"})
        check(!dusk::parse_coords(bad, c), std::string("not coordinates: '") + bad + "'");

    const std::string table =
        "# tzdb timezone descriptions\n#codes\tcoordinates\tTZ\tcomments\n"
        "AD\t+4230+00131\tEurope/Andorra\n"
        "AE,OM,RE,SC,TF\t+2518+05518\tAsia/Dubai\tCrozet\n"
        "GB,GG,IM,JE\t+513030-0000731\tEurope/London\n"
        "XX\tsomewhere\tNowhere/Bad\n"
        "YY\t+0000+00000\n"
        "\n#@SJ\tArctic/\n";
    const auto zones = dusk::parse_zone_table(table);
    check(zones.size() == 3, "a table: three zones read, comments and bad lines passed over (" + std::to_string(zones.size()) + ")");
    check(zones.count("Asia/Dubai") && std::fabs(zones.at("Asia/Dubai").lon - 55.3) < 1e-6, "a table: a zone of several countries");

    // The system's tables.
    check(dusk::zone_coords("Europe/London", c) && std::fabs(c.lat - 51.508) < 0.01, "the system's table: Europe/London");
    check(dusk::zone_coords("Australia/Sydney", c) && c.lat < -33, "the system's table: Australia/Sydney");
    check(dusk::zone_coords("Europe/Oslo", c) && std::fabs(c.lat - 59.917) < 0.01,
          "Europe/Oslo, which zone1970.tab merges into Berlin: its own city from zone.tab");
    for (const char *none : {"Etc/UTC", "UTC", "Etc/GMT+5", "", "Australia", "../etc/passwd"})
        check(!dusk::zone_coords(none, c), std::string("no coordinates for '") + none + "'");
}

// Where the sun is reckoned for, from the settings.
void test_where() {
    Settings s;
    s.timezone = "Etc/UTC";
    Where w = dusk::where_for(s);
    check(w.from == Where::From::Hours && w.zone == "Etc/UTC", "Etc/UTC and no home city: fixed hours");
    s.timezone = "Europe/London";
    w = dusk::where_for(s);
    check(w.from == Where::From::Zone && std::fabs(w.lat - 51.508) < 0.01, "Europe/London: the zone's city");
    s.home_place = {-33.87, 151.21, "Australia/Sydney"};
    s.timezone = "";
    check(dusk::where_for(s).from == Where::From::Home, "a home city, the system's zone: the home city");
    s.timezone = "Etc/UTC";
    w = dusk::where_for(s);
    check(w.from == Where::From::Home && std::fabs(w.lat + 33.87) < 1e-9, "a home city, a zone with no city: the home city");
    s.timezone = "Australia/Sydney";
    check(dusk::where_for(s).from == Where::From::Home, "a home city, its own zone set: the home city");
    s.timezone = "Asia/Tokyo";
    w = dusk::where_for(s);
    check(w.from == Where::From::Zone && w.zone == "Asia/Tokyo" && w.lat > 35, "a home city, another zone set (travelling): that zone's city");
    s.timezone = "Not/AZone";
    check(dusk::where_for(s).from == Where::From::Home, "a home city, a zone that does not exist (the system's): the home city");
    s.home_place = HomePlace();
    s.timezone = "Asia/Tokyo";
    check(dusk::where_for(s).from == Where::From::Zone, "no home place: the zone's city");
}

// The home city's place in the settings: saved, read back with checks, and forgotten when the city or country changes.
void test_settings(const std::string &dir) {
    setenv("XDG_CONFIG_HOME", (dir + "/config").c_str(), 1);
    Settings::load();
    check(!Settings::get().home_place.known(), "no settings file: no home place");
    Settings::update([](Settings &s) { s.home_city = "Southampton"; s.home_country = "United Kingdom"; });
    Settings::update([](Settings &s) { s.home_place = {50.9, -1.4, "Europe/London"}; });
    Settings::update([](Settings &s) { s.volume = 40; s.timezone = "Europe/Paris"; });
    check(Settings::get().home_place.known(), "another setting changed: the home place kept");
    Settings::update([](Settings &s) { s.home_city = "Southampton"; });
    check(Settings::get().home_place.known(), "the home city set to what it was: kept");
    const json saved = util::load_json(dir + "/config/zero-bot/settings.json", json::object());
    check(saved.value("home_place", json()) == json{{"lat", 50.9}, {"lon", -1.4}, {"zone", "Europe/London"}}, "saved: " + saved.dump());
    Settings::load();
    check(Settings::get().home_place.known() && Settings::get().home_place.zone == "Europe/London", "read back");
    Settings::update([](Settings &s) { s.home_country = "Canada"; });
    check(!Settings::get().home_place.known() && Settings::get().home_place.zone.empty(), "the country changed: forgotten");
    Settings::update([](Settings &s) { s.home_place = {50.9, -1.4, ""}; });
    Settings::update([](Settings &s) { s.home_city = "Sydney"; });
    check(!Settings::get().home_place.known(), "the city changed: forgotten");
    const json after = util::load_json(dir + "/config/zero-bot/settings.json", json::object());
    check(after.contains("home_place") && after["home_place"].is_null(), "forgotten: saved as null");

    Settings s;
    s.home_place = {1, 2, "x"};
    s.from_json(json::object());
    check(s.home_place.known(), "a file without home_place: the place as it was");
    for (const json &bad : {json(), json("x"), json{{"lat", 91}, {"lon", 0}}, json{{"lat", 10}}, json{{"lat", "10"}, {"lon", 0}},
                            json{{"lat", 10}, {"lon", -180.5}}}) {
        s.home_place = {1, 2, "x"};
        s.from_json(json{{"home_place", bad}});
        check(!s.home_place.known(), "home_place " + bad.dump() + ": not known");
    }
    s.from_json(json{{"home_place", {{"lat", -90}, {"lon", 180}}}});
    check(s.home_place.known(), "the edges of the globe: known");
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: dusk_test STATE_DIR\n"); return 2; }
    util::make_dirs(argv[1]);
    test_almanac();
    test_polar();
    test_everywhere();
    test_edges();
    test_hours();
    test_tables();
    test_where();
    test_settings(argv[1]);
    printf("dusk: %d passed, %d failed; sunrise and sunset within %.1f minutes of the almanac\n", g_passed, g_failed, g_worst);
    return g_failed ? 1 : 0;
}
