// The weather tool: the weather now and a daily forecast for a place.
#include "agent/tools/common.h"

#include <cstdio>

#include "agent/experiment.h"
#include "agent/tools/places.h"
#include "agent/tools/web.h"
#include "core/settings.h"

namespace tools {

namespace {

const char *weather_text(int code) {
    switch (code) {
        case 0: return "clear sky";
        case 1: return "mainly clear";
        case 2: return "partly cloudy";
        case 3: return "overcast";
        case 45: case 48: return "fog";
        case 51: case 53: case 55: return "drizzle";
        case 56: case 57: return "freezing drizzle";
        case 61: return "light rain";
        case 63: return "rain";
        case 65: return "heavy rain";
        case 66: case 67: return "freezing rain";
        case 71: return "light snow";
        case 73: return "snow";
        case 75: return "heavy snow";
        case 77: return "snow grains";
        case 80: case 81: return "rain showers";
        case 82: return "violent rain showers";
        case 85: case 86: return "snow showers";
        case 95: return "thunderstorm";
        case 96: case 99: return "thunderstorm with hail";
        default: return "unknown conditions";
    }
}

std::string fmt1(double v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f", v);
    return buf;
}

std::string t_weather(const json &a, Context &ctx) {
    std::string where = str_arg(a, "location");
    std::string country;
    if (where.empty()) { where = Settings::get().home_city; country = Settings::get().home_country; }
    if (where.empty()) return fail("no location given and no home city is set; ask the user where they are");
    double days_d = 3;
    num_arg(a, "days", days_d);
    const int days = clamp_int(days_d, 1, 7);
    Place p;
    std::string err;
    if (!geocode(where, p, err, ctx, country)) return fail(err);
    const bool imperial = Settings::get().units == "imperial";
    char url[512];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&timezone=auto&forecast_days=%d"
             "&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m,relative_humidity_2m"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset%s",
             p.lat, p.lon, days, imperial ? "&temperature_unit=fahrenheit&wind_speed_unit=mph" : "");
    const json j = fetch_json(experiment::web(url), err, ctx);
    if (!err.empty()) return fail(err);
    const std::string deg = imperial ? "F" : "C", wind = imperial ? " mph" : " km/h";
    json out = {{"place", p.name + (p.country.empty() ? "" : ", " + p.country)}};
    if (j.contains("current")) {
        const json &c = j["current"];
        out["now"] = std::string(weather_text(jget(c, "weather_code", -1))) + ", " +
                     fmt1(jget(c, "temperature_2m", 0.0)) + deg + " (feels like " +
                     fmt1(jget(c, "apparent_temperature", 0.0)) + deg + "), wind " +
                     fmt1(jget(c, "wind_speed_10m", 0.0)) + wind + ", humidity " +
                     std::to_string(jget(c, "relative_humidity_2m", 0)) + "%";
    }
    if (j.contains("daily") && j["daily"].contains("time")) {
        const json &d = j["daily"];
        json arr = json::array();
        for (size_t i = 0; i < d["time"].size(); ++i) {
            auto at = [&](const char *k) -> json { return d.contains(k) && i < d[k].size() ? d[k][i] : json(); };
            const json hi = at("temperature_2m_max"), lo = at("temperature_2m_min"), rain = at("precipitation_probability_max");
            // A day without a rain chance says so: left out, the model filled the gap with a figure
            // of its own ("60% or more") and set a reminder that hung on it (scored case
            // review_rain_missing, three runs in three).
            arr.push_back(d["time"][i].get<std::string>() + ": " +
                          weather_text(at("weather_code").is_number() ? at("weather_code").get<int>() : -1) + ", " +
                          (lo.is_number() ? fmt1(lo.get<double>()) : "?") + " to " +
                          (hi.is_number() ? fmt1(hi.get<double>()) : "?") + deg +
                          (rain.is_number() ? ", rain chance " + std::to_string(rain.get<int>()) + "%" : ", rain chance not given"));
        }
        out["forecast"] = arr;
    }
    return out.dump();
}

}  // namespace

Tool make_weather() {
    return {"weather", "Checking the weather",
            "Current weather and a daily forecast for a place.",
            obj({{"location", S("City or town, optionally with country. Omit to use the user's home city.")},
                 {"days", N("Forecast length in days, 1 to 7 (default 3)")}}), t_weather};
}

}  // namespace tools
