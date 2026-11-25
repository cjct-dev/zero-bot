// Unit tests for the art engine (src/ui/art, no LVGL): the canvas (pictures in order, clear pixels, edges, the
// areas it returns, at most kMaxAreas of them, the 2x copy for the screen), PNG decoding (the repository's
// pictures, and broken or hostile files), every scene's frames on a fake clock through a model of the panel
// against a budget per scene (a tripwire: raise it knowingly), the radio's frames against the finished animation
// (each frame's hash, taken from the animation's own frames when its pictures were exported) and the registry. Run
// from the repository (it reads assets/art/). Each failure prints a line.
#include "ui/art/assets.h"
#include "ui/art/canvas.h"
#include "ui/art/scene.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
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

    Assets assets(files);
    const Image *a = assets.image("radio/sky-dusk.png");
    check(a && assets.image("radio/sky-dusk.png") == a, "a picture is decoded once and kept");
    check(!assets.image("radio/none.png"), "a missing picture is none");
}

// ------------------------------------------------------------------ the scenes
// The panel (AGENTS.md section 2; the kernel's deferred I/O for the framebuffer): the first write after an update opens a
// 50 ms window, everything written in it goes in one update, and an update is one full-width band from the first to
// the last 4 KiB page written. Only the chat's stage (art rows 9 to 75) reaches the screen.
class Panel {
public:
    void write(unsigned long t, const Rect &art) {
        const Rect r = overlap(art, {0, kStageFirst, kW, kStageRows});
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
    static constexpr int kStageFirst = 9, kStageRows = 67;

private:
    static constexpr long kRowBytes = 640, kPage = 4096, kScreenBytes = 640 * 170;
    bool open_ = false;
    unsigned long since_ = 0;
    int first_ = 0, last_ = 0;
    long bytes_ = 0;
};

// What each scene may send the panel, in kB a second over 20 s in either lighting: a tripwire, as the first
// request's 32,000 bytes is. The radio's finished animation measured 217.
const std::map<std::string, double> kBudgetKBps = {{"radio", 250}};

// A scene started on a fresh canvas in a lighting; nullptr (with a failure) when it cannot start.
std::unique_ptr<Scene> started(const std::string &name, Canvas &c, Assets &assets, bool night) {
    std::unique_ptr<Scene> scene = make(name);
    const bool ok = scene && scene->start(c, assets, {0, night});
    check(ok, name + " starts " + (night ? "at night" : "at dusk"));
    return ok ? std::move(scene) : nullptr;
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
            Panel panel;
            constexpr unsigned long kRunMs = 20000;
            for (unsigned long t = 0; t < kRunMs; t += scene->period_ms()) {
                scene->frame(c, {t, night});
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

// FNV-1a over a frame's RGB565 values, little-endian, row by row, as the export took it of the animation's frames.
uint64_t frame_hash(const Canvas &c) {
    uint64_t h = 0xcbf29ce484222325ull;
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
            scene->frame(c, {static_cast<unsigned long>(f) * 250 + 100, is_night});   // any time within the frame
            const std::vector<Rect> areas = c.compose();
            if (f > 0) areas_exact(before, c, areas, "radio " + lighting + " frame " + std::to_string(f));
            if (frame_hash(c) == want[f % 16]) ++same;
            else printf("     radio %s frame %d: hash 0x%016llx\n", lighting.c_str(), f, static_cast<unsigned long long>(frame_hash(c)));
        }
        check(same == 17, "radio at " + lighting + ": every frame of the loop is the animation's, pixel for pixel (" +
                              std::to_string(same) + " of 17)");
    }
}

void test_registry() {
    check(!names().empty() && names().front() == "radio", "the registry lists the radio");
    for (const std::string &name : names()) check(make(name) != nullptr, name + " is made by its name");
    check(make("nothing") == nullptr, "an unknown scene is none");
    Canvas c;
    Assets none(folder("/nonexistent"));
    check(!make("radio")->start(c, none, {0, false}), "a scene without its pictures does not start");
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
    test_registry();
    printf("art: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
