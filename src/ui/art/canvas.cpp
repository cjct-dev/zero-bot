#include "ui/art/canvas.h"

#include <algorithm>
#include <climits>

namespace art {

namespace {

constexpr Rect kWhole{0, 0, kW, kH};
constexpr int kJoinSlack = 32;   // two areas this close to touching (in art pixels wasted) are composed as one

long area_of(const Rect &r) { return r.empty() ? 0 : static_cast<long>(r.w) * r.h; }

}  // namespace

Rect join(const Rect &a, const Rect &b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const int x1 = std::min(a.x, b.x), y1 = std::min(a.y, b.y);
    const int x2 = std::max(a.x + a.w, b.x + b.w), y2 = std::max(a.y + a.h, b.y + b.h);
    return {x1, y1, x2 - x1, y2 - y1};
}

Rect overlap(const Rect &a, const Rect &b) {
    const int x1 = std::max(a.x, b.x), y1 = std::max(a.y, b.y);
    const int x2 = std::min(a.x + a.w, b.x + b.w), y2 = std::min(a.y + a.h, b.y + b.h);
    if (x2 <= x1 || y2 <= y1) return {};
    return {x1, y1, x2 - x1, y2 - y1};
}

Rect Canvas::Item::bounds() const { return overlap({x, y, src.w, src.h}, kWhole); }

Canvas::Canvas() : px_(static_cast<size_t>(kW) * kH, 0) {}

void Canvas::clear() {
    items_.clear();
    dirty_.assign(1, kWhole);
}

int Canvas::add(const Image &img, Rect src, int x, int y, bool shown) {
    items_.push_back({&img, overlap(src, {0, 0, img.w, img.h}), x, y, shown});
    if (shown) touch(items_.back());
    return static_cast<int>(items_.size()) - 1;
}

void Canvas::show(int id, bool on) {
    Item &item = items_[static_cast<size_t>(id)];
    if (item.shown == on) return;
    item.shown = on;
    touch(item);
}

void Canvas::cell(int id, Rect src) {
    Item &item = items_[static_cast<size_t>(id)];
    src = overlap(src, {0, 0, item.img->w, item.img->h});
    if (src == item.src) return;
    if (item.shown) touch(item);
    item.src = src;
    if (item.shown) touch(item);
}

void Canvas::move(int id, int x, int y) {
    Item &item = items_[static_cast<size_t>(id)];
    if (item.x == x && item.y == y) return;
    if (item.shown) touch(item);
    item.x = x;
    item.y = y;
    if (item.shown) touch(item);
}

void Canvas::touch(const Item &item) {
    const Rect r = item.bounds();
    if (!r.empty()) dirty_.push_back(r);
}

// Areas that overlap are joined (each pixel is composed once, and the areas returned do not overlap), and so are
// areas whose join wastes little; past kMaxAreas, one area round them all. The panel sends whole rows anyway.
void Canvas::merge_dirty() {
    for (bool merged = true; merged;) {
        merged = false;
        for (size_t i = 0; i < dirty_.size() && !merged; ++i)
            for (size_t j = i + 1; j < dirty_.size() && !merged; ++j) {
                const Rect u = join(dirty_[i], dirty_[j]);
                if (!overlap(dirty_[i], dirty_[j]).empty() || area_of(u) <= area_of(dirty_[i]) + area_of(dirty_[j]) + kJoinSlack) {
                    dirty_[i] = u;
                    dirty_.erase(dirty_.begin() + static_cast<long>(j));
                    merged = true;
                }
            }
    }
    if (dirty_.size() > static_cast<size_t>(kMaxAreas)) {
        Rect all;
        for (const Rect &r : dirty_) all = join(all, r);
        dirty_.assign(1, all);
    }
}

std::vector<Rect> Canvas::compose() {
    merge_dirty();
    std::vector<Rect> changed;
    for (const Rect &r : dirty_) {
        const Rect c = compose_area(r);
        if (!c.empty()) changed.push_back(c);
    }
    dirty_.clear();
    return changed;
}

// Composes one area; returns the smallest rectangle round the pixels that changed in it.
Rect Canvas::compose_area(const Rect &area) {
    std::vector<const Item *> over;   // the pictures that reach into the area, bottom first
    for (const Item &item : items_)
        if (item.shown && !overlap(item.bounds(), area).empty()) over.push_back(&item);
    int x1 = INT_MAX, y1 = INT_MAX, x2 = INT_MIN, y2 = INT_MIN;
    for (int y = area.y; y < area.y + area.h; ++y)
        for (int x = area.x; x < area.x + area.w; ++x) {
            const uint16_t c = colour_at(x, y, over);
            uint16_t &px = px_[static_cast<size_t>(y) * kW + static_cast<size_t>(x)];
            if (c == px) continue;
            px = c;
            x1 = std::min(x1, x); y1 = std::min(y1, y);
            x2 = std::max(x2, x); y2 = std::max(y2, y);
        }
    if (x2 < x1) return {};
    return {x1, y1, x2 - x1 + 1, y2 - y1 + 1};
}

// The topmost picture with a colour at x, y decides it; black where none has one.
uint16_t Canvas::colour_at(int x, int y, const std::vector<const Item *> &over) const {
    for (auto it = over.rbegin(); it != over.rend(); ++it) {
        const Item &item = **it;
        const int ix = x - item.x, iy = y - item.y;
        if (ix < 0 || iy < 0 || ix >= item.src.w || iy >= item.src.h) continue;
        const uint8_t index = item.img->at(item.src.x + ix, item.src.y + iy);
        if (index && index < item.img->pal.size()) return item.img->pal[index];
    }
    return 0;
}

Rect scale2x(const Canvas &canvas, const Rect &area, uint16_t *dst, int first_row, int rows) {
    const Rect a = overlap(area, {0, first_row, kW, rows});
    if (a.empty()) return {};
    constexpr size_t kStride = static_cast<size_t>(kW) * 2;
    for (int y = a.y; y < a.y + a.h; ++y) {
        uint16_t *top = dst + static_cast<size_t>(2 * (y - first_row)) * kStride, *bottom = top + kStride;
        for (int x = a.x; x < a.x + a.w; ++x) {
            const uint16_t c = canvas.at(x, y);
            top[2 * x] = top[2 * x + 1] = bottom[2 * x] = bottom[2 * x + 1] = c;
        }
    }
    return {2 * a.x, 2 * (a.y - first_row), 2 * a.w, 2 * a.h};
}

}  // namespace art
