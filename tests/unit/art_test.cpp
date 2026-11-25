// Unit tests for the art engine (src/ui/art, no LVGL): the canvas (pictures in order, clear pixels, edges, the
// areas it returns, at most kMaxAreas of them, the 2x copy for the screen), PNG decoding (the repository's
// pictures, and broken or hostile files), every scene's frames on a fake clock (and a box breath) through a model
// of the panel against a budget per scene (a tripwire: raise it knowingly), every scene's frames against its finished
// animation (the radio's and the breathing scene's frame by frame, the others' whole loops, by hashes taken from the
// animations' own frames when their pictures were exported), dusk turning into night (the land's colours mixed), the
// registry, and which scene plays for what (pick.h: every genre of the map, a station's tags, the city's weather). Run
// from the repository (it reads assets/art/). Each failure prints a line.
#include "core/breath.h"
#include "ui/art/assets.h"
#include "ui/art/canvas.h"
#include "ui/art/pick.h"
#include "ui/art/scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <zlib.h>

using namespace art;

namespace {

int g_passed = 0, g_failed = 0;

void check(bool ok, const std::string &what) {
    if (ok) ++g_passed;
    else { ++g_failed; printf("FAIL %s\n", what.c_str()); }
}

const char *const kArt = "assets/art";

// A picture filled with `index`, and a palette whose colour i is `base` + i.
Image picture(int w, int h, uint8_t index, uint16_t base, int colours = 4) {
    Image img;
    img.w = w;
    img.h = h;
    img.px.assign(static_cast<size_t>(w) * h, index);
    for (int i = 0; i < colours; ++i) img.pal.push_back(static_cast<uint16_t>(base + i));
    return img;
}

bool inside(const std::vector<Rect> &areas, int x, int y) {
    return std::any_of(areas.begin(), areas.end(), [x, y](const Rect &r) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; });
}

// Every pixel that differs from `before` lies in one of the areas, the areas do not overlap, there are at most
// kMaxAreas, and each holds a change on its first and last row and column (it is no bigger than it must be).
void areas_exact(const std::vector<uint16_t> &before, const Canvas &c, const std::vector<Rect> &areas, const std::string &what) {
    bool covered = true;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            if (before[static_cast<size_t>(y) * kW + x] != c.at(x, y) && !inside(areas, x, y)) covered = false;
    check(covered, what + ": every changed pixel is in an area returned");
    check(areas.size() <= static_cast<size_t>(kMaxAreas), what + ": at most kMaxAreas areas");
    bool apart = true, tight = true;
    for (size_t i = 0; i < areas.size(); ++i) {
        for (size_t j = i + 1; j < areas.size(); ++j) apart = apart && overlap(areas[i], areas[j]).empty();
        const Rect &r = areas[i];
        auto changed = [&](int x, int y) { return before[static_cast<size_t>(y) * kW + x] != c.at(x, y); };
        bool top = false, bottom = false, left = false, right = false;
        for (int x = r.x; x < r.x + r.w; ++x) { top = top || changed(x, r.y); bottom = bottom || changed(x, r.y + r.h - 1); }
        for (int y = r.y; y < r.y + r.h; ++y) { left = left || changed(r.x, y); right = right || changed(r.x + r.w - 1, y); }
        tight = tight && top && bottom && left && right;
    }
    check(apart, what + ": the areas do not overlap");
    check(tight, what + ": each area is no bigger than its changes");
}

std::vector<uint16_t> snapshot(const Canvas &c) { return std::vector<uint16_t>(c.pixels(), c.pixels() + kW * kH); }

std::string util_upper(const char *text) {
    std::string out = text;
    for (char &ch : out) ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
    return out;
}

// ------------------------------------------------------------------ the canvas
// Pictures in the order added, each through its own palette; clear pixels show what is under them.
void test_canvas_layers() {
    Canvas c;
    check(c.compose().empty(), "a new canvas has nothing to compose");
    const Image sky = picture(kW, kH, 1, 0x100);
    Image sprite = picture(4, 4, 2, 0x200);
    sprite.px[0] = sprite.px[3] = 0;                       // clear corners: the sky shows through
    const Image cover = picture(2, 2, 1, 0x300);
    c.add(sky);
    std::vector<uint16_t> before = snapshot(c);
    std::vector<Rect> areas = c.compose();
    check(areas.size() == 1 && areas[0] == Rect{0, 0, kW, kH}, "a layer is composed whole");
    check(c.at(0, 0) == 0x101 && c.at(kW - 1, kH - 1) == 0x101, "a layer's colour comes from its own palette");
    c.add(sprite, {0, 0, 4, 4}, 10, 10);
    before = snapshot(c);
    areas = c.compose();
    areas_exact(before, c, areas, "a sprite added");
    check(c.at(10, 10) == 0x101 && c.at(13, 10) == 0x101 && c.at(11, 10) == 0x202 && c.at(10, 13) == 0x202,
          "a sprite's clear pixels show what is under it");
    c.add(cover, {0, 0, 2, 2}, 12, 12);
    c.compose();
    check(c.at(12, 12) == 0x301 && c.at(11, 11) == 0x202, "a picture added later lies over one added before");
    check(c.compose().empty(), "nothing changed: nothing composed");
    c.clear();
    before = snapshot(c);
    areas = c.compose();
    check(c.at(0, 0) == 0 && c.at(kW - 1, kH - 1) == 0, "cleared: black");
    areas_exact(before, c, areas, "cleared");
}

// Sprites moved past the edges, shown and hidden, and switched to another cell of their sheet: the areas returned
// cover exactly what changed.
void test_canvas_changes() {
    Canvas c;
    const Image sky = picture(kW, kH, 1, 0x100);
    Image sprite = picture(4, 4, 2, 0x200);
    sprite.px[0] = sprite.px[3] = 0;
    c.add(sky);
    const int s = c.add(sprite, {0, 0, 4, 4}, 10, 10);
    c.compose();
    std::vector<uint16_t> before = snapshot(c);
    c.move(s, -2, -2);
    areas_exact(before, c, c.compose(), "a sprite moved past the top left edge");
    check(c.at(0, 0) == 0x202 && c.at(1, 0) == 0x202 && c.at(2, 0) == 0x101 && c.at(10, 11) == 0x101, "a sprite clipped at the top left edge");
    before = snapshot(c);
    c.move(s, kW - 2, kH - 1);
    areas_exact(before, c, c.compose(), "a sprite moved past the bottom right edge");
    check(c.at(kW - 2, kH - 1) == 0x101 && c.at(kW - 1, kH - 1) == 0x202, "a sprite clipped at the bottom right edge");
    before = snapshot(c);
    c.show(s, false);
    areas_exact(before, c, c.compose(), "a sprite hidden");
    check(c.at(kW - 1, kH - 1) == 0x101, "a hidden sprite leaves what is under it");
    c.show(s, true);
    c.show(s, false);
    check(c.compose().empty(), "shown and hidden again before composing: no change");

    // A sheet of two cells side by side, one solid, one clear.
    Image sheet = picture(4, 2, 1, 0x400);
    for (int y = 0; y < 2; ++y) sheet.px[static_cast<size_t>(y) * 4 + 2] = sheet.px[static_cast<size_t>(y) * 4 + 3] = 0;
    const int f = c.add(sheet, {0, 0, 2, 2}, 50, 50);
    c.compose();
    check(c.at(50, 50) == 0x401, "a sheet's first cell");
    before = snapshot(c);
    c.cell(f, {2, 0, 2, 2});
    areas_exact(before, c, c.compose(), "another cell of a sheet");
    check(c.at(50, 50) == 0x101, "a sheet's clear cell");
    c.cell(f, {3, 1, 5, 5});                                // past the sheet's edge: clipped to it
    c.compose();
    check(c.at(50, 50) == 0x101, "a cell past the sheet's edge is clipped");
}

void test_many_areas() {
    Canvas c;
    const Image sky = picture(kW, kH, 1, 0x100);
    const Image dot = picture(1, 1, 1, 0x500);
    c.add(sky);
    std::vector<int> dots;
    for (int i = 0; i < 40; ++i) dots.push_back(c.add(dot, {0, 0, 1, 1}, (i * 37) % kW, (i * 23) % kH, false));
    c.compose();
    const std::vector<uint16_t> before = snapshot(c);
    for (int id : dots) c.show(id, true);
    const std::vector<Rect> areas = c.compose();
    areas_exact(before, c, areas, "40 dots shown at once");
}

void test_scale2x() {
    Canvas c;
    Image sky = picture(kW, kH, 1, 0x100);
    sky.px[static_cast<size_t>(9) * kW + 3] = 2;            // art row 9, the stage's first
    sky.px[static_cast<size_t>(75) * kW + 159] = 3;         // art row 75, its last
    c.add(sky);
    c.compose();
    constexpr int kFirst = 9, kRows = 67;                   // the chat's stage: screen rows 18 to 151
    std::vector<uint16_t> stage(static_cast<size_t>(2 * kW) * 2 * kRows, 0);
    const Rect written = scale2x(c, {0, 0, kW, kH}, stage.data(), kFirst, kRows);
    check(written == Rect{0, 0, 2 * kW, 2 * kRows}, "the whole canvas fills the stage's rows");
    auto at = [&stage](int x, int y) { return stage[static_cast<size_t>(y) * 2 * kW + static_cast<size_t>(x)]; };
    check(at(6, 0) == 0x102 && at(7, 0) == 0x102 && at(6, 1) == 0x102 && at(7, 1) == 0x102 && at(8, 0) == 0x101,
          "an art pixel is 2x2 on the screen");
    check(at(318, 132) == 0x103 && at(319, 133) == 0x103, "the stage's last art row");
    check(scale2x(c, {0, 0, kW, 9}, stage.data(), kFirst, kRows).empty(), "rows above the stage are not copied");
    check(scale2x(c, {10, 70, 5, 10}, stage.data(), kFirst, kRows) == (Rect{20, 122, 10, 12}), "an area cut at the stage's foot");
}

// ------------------------------------------------------------------ decoding
// A PNG made by hand (zlib for its data and checksums), for files no exporter would write.
std::string chunk(const char *kind, const std::string &data) {
    auto be32 = [](uint32_t v) { return std::string{char(v >> 24), char(v >> 16), char(v >> 8), char(v)}; };
    std::string body = std::string(kind, 4) + data;
    return be32(static_cast<uint32_t>(data.size())) + body +
           be32(static_cast<uint32_t>(crc32(0, reinterpret_cast<const Bytef *>(body.data()), static_cast<uInt>(body.size()))));
}

std::string handmade_png(int w, int h, int depth, int type, int palette, const std::string &rows) {
    auto be32 = [](uint32_t v) { return std::string{char(v >> 24), char(v >> 16), char(v >> 8), char(v)}; };
    std::string ihdr = be32(static_cast<uint32_t>(w)) + be32(static_cast<uint32_t>(h)) + char(depth) + char(type) + '\0' + '\0' + '\0';
    uLongf size = compressBound(static_cast<uLong>(rows.size()));
    std::string packed(size, '\0');
    compress(reinterpret_cast<Bytef *>(&packed[0]), &size, reinterpret_cast<const Bytef *>(rows.data()), static_cast<uLong>(rows.size()));
    packed.resize(size);
    std::string png = "\x89PNG\r\n\x1a\n" + chunk("IHDR", ihdr);
    if (palette) png += chunk("PLTE", std::string(static_cast<size_t>(palette) * 3, '\x40'));
    return png + chunk("IDAT", packed) + chunk("IEND", "");
}

void test_decode() {
    const Source files = folder(kArt);
    Image img;
    std::string err;
    for (const char *name : {"sky-dusk", "sky-night", "land-dusk", "land-night", "lights-night", "twinkle-night"}) {
        const bool ok = decode(files(std::string("radio/") + name + ".png"), img, err);
        check(ok && img.w == kW && img.h == kH && img.pal.size() <= 25, std::string("radio/") + name + ".png decodes as a 160x85 layer" + (ok ? "" : ": " + err));
    }
    check(decode(files("radio/rings-dusk.png"), img, err) && img.w == 8 * 49 && img.h == 5 * 49, "the rings' sheet: 8 ages by 5 turns");
    check(decode(files("radio/light.png"), img, err) && img.w == 2 && img.h == 1 && img.pal.size() == 3 &&
              img.pal[1] != img.pal[2], "the mast's light: two colours");
    Image dusk, night;
    decode(files("radio/land-dusk.png"), dusk, err);
    decode(files("radio/land-night.png"), night, err);
    check(dusk.px == night.px && dusk.pal != night.pal, "the land: the same pixels in two lightings, two palettes");

    const std::string sky = files("radio/sky-dusk.png");
    check(!decode("", img, err) && err == "not a PNG", "nothing is not a PNG");
    check(!decode("GIF89a and then some bytes", img, err) && err == "not a PNG", "another kind of file is refused");
    check(!decode(sky.substr(0, sky.size() / 2), img, err), "a PNG cut short is refused: " + err);
    const std::string row = std::string(1, '\0') + "\x01\x02";
    check(decode(handmade_png(2, 1, 8, 3, 3, row), img, err), "a hand-made indexed PNG decodes" + (err.empty() ? "" : ": " + err));
    check(!decode(handmade_png(2, 1, 8, 3, 2, row), img, err) && err == "a pixel outside the palette", "a pixel outside the palette is refused");
    check(!decode(handmade_png(1, 1, 8, 2, 0, std::string("\0\x10\x20\x30", 4)), img, err), "a colour (RGB) PNG is refused");
    check(!decode(handmade_png(8, 1, 1, 3, 2, std::string("\0\x55", 2)), img, err), "a 1-bit indexed PNG is refused");
    check(!decode(handmade_png(1, 1, 8, 3, 0, std::string("\0\0", 2)), img, err), "an indexed PNG without a palette is refused");
    check(!decode(handmade_png(70000, 1, 8, 3, 1, std::string(70001, '\0')), img, err), "an impossible size is refused");

    check(decode(files("breathe/disc-dusk.png"), img, err) && img.w == 8 * 21 && img.h == 38, "the disc's sheet: 8 sizes across");
    check(decode(files("breathe/waves-night.png"), img, err) && img.w == kW && img.h == 16 * 15, "the waves' sheet: 16 frames down");
    decode(files("breathe/disc-dusk.png"), dusk, err);
    decode(files("breathe/disc-night.png"), night, err);
    check(dusk.px == night.px && dusk.pal.size() == 5 && dusk.pal[3] == dusk.pal[1] && night.pal[3] != night.pal[1],
          "the disc: the same pixels at dusk and at night, the moon's spots in the sun's own colour at dusk");

    Assets assets(files);
    const Image *a = assets.image("radio/sky-dusk.png");
    check(a && assets.image("radio/sky-dusk.png") == a, "a picture is decoded once and kept");
    check(!assets.image("radio/none.png"), "a missing picture is none");
}

// ------------------------------------------------------------------ the scenes
// The panel (AGENTS.md section 2; the kernel's deferred I/O for the framebuffer): the first write after an update opens a
// 50 ms window, everything written in it goes in one update, and an update is one full-width band from the first to
// the last 4 KiB page written. Only the rows the scene is shown in reach the screen: the chat's stage (art rows 9 to
// 75), or all of them (the breathing screen).
class Panel {
public:
    explicit Panel(bool full_screen) : shown_first_(full_screen ? 0 : 9), shown_rows_(full_screen ? kH : 67) {}
    void write(unsigned long t, const Rect &art) {
        const Rect r = overlap(art, {0, shown_first_, kW, shown_rows_});
        if (r.empty()) return;
        if (open_ && t - since_ >= 50) flush();
        if (!open_) { open_ = true; since_ = t; first_ = 2 * r.y; last_ = 2 * (r.y + r.h) - 1; }
        first_ = std::min(first_, 2 * r.y);
        last_ = std::max(last_, 2 * (r.y + r.h) - 1);
    }
    void flush() {
        if (!open_) return;
        open_ = false;
        const long from = first_ * kRowBytes / kPage * kPage, to = std::min(kScreenBytes, ((last_ + 1) * kRowBytes - 1) / kPage * kPage + kPage);
        bytes_ += to - from;
    }
    long bytes() const { return bytes_; }

private:
    static constexpr long kRowBytes = 640, kPage = 4096, kScreenBytes = 640 * 170;
    const int shown_first_, shown_rows_;   // the art rows that reach the screen
    bool open_ = false;
    unsigned long since_ = 0;
    int first_ = 0, last_ = 0;
    long bytes_ = 0;
};

// What each scene may send the panel, in kB a second over 80 s (every scene's loop; the breathing scene with a box
// breath) in either lighting: a tripwire, as the first request's 32,000 bytes is. The finished animations measured
// (over the whole screen's rows, in 4 KiB pages): the radio 217; the breathing scene 103 at dusk and 110 at
// night over its 16 s loop of one box breath; the city 81 at dusk and 28 at night, in the rain 358; pop 342, rock 117,
// jazz 212, electronic 125, classical 60.
const std::map<std::string, double> kBudgetKBps = {{"radio", 250}, {"breathe", 130}, {"city", 100}, {"city_rain", 380},
                                                   {"pop", 360}, {"rock", 130}, {"jazz", 230}, {"electronic", 140},
                                                   {"classical", 75}};
// The scenes shown over the whole screen (the breathing screen's); the others on the chat's stage.
const std::set<std::string> kFullScreen = {"breathe"};

// A scene started on a fresh canvas in a lighting; nullptr (with a failure) when it cannot start.
std::unique_ptr<Scene> started(const std::string &name, Canvas &c, Assets &assets, const Moment &m) {
    std::unique_ptr<Scene> scene = make(name);
    const bool ok = scene && scene->start(c, assets, m);
    char what[96];
    snprintf(what, sizeof(what), "%s starts %.2f of the way from dusk to night", name.c_str(), m.blend);
    check(ok, what);
    return ok ? std::move(scene) : nullptr;
}

std::unique_ptr<Scene> started(const std::string &name, Canvas &c, Assets &assets, bool night) {
    return started(name, c, assets, Moment::at(0, night ? 1.f : 0.f));
}

void test_budgets() {
    Assets assets(folder(kArt));
    for (const std::string &name : names()) {
        const auto budget = kBudgetKBps.find(name);
        check(budget != kBudgetKBps.end(), name + " has a budget in kBudgetKBps");
        for (bool night : {false, true}) {
            Canvas c;
            std::unique_ptr<Scene> scene = started(name, c, assets, night);
            if (!scene || budget == kBudgetKBps.end()) continue;
            Panel panel(kFullScreen.count(name) > 0);
            constexpr unsigned long kRunMs = 80000;
            for (unsigned long t = 0; t < kRunMs; t += scene->period_ms()) {
                const float level = breath::at(breath::pattern("box"), static_cast<long>(t), kRunMs).level;
                scene->frame(c, Moment::at(t, night, level));
                for (const Rect &r : c.compose()) panel.write(t, r);
            }
            panel.flush();
            const double kbps = panel.bytes() / (kRunMs / 1000.0) / 1000.0;
            char what[160];
            snprintf(what, sizeof(what), "%s %s sends %.0f kB/s to the panel (budget %.0f)", name.c_str(), night ? "at night" : "at dusk",
                     kbps, budget->second);
            check(kbps <= budget->second, what);
            printf("%s\n", what);
        }
    }
}

// FNV-1a over a frame's RGB565 values, little-endian, row by row, as the export took it of the animation's frames;
// from `h`, the hash of the frames before it in a loop.
constexpr uint64_t kFnvStart = 0xcbf29ce484222325ull;
uint64_t frame_hash(const Canvas &c, uint64_t h = kFnvStart) {
    for (int i = 0; i < kW * kH; ++i)
        for (const uint8_t byte : {static_cast<uint8_t>(c.pixels()[i] & 0xff), static_cast<uint8_t>(c.pixels()[i] >> 8)})
            h = (h ^ byte) * 0x100000001b3ull;
    return h;
}

// The radio's 16 frames, each against the finished animation's (its dusk and night loops of 160x85 frames, in
// RGB565), and the loop: frame 16 is frame 0 again. The areas returned cover every change.
void test_radio_frames() {
    const uint64_t dusk[16] = {0xe3483ac753380bc9ull, 0x18f29cfa39bc53b7ull, 0x6628618899d3ec69ull, 0x4e31a6c1f003f735ull,
                               0xe4e9ace2f31e8505ull, 0x5e11977d7bc25ec7ull, 0x452043b4fa0b4e10ull, 0x90d8f7011c36d11aull,
                               0xb6cd938bb0b1303cull, 0xcc7b6724ce4f69ecull, 0xf07085b921dbf5dbull, 0xf3b797796b9a9249ull,
                               0x1f677d4ba82d25f0ull, 0x399fc924dabcafffull, 0x1ee5d6ec9cf9b9fcull, 0x613aad525cffa692ull};
    const uint64_t night[16] = {0x56c4d7e6db34db34ull, 0x2c7801c4003f69e9ull, 0x1eb9f0ba040dd59full, 0x597b4f471c4afe4aull,
                                0xf9c711518fba07b2ull, 0x88c4bb2171d40e67ull, 0xa3b9892afbdfa287ull, 0x20322c8ce9386715ull,
                                0xdd90f0e697f4b4f7ull, 0x6cb0ee0d56734bcbull, 0x2f143c9f38217e7dull, 0x6cd5bf4e18664d97ull,
                                0xbd6856886f17260full, 0x01b353e6a82c6683ull, 0x99f9ac432db88938ull, 0xc45930646ae383fbull};
    Assets assets(folder(kArt));
    for (bool is_night : {false, true}) {
        const uint64_t *want = is_night ? night : dusk;
        const std::string lighting = is_night ? "night" : "dusk";
        Canvas c;
        std::unique_ptr<Scene> scene = started("radio", c, assets, is_night);
        if (!scene) continue;
        check(scene->period_ms() == 250, "the radio steps 4 times a second");
        int same = 0;
        for (int f = 0; f <= 16; ++f) {
            const std::vector<uint16_t> before = snapshot(c);
            scene->frame(c, Moment::at(static_cast<unsigned long>(f) * 250 + 100, is_night));   // any time within the frame
            const std::vector<Rect> areas = c.compose();
            if (f > 0) areas_exact(before, c, areas, "radio " + lighting + " frame " + std::to_string(f));
            if (frame_hash(c) == want[f % 16]) ++same;
            else printf("     radio %s frame %d: hash 0x%016llx\n", lighting.c_str(), f, static_cast<unsigned long long>(frame_hash(c)));
        }
        check(same == 17, "radio at " + lighting + ": every frame of the loop is the animation's, pixel for pixel (" +
                              std::to_string(same) + " of 17)");
    }
}

// Every scene's whole loop at dusk and at night against its finished animation: one hash over all its frames in order
// (taken from the animation's own frames), the areas returned covering every change, and the loop closing (the frame
// after the last is the first again).
void test_scene_loops() {
    const struct {
        const char *scene;
        bool night;
        int frames;
        uint64_t hash;
    } kLoops[] = {
        {"city", false, 640, 0x1a831313ff2a7b84ull},      {"city", true, 640, 0xaafac9f9371301bbull},
        {"city_rain", false, 640, 0x683f768d009acd94ull}, {"city_rain", true, 640, 0x3edfc0000667cf52ull},
        {"pop", false, 64, 0xd49cea480f034895ull},        {"pop", true, 64, 0x615a79c58fca6c65ull},
        {"rock", false, 64, 0x732a9e3557e52f2dull},       {"rock", true, 64, 0xf2b668aec89d0a71ull},
        {"jazz", false, 64, 0x860a28166f7dc381ull},       {"jazz", true, 64, 0x25596f9938be57c9ull},
        {"electronic", false, 64, 0x12745575edebee55ull}, {"electronic", true, 64, 0xda41dad9d7b6fab9ull},
        {"classical", false, 64, 0xad20d9a44a288a45ull},  {"classical", true, 64, 0xeaca8a70467e7e35ull},
    };
    Assets assets(folder(kArt));
    for (const auto &loop : kLoops) {
        const std::string what = std::string(loop.scene) + (loop.night ? " at night" : " at dusk");
        Canvas c;
        std::unique_ptr<Scene> scene = started(loop.scene, c, assets, loop.night);
        if (!scene) continue;
        check(scene->period_ms() == 125, what + " steps 8 times a second");
        uint64_t h = kFnvStart, first = 0;
        bool covered = true;
        for (int f = 0; f <= loop.frames; ++f) {
            const std::vector<uint16_t> before = snapshot(c);
            scene->frame(c, Moment::at(static_cast<unsigned long>(f) * 125 + 60, loop.night));   // any time within the frame
            const std::vector<Rect> areas = c.compose();
            for (int i = 0; i < kW * kH && covered; ++i)
                if (before[static_cast<size_t>(i)] != c.pixels()[i] && f > 0 && !inside(areas, i % kW, i / kW)) covered = false;
            if (f == 0) first = frame_hash(c);
            if (f < loop.frames) h = frame_hash(c, h);
            else check(frame_hash(c) == first, what + ": the loop closes");
        }
        check(covered, what + ": the areas returned cover every change");
        check(h == loop.hash, what + ": every frame of the loop is the animation's, pixel for pixel");
        if (h != loop.hash) printf("     %s: hash 0x%016llx\n", what.c_str(), static_cast<unsigned long long>(h));
    }
}

// Dusk turning into night: the land's colours mixed from the two lightings' (each part of RGB565 on its own), the
// same pixels; dusk's and the night's own pictures at the ends; no mixing of pictures that differ; and every scene
// starts anywhere on the way.
void test_blend() {
    Assets assets(folder(kArt));
    const Image *dusk = assets.lit("radio/land", 0), *night = assets.lit("radio/land", 1), *half = assets.lit("radio/land", 0.5f);
    check(dusk && dusk == assets.image("radio/land-dusk.png") && night == assets.image("radio/land-night.png"),
          "dusk's and the night's own land at the ends");
    bool between = half && half->px == dusk->px, moved = false;
    for (size_t i = 0; half && i < half->pal.size(); ++i)
        for (const auto &part : {std::make_pair(11, 0x1F), std::make_pair(5, 0x3F), std::make_pair(0, 0x1F)}) {
            const int a = dusk->pal[i] >> part.first & part.second, b = night->pal[i] >> part.first & part.second,
                      m = half->pal[i] >> part.first & part.second;
            between = between && std::abs(2 * m - (a + b)) <= 1;
            moved = moved || m != a;
        }
    check(between && moved, "half way, the land's colours are half way from dusk's to the night's, its pixels dusk's");
    check(assets.lit("radio/land", 0.5f) == half, "a mix is made once for each light");
    check(!assets.lit("radio/sky", 0.5f), "pictures that differ are not mixed");
    check(!assets.lit("radio/none", 0.5f), "nor missing ones");
    check(Moment::at(0, 0.49f).night == false && Moment::at(0, 0.5f).night, "the night's pictures from half way");
    for (const std::string &name : names())
        for (const float blend : {0.25f, 0.75f}) {
            Canvas c;
            if (std::unique_ptr<Scene> scene = started(name, c, assets, Moment::at(0, blend))) {
                scene->frame(c, Moment::at(1000, blend));
                c.compose();
            }
        }
}

// ------------------------------------------------------------------ which scene plays (pick.h)
// Every ID3v1 genre in its family (the genre map: all 148), the free text's rules in their order, and the
// default.
void test_genres() {
    const std::map<std::string, std::vector<const char *>> kId3v1 = {
        {"city", {"New Age", "Ambient", "Trip-Hop", "Space", "Meditative", "Dream", "Lo-Fi", "Easy Listening", "Acoustic"}},
        {"electronic", {"Dance", "Techno", "Industrial", "Euro-Techno", "Trance", "Acid", "House", "Game", "Noise", "Bass",
                        "Darkwave", "Techno-Industrial", "Electronic", "Eurodance", "Jungle", "New Wave", "Rave", "Club",
                        "Euro-House", "Goa", "Drum & Bass", "Club-House", "Hardcore", "Terror", "Synthpop"}},
        {"rock", {"Classic Rock", "Grunge", "Metal", "Alternative", "Death Metal", "Alternative Rock", "Punk",
                  "Instrumental Rock", "Gothic", "Southern Rock", "Psychedelic", "Acid Punk", "Rock & Roll", "Hard Rock",
                  "Folk-Rock", "Gothic Rock", "Progressive Rock", "Psychedelic Rock", "Symphonic Rock", "Slow Rock", "Rock",
                  "Primus", "Power Ballad", "Punk Rock", "Drum Solo", "Indie", "BritPop", "Afro-Punk", "Polsk Punk",
                  "Heavy Metal", "Black Metal", "Crossover", "Christian Rock", "Thrash Metal"}},
        {"jazz", {"Blues", "Funk", "Jazz", "R&B", "Jazz+Funk", "Fusion", "Gospel", "Soul", "Cabaret", "Acid Jazz", "Swing",
                  "Fast Fusion", "Bebop", "Big Band", "Chanson", "Slow Jam", "Rhythmic Soul"}},
        {"classical", {"Classical", "Soundtrack", "Avantgarde", "Chorus", "Opera", "Chamber Music", "Sonata", "Symphony"}},
        {"pop", {"Country", "Disco", "Hip-Hop", "Oldies", "Other", "Pop", "Rap", "Reggae", "Ska", "Pranks", "Vocal",
                 "Instrumental", "Sound Clip", "Instrumental Pop", "Ethnic", "Pop-Folk", "Comedy", "Cult", "Gangsta", "Top 40",
                 "Christian Rap", "Pop/Funk", "Native American", "Showtunes", "Trailer", "Tribal", "Polka", "Retro", "Musical",
                 "Folk", "National Folk", "Latin", "Revival", "Celtic", "Bluegrass", "Humour", "Speech", "Booty Bass",
                 "Porn Groove", "Satire", "Tango", "Samba", "Folklore", "Ballad", "Freestyle", "Duet", "A Cappella",
                 "Dance Hall", "Beat", "Christian Gangsta Rap", "Contemporary Christian", "Merengue", "Salsa", "Anime", "JPop"}},
    };
    size_t count = 0;
    for (const auto &family : kId3v1)
        for (const char *genre : family.second) {
            ++count;
            const std::string got = scene_for_genre(genre);
            check(got == family.first, std::string("ID3v1's \"") + genre + "\" plays " + family.first + " (got " + got + ")");
            check(scene_for_genre(util_upper(genre)) == family.first, std::string("\"") + genre + "\" in capitals too");
        }
    check(count == 148, "all 148 of ID3v1's genres are mapped (" + std::to_string(count) + ")");
    const std::vector<std::pair<const char *, const char *>> kFree = {
        // 1. chill, lo-fi, ambient: the city
        {"Chillout", "city"}, {"chillhop", "city"}, {"Chillwave", "city"}, {"lo-fi hip hop", "city"}, {"Lo_Fi", "city"},
        {"lo.fi beats", "city"}, {"LoFi", "city"}, {"Dark Ambient", "city"}, {"Downtempo", "city"}, {"Sleep sounds", "city"},
        {"Study beats", "city"},
        // 2. jazz and blues, before rock: "jazz rock" is jazz
        {"Jazz Rock", "jazz"}, {"Delta Blues", "jazz"}, {"Neo-Soul", "jazz"}, {"Funky", "jazz"}, {"Western Swing", "jazz"},
        {"Hard Bop Bebop", "jazz"}, {"Contemporary R&B", "jazz"}, {"RnB", "jazz"}, {"Southern Gospel", "jazz"},
        {"Lounge", "jazz"}, {"Motown", "jazz"},
        // 3. rock, before electronic: "pop rock", "symphonic metal" and "indie electronic" are rock
        {"Pop Rock", "rock"}, {"Symphonic Metal", "rock"}, {"Krautrock", "rock"}, {"Pop-Punk", "rock"}, {"post-grunge", "rock"},
        {"Emo", "rock"}, {"Shoegaze", "rock"}, {"Indie Electronic", "rock"},
        // 4. electronic
        {"Electronica", "electronic"}, {"Electropop", "electronic"}, {"Minimal Techno", "electronic"}, {"Deep House", "electronic"},
        {"Psytrance", "electronic"}, {"EDM", "electronic"}, {"Dubstep", "electronic"}, {"Drum and Bass", "electronic"},
        {"drum & bass", "electronic"}, {"DnB", "electronic"}, {"Synthwave", "electronic"}, {"Rave", "electronic"},
        {"IDM", "electronic"}, {"Breakbeat", "electronic"}, {"UK Garage", "electronic"}, {"Chiptune", "electronic"},
        {"Retrowave", "electronic"}, {"Vaporwave", "electronic"}, {"Outrun", "electronic"},
        // 5. classical
        {"Modern Classical", "classical"}, {"Orchestral", "classical"}, {"Symphonic", "classical"}, {"Opera buffa", "classical"},
        {"Baroque", "classical"}, {"Choral", "classical"}, {"Piano Concerto", "classical"}, {"Piano Sonata", "classical"},
        {"Chamber Pop", "classical"}, {"Piano Solo", "classical"}, {"Film Score", "classical"},
        // 6. anything else, or none: pop
        {"", "pop"}, {"K-Pop", "pop"}, {"Hip Hop", "pop"}, {"Klezmer", "pop"}, {"  ", "pop"}};
    for (const auto &c : kFree) {
        const std::string got = scene_for_genre(c.first);
        check(got == c.second, std::string("\"") + c.first + "\" plays " + c.second + " (got " + got + ")");
    }
}

// A calm station (focus, lofi, study, chill, ambient, downtempo, in any case) shows the city; any other the radio.
void test_stations() {
    for (const char *tag : {"focus", "lofi", "study", "chill", "ambient", "downtempo", "Focus", "LOFI"})
        check(scene_for_station({"talk", tag}) == "city", std::string("a station tagged ") + tag + " shows the city");
    for (const std::vector<std::string> &tags : std::vector<std::vector<std::string>>{{}, {"talk"}, {"classical", "news"}, {"lofi radio"}})
        check(scene_for_station(tags) == "radio", "a station tagged otherwise shows the radio (" + std::to_string(tags.size()) + " tags)");
}

// The city's weather: chosen as the city comes on, never changed while it plays, kept from one time to the next until
// it has played 30 minutes with it, then chosen again; rain about one time in three; other scenes pass as they are.
void test_weather() {
    const time_t noon = 1795003200;
    int rainy = 0;
    for (int minute = 0; minute < 3000; ++minute) {
        Weather w;
        rainy += w.follow("city", 0, noon + 60 * minute) == "city_rain";
    }
    check(rainy > 800 && rainy < 1200, "rain about one time in three (" + std::to_string(rainy) + " of 3000)");
    time_t rain_at = noon, clear_at = noon;
    for (int minute = 0; minute < 100; ++minute) {
        Weather w;
        (w.follow("city", 0, noon + 60 * minute) == "city_rain" ? rain_at : clear_at) = noon + 60 * minute;
    }
    Weather w;
    check(w.follow("pop", 0, rain_at) == "pop" && w.follow("", 10, rain_at) == "", "other scenes pass as they are");
    check(w.follow("city", 1000, rain_at) == "city_rain", "the city comes on in the rain");
    bool kept = true;
    unsigned long t = 1000;
    for (; t < 1000 + 2 * Weather::kKeepMs; t += 60000) kept = kept && w.follow("city", t, clear_at) == "city_rain";
    check(kept, "the weather never changes while the city plays, an hour on");
    w.follow("rock", t, clear_at);
    check(w.follow("city", t + 1000, clear_at) == "city", "after 30 minutes of it, the next time chooses again");
    // 29 minutes of the city over three times, then a 30th: kept until then, whatever the clock says.
    const unsigned long minute = 60000;
    Weather v;
    check(v.follow("city", 0, rain_at) == "city_rain", "rain chosen");
    v.follow("", 10 * minute, rain_at);
    check(v.follow("city", 11 * minute, clear_at) == "city_rain", "kept from one time to the next within minutes");
    v.follow("jazz", 30 * minute, clear_at);
    check(v.follow("city", 31 * minute, clear_at) == "city_rain", "kept after 29 minutes of it in all");
    v.follow("", 32 * minute, clear_at);
    check(v.follow("city", 33 * minute, clear_at) == "city", "chosen again after 30 minutes of it in all");
}


// The breathing scene's 64 frames (16 s: one box breath, the waves' 4 s loop four times, each star out once) against
// the finished animation's in both lightings, each at the size the animation took from the breath on that frame (a
// level of k/7 for size k), and the loop: frame 64 is frame 0 again. The areas returned cover every change. A level
// past full is full, and one that is not a number is empty.
void test_breathe_frames() {
    const char *const sizes = "0001122334556677777777777777777777766554432211000000000000000000";
    const uint64_t dusk[64] = {
        0x4ffd582135783ebbull, 0x14c496e05e0b6b59ull, 0xfc2844b7dd796359ull, 0x4f0c951f2570d781ull,
        0x1d9a5c25bc2e872bull, 0x1a82bbaeebd77fcdull, 0xc2f02e622c2a72e5ull, 0x140cfa1d98c6c6e2ull,
        0x220b8079f42adadaull, 0xcb49aafd729e3df3ull, 0x7d290b97f64dcf09ull, 0x63fcaea7abd3b419ull,
        0x7a5bc18aec51f8caull, 0x87490ff2db1634b7ull, 0x0030e70cadc022a3ull, 0xee23c2837e3c8af1ull,
        0x58d4983768df6577ull, 0x255d54ab3fad2fddull, 0x5d897c5d8492589dull, 0x8b62b90d0f0525f7ull,
        0xbfd0891cc14c5365ull, 0x3829247b02f91aa9ull, 0x8fd6b98607960e21ull, 0x1002ee970f11baf8ull,
        0x2635ac6eea924db8ull, 0x6838c8c1fa127a24ull, 0x4356e601a9faf888ull, 0x6bb37849af71fcf8ull,
        0x5af7d7b082884c00ull, 0x8fd6b98607960e21ull, 0x0030e70cadc022a3ull, 0xee23c2837e3c8af1ull,
        0x58d4983768df6577ull, 0x255d54ab3fad2fddull, 0x5d897c5d8492589dull, 0x8e13f7c373d722a1ull,
        0xb6e6b7925dc9624bull, 0x77d6c56237ca8110ull, 0x120492258beb4908ull, 0x08cee3862715f73full,
        0x60cce1e2f907548full, 0x93d3a38f8157fd62ull, 0x83f2efef412c950cull, 0x44caa5f3e3f89b8cull,
        0x765502f559ba83aaull, 0x1742d95f424c2c97ull, 0xf1ff53665bb2523full, 0x4f0dd94f8dc060edull,
        0x4ffd582135783ebbull, 0x14c496e05e0b6b59ull, 0xfc2844b7dd796359ull, 0xf52bb580093dc7bbull,
        0xc1808bdd505f3be1ull, 0xff82609e2628e3e5ull, 0x9481b9d6daa25a3dull, 0xf3e73156d158fc94ull,
        0xc22e0e4def4ea3d4ull, 0xe0d3f8ca86f66d90ull, 0x84cb3965b62eb594ull, 0x1cfbd7704516e9d4ull,
        0x9a6c24db090859ccull, 0x9481b9d6daa25a3dull, 0xf1ff53665bb2523full, 0x4f0dd94f8dc060edull};
    const uint64_t night[64] = {
        0x7cda8e0922d565c0ull, 0xe88daaa71a93aff9ull, 0xa291d05a5b261b7dull, 0x85a2e62798661e45ull,
        0x4fd0bc6b384b3aa8ull, 0xf772153d629f0047ull, 0x6c656dd0a8b9373eull, 0x472c4239f0788e8full,
        0x96b8b361e86b6533ull, 0x2a81ecea465940cbull, 0xb621d398711dc384ull, 0xd6f376532d6b09e6ull,
        0x640a613036359a92ull, 0x23590f8924f0a124ull, 0x6ece635503ebf405ull, 0x6ece635503ebf405ull,
        0xe9c9c9ee26826ef0ull, 0x7ce3496913502c09ull, 0x3e32624bbe866098ull, 0xa9d3c3ce49bd0369ull,
        0x78807e0c3bbc6789ull, 0xe9a4ffb6487c2b44ull, 0xc97962e0340a1d71ull, 0x6a09aed68735914aull,
        0xcad0570bf1551bdeull, 0xc9bbe950292ed325ull, 0x65f714588123c5c0ull, 0x521a899227304412ull,
        0x4d721a71119036efull, 0xc97962e0340a1d71ull, 0x6ece635503ebf405ull, 0xcb3bfec7eb388783ull,
        0x24ad8e16320fd90aull, 0x7ce3496913502c09ull, 0xa5b8176c200bec4dull, 0xee180823c0bcdb51ull,
        0xa0b59bb1d1f2b514ull, 0x8bee1f9be89c5590ull, 0x4274235fb46929fdull, 0xde89589dac405658ull,
        0xef090f3dd7ddd9dcull, 0x31e0c5e98773c7f4ull, 0xa6786d76663d5233ull, 0x5bdf5ca062233595ull,
        0xdd18b513c0888e86ull, 0x3cbe0e9b6b22ca20ull, 0x71061003898d10f5ull, 0x71061003898d10f5ull,
        0x7cda8e0922d565c0ull, 0xe88daaa71a93aff9ull, 0xa291d05a5b261b7dull, 0x48d2e067f3295cd4ull,
        0xf26fdc5d204fb579ull, 0x200c92db40e6e5d4ull, 0x27911613cd85a4a1ull, 0xee96a85e55562c1aull,
        0x035fe2a087746f6eull, 0x9a2cd5555fac1e95ull, 0x53da369b68ea80b0ull, 0x86d0e1bec3a90922ull,
        0x547727090ee1e8bfull, 0x27911613cd85a4a1ull, 0x71061003898d10f5ull, 0x71061003898d10f5ull};
    Assets assets(folder(kArt));
    for (bool is_night : {false, true}) {
        const uint64_t *want = is_night ? night : dusk;
        const std::string lighting = is_night ? "night" : "dusk";
        Canvas c;
        std::unique_ptr<Scene> scene = started("breathe", c, assets, is_night);
        if (!scene) continue;
        check(scene->period_ms() == 250, "the breathing scene steps 4 times a second");
        int same = 0;
        for (int f = 0; f <= 64; ++f) {
            const std::vector<uint16_t> before = snapshot(c);
            const float level = static_cast<float>(sizes[f % 64] - '0') / 7;
            scene->frame(c, Moment::at(static_cast<unsigned long>(f) * 250 + 100, is_night, level));
            const std::vector<Rect> areas = c.compose();
            if (f > 0) areas_exact(before, c, areas, "breathe " + lighting + " frame " + std::to_string(f));
            if (frame_hash(c) == want[f % 64]) ++same;
            else printf("     breathe %s frame %d: hash 0x%016llx\n", lighting.c_str(), f, static_cast<unsigned long long>(frame_hash(c)));
        }
        check(same == 65, "breathe at " + lighting + ": every frame of the loop is the animation's, pixel for pixel (" +
                              std::to_string(same) + " of 65)");
        scene->frame(c, Moment::at(16 * 250, is_night, 3.f));
        c.compose();
        check(frame_hash(c) == want[16] && sizes[16] == '7', "breathe at " + lighting + ": a level past full is full");
        scene->frame(c, Moment::at(0, is_night, std::nanf("")));
        c.compose();
        check(frame_hash(c) == want[0] && sizes[0] == '0', "breathe at " + lighting + ": a level that is not a number is empty");
    }
}

void test_registry() {
    check(!names().empty() && names().front() == "radio", "the registry lists the radio");
    check(std::count(names().begin(), names().end(), "breathe") == 1, "the registry lists the breathing scene");
    for (const std::string &name : names()) check(make(name) != nullptr, name + " is made by its name");
    check(make("nothing") == nullptr, "an unknown scene is none");
    Canvas c;
    Assets none(folder("/nonexistent"));
    for (const std::string &name : names()) check(!make(name)->start(c, none, {}), name + " without its pictures does not start");
}

}  // namespace

int main() {
    test_canvas_layers();
    test_canvas_changes();
    test_many_areas();
    test_scale2x();
    test_decode();
    test_budgets();
    test_radio_frames();
    test_breathe_frames();
    test_scene_loops();
    test_blend();
    test_registry();
    test_genres();
    test_stations();
    test_weather();
    printf("art: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
