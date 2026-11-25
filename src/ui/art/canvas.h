// The art engine's canvas (ZB-061): a picture of 160x85 art pixels composed from indexed pictures (the
// layers and sprites a scene adds), each with its own palette, into RGB565. Only what changed since the
// last compose is composed again, and only the areas whose pixels did change are returned, so that only
// they are drawn and sent to the panel (the panel's cost is the rows sent: AGENTS.md section 2). No LVGL
// here: tests/unit/art_test.cpp builds it with g++ alone, and ArtView (art_view.h) shows it.
#pragma once
#include <cstdint>
#include <vector>

namespace art {

constexpr int kW = 160, kH = 85;   // the art grid; one art pixel is 2x2 on the screen
constexpr int kMaxAreas = 16;      // compose() returns at most this many areas (more become one round them all)

// An indexed picture with its own palette (assets.h decodes one from a PNG). Index 0 is clear.
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;       // w * h palette indices, row by row
    std::vector<uint16_t> pal;     // the RGB565 colour of each index
    uint8_t at(int x, int y) const { return px[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)]; }
};

// A rectangle: its top left corner and its size.
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
    bool operator==(const Rect &o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};
Rect join(const Rect &a, const Rect &b);        // the smallest rectangle round both (an empty one adds nothing)
Rect overlap(const Rect &a, const Rect &b);      // what the two have in common, empty if nothing

class Canvas {
public:
    Canvas();
    // Takes every picture off. The next compose() looks at the whole canvas again.
    void clear();
    // Adds a picture over those added before it: the cell `src` of `img` (a frame of a sprite sheet, or the whole
    // image) with its top left at x, y. `img` must stay as it is for as long as it is on the canvas. Returns its id.
    int add(const Image &img, Rect src, int x, int y, bool shown = true);
    int add(const Image &img) { return add(img, {0, 0, img.w, img.h}, 0, 0); }   // a layer: the whole image at 0, 0
    void show(int id, bool on);
    void cell(int id, Rect src);                 // another cell of its sheet
    void move(int id, int x, int y);
    // Composes what may have changed since the last call. Returns the areas whose pixels did change (art pixels,
    // at most kMaxAreas, none overlapping); none when nothing did.
    std::vector<Rect> compose();
    const uint16_t *pixels() const { return px_.data(); }   // kW x kH, RGB565, row by row; black where nothing is
    uint16_t at(int x, int y) const { return px_[static_cast<size_t>(y) * kW + static_cast<size_t>(x)]; }

private:
    struct Item {
        const Image *img;
        Rect src;
        int x, y;
        bool shown;
        Rect bounds() const;   // where it lies on the canvas, clipped to it
    };
    void touch(const Item &item);   // its area is composed again at the next compose()
    void merge_dirty();
    Rect compose_area(const Rect &area);
    uint16_t colour_at(int x, int y, const std::vector<const Item *> &over) const;

    std::vector<Item> items_;
    std::vector<Rect> dirty_;
    std::vector<uint16_t> px_;
};

// Copies `area` of the canvas into `dst`, a screen buffer of RGB565 rows kW * 2 wide that shows art rows
// `first_row` to `first_row + rows - 1` at twice the size (each art pixel 2x2). Returns the screen pixels written,
// relative to `dst` (empty when the area lies outside those rows).
Rect scale2x(const Canvas &canvas, const Rect &area, uint16_t *dst, int first_row, int rows);

}  // namespace art
