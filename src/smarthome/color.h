// Colour names and conversions shared by every light driver. The smart-home layer speaks hue
// (0..360) and saturation (0..100) for colours and kelvin for shades of white (driver.h); each
// driver converts to what its devices take: mireds, RGB, or CIE 1931 xy.
//
// Formulas: the sRGB transfer function and the wide-gamut RGB/XYZ matrices published for Hue
// lights; Kim et al. (2002) for the Planckian locus; McCamy (1992) for xy to kelvin; the usual
// curve fit of the black-body colour for kelvin to RGB. Header-only so that a driver built on
// its own needs nothing else from the app.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

namespace color {

struct Named {
    const char *name = "";
    int kelvin = 0;          // a shade of white when non-zero
    int hue = 0;             // 0..360
    int saturation = 0;      // 0..100
};

// The names the assistant may use, whites first.
inline const std::vector<Named> &names() {
    static const std::vector<Named> kNames = {
        {"candlelight", 2000, 0, 0}, {"warm white", 2700, 0, 0}, {"soft white", 3000, 0, 0}, {"white", 4000, 0, 0},
        {"cool white", 4000, 0, 0},  {"daylight", 6500, 0, 0},
        {"red", 0, 0, 100},          {"orange", 0, 30, 100},     {"amber", 0, 45, 100},      {"yellow", 0, 60, 100},
        {"lime", 0, 90, 100},        {"green", 0, 120, 100},     {"turquoise", 0, 174, 72},  {"cyan", 0, 180, 100},
        {"sky blue", 0, 197, 43},    {"blue", 0, 240, 100},      {"purple", 0, 275, 100},    {"violet", 0, 270, 45},
        {"magenta", 0, 300, 100},    {"pink", 0, 330, 50},       {"lavender", 0, 260, 30},
    };
    return kNames;
}

// Finds a colour by name, forgiving case, spacing and a few common other words for the same.
inline bool lookup(const std::string &said, Named &out) {
    std::string n;
    for (const char c : said) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (isalpha(u)) n += static_cast<char>(tolower(u));
        else if (!n.empty() && n.back() != ' ') n += ' ';
    }
    while (!n.empty() && n.back() == ' ') n.pop_back();
    static const std::pair<const char *, const char *> kOther[] = {
        {"warm", "warm white"}, {"warmwhite", "warm white"}, {"cool", "cool white"}, {"cold white", "cool white"},
        {"coolwhite", "cool white"}, {"day light", "daylight"}, {"candle", "candlelight"}, {"candle light", "candlelight"},
        {"neutral white", "white"}, {"neutral", "white"}, {"soft", "soft white"}, {"gold", "amber"}, {"teal", "turquoise"},
        {"light blue", "sky blue"}, {"aqua", "cyan"}, {"fuchsia", "magenta"}, {"warm light", "warm white"},
        {"cool light", "cool white"}};
    for (const auto &o : kOther)
        if (n == o.first) n = o.second;
    for (const auto &c : names())
        if (n == c.name) { out = c; return true; }
    return false;
}

inline int mired_from_kelvin(int kelvin) { return kelvin > 0 ? (1000000 + kelvin / 2) / kelvin : 0; }
inline int kelvin_from_mired(int mired) { return mired > 0 ? (1000000 + mired / 2) / mired : 0; }

inline int clamp255(double v) { return static_cast<int>(std::lround(std::max(0.0, std::min(255.0, v)))); }

// Hue 0..360, saturation and value 0..100 to 8-bit RGB.
inline void hs_to_rgb(double hue, double sat, double val, int &r, int &g, int &b) {
    const double h = std::fmod(std::fmod(hue, 360.0) + 360.0, 360.0) / 60.0;
    const double s = std::max(0.0, std::min(100.0, sat)) / 100.0, v = std::max(0.0, std::min(100.0, val)) / 100.0;
    const double c = v * s, x = c * (1 - std::fabs(std::fmod(h, 2.0) - 1)), m = v - c;
    double rr = 0, gg = 0, bb = 0;
    switch (static_cast<int>(h)) {
        case 0: rr = c; gg = x; break;
        case 1: rr = x; gg = c; break;
        case 2: gg = c; bb = x; break;
        case 3: gg = x; bb = c; break;
        case 4: rr = x; bb = c; break;
        default: rr = c; bb = x; break;
    }
    r = clamp255((rr + m) * 255);
    g = clamp255((gg + m) * 255);
    b = clamp255((bb + m) * 255);
}

// 8-bit RGB to hue 0..360 and saturation and value 0..100.
inline void rgb_to_hs(int r, int g, int b, double &hue, double &sat, double &val) {
    const double rr = r / 255.0, gg = g / 255.0, bb = b / 255.0;
    const double mx = std::max(rr, std::max(gg, bb)), mn = std::min(rr, std::min(gg, bb)), d = mx - mn;
    hue = 0;
    if (d > 0) {
        if (mx == rr) hue = 60 * std::fmod((gg - bb) / d, 6.0);
        else if (mx == gg) hue = 60 * ((bb - rr) / d + 2);
        else hue = 60 * ((rr - gg) / d + 4);
    }
    if (hue < 0) hue += 360;
    sat = mx > 0 ? d / mx * 100 : 0;
    val = mx * 100;
}

// The colour of a black body at this temperature (1000..40000 K), as 8-bit RGB.
inline void kelvin_to_rgb(int kelvin, int &r, int &g, int &b) {
    const double t = std::max(1000, std::min(40000, kelvin)) / 100.0;
    r = t <= 66 ? 255 : clamp255(329.698727446 * std::pow(t - 60, -0.1332047592));
    g = t <= 66 ? clamp255(99.4708025861 * std::log(t) - 161.1195681661) : clamp255(288.1221695283 * std::pow(t - 60, -0.0755148492));
    b = t >= 66 ? 255 : t <= 19 ? 0 : clamp255(138.5177312231 * std::log(t - 10) - 305.0447927307);
}

// A shade of white as the nearest colour, for lights that take only colours.
inline void kelvin_to_hs(int kelvin, int &hue, int &sat) {
    int r, g, b;
    kelvin_to_rgb(kelvin, r, g, b);
    double h, s, v;
    rgb_to_hs(r, g, b, h, s, v);
    hue = static_cast<int>(std::lround(h)) % 360;
    sat = static_cast<int>(std::lround(s));
}

inline double srgb_to_linear(double c) { return c > 0.04045 ? std::pow((c + 0.055) / 1.055, 2.4) : c / 12.92; }
inline double linear_to_srgb(double c) { return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1 / 2.4) - 0.055; }

// 8-bit RGB to CIE 1931 xy (wide-gamut D65 primaries, as published for Hue lights).
inline void rgb_to_xy(int r, int g, int b, double &x, double &y) {
    const double rr = srgb_to_linear(r / 255.0), gg = srgb_to_linear(g / 255.0), bb = srgb_to_linear(b / 255.0);
    const double X = rr * 0.664511 + gg * 0.154324 + bb * 0.162028;
    const double Y = rr * 0.283881 + gg * 0.668433 + bb * 0.047685;
    const double Z = rr * 0.000088 + gg * 0.072310 + bb * 0.986039;
    const double sum = X + Y + Z;
    if (sum <= 0) { x = 0.3127; y = 0.3290; return; }   // black: say the white point
    x = X / sum;
    y = Y / sum;
}

// CIE 1931 xy at full brightness to 8-bit RGB (the inverse of rgb_to_xy, scaled to fit).
inline void xy_to_rgb(double x, double y, int &r, int &g, int &b) {
    if (y <= 0) { r = g = b = 255; return; }
    const double Y = 1.0, X = Y / y * x, Z = Y / y * (1 - x - y);
    double rr = X * 1.656492 - Y * 0.354851 - Z * 0.255038;
    double gg = -X * 0.707196 + Y * 1.655397 + Z * 0.036152;
    double bb = X * 0.051713 - Y * 0.121364 + Z * 1.011530;
    rr = std::max(0.0, rr); gg = std::max(0.0, gg); bb = std::max(0.0, bb);
    const double mx = std::max(rr, std::max(gg, bb));
    if (mx > 1) { rr /= mx; gg /= mx; bb /= mx; }
    r = clamp255(linear_to_srgb(rr) * 255);
    g = clamp255(linear_to_srgb(gg) * 255);
    b = clamp255(linear_to_srgb(bb) * 255);
}

inline void hs_to_xy(double hue, double sat, double &x, double &y) {
    int r, g, b;
    hs_to_rgb(hue, sat, 100, r, g, b);
    rgb_to_xy(r, g, b, x, y);
}

inline void xy_to_hs(double x, double y, double &hue, double &sat) {
    int r, g, b;
    xy_to_rgb(x, y, r, g, b);
    double v;
    rgb_to_hs(r, g, b, hue, sat, v);
}

// The point on the Planckian locus for 1667..25000 K (Kim et al.).
inline void kelvin_to_xy(int kelvin, double &x, double &y) {
    const double t = std::max(1667, std::min(25000, kelvin)), t2 = t * t, t3 = t2 * t;
    x = t <= 4000 ? -0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / t + 0.179910
                  : -3.0258469e9 / t3 + 2.1070379e6 / t2 + 0.2226347e3 / t + 0.240390;
    const double x2 = x * x, x3 = x2 * x;
    if (t <= 2222) y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683;
    else if (t <= 4000) y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867;
    else y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
}

// The correlated colour temperature of a white near the locus (McCamy).
inline int xy_to_kelvin(double x, double y) {
    if (std::fabs(0.1858 - y) < 1e-9) return 6500;
    const double n = (x - 0.3320) / (0.1858 - y);
    return static_cast<int>(std::lround(449 * n * n * n + 3525 * n * n + 6823.3 * n + 5520.33));
}

}  // namespace color
