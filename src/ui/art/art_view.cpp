#include "ui/art/art_view.h"

#include <cstdio>
#include <cstring>
#include <utility>

#include "core/dusk.h"
#include "ui/theme.h"

// The PNGs under assets/art/, built into the program: a table the build writes (cmake/embed.cmake).
extern "C" {
struct ZbArtFile {
    const char *path;
    const unsigned char *data;
    unsigned long size;
};
extern const ZbArtFile zb_art_files[];
extern const unsigned long zb_art_file_count;
}

namespace art {

namespace {

// The chat's stage: art rows 9 to 75, screen rows 18 to 151, between the status bar and the bottom row.
constexpr int kStageFirstRow = theme::kStatusH / 2;
constexpr int kStageRows = (theme::kScreenH - theme::kStatusH - theme::kFooterH) / 2;
constexpr int kWidth = kW * 2;                        // in screen pixels, whatever the place
constexpr int kBlock = 4;                             // the dissolve's blocks: 2x2 art pixels
constexpr int kBlockColumns = kWidth / kBlock;
constexpr unsigned long kLightingEveryMs = 60000;    // how often the lighting is looked at while a scene shows

Source embedded() {
    return [](const std::string &path) {
        for (unsigned long i = 0; i < zb_art_file_count; ++i)
            if (path == zb_art_files[i].path) return std::string(reinterpret_cast<const char *>(zb_art_files[i].data), zb_art_files[i].size);
        return std::string();
    };
}

// The dissolve's blocks (row * kBlockColumns + column) over `height` screen rows, shuffled once with a fixed seed:
// the same pattern every time, and in every --shots run.
std::vector<uint16_t> shuffled_blocks(int height) {
    const int rows = (height + kBlock - 1) / kBlock;
    std::vector<uint16_t> order(static_cast<size_t>(kBlockColumns * rows));
    for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<uint16_t>(i);
    uint32_t seed = 7;
    for (size_t i = order.size() - 1; i > 0; --i) {
        seed = seed * 1103515245u + 12345u;
        std::swap(order[i], order[(seed >> 8) % (i + 1)]);
    }
    return order;
}

void set_hidden(lv_obj_t *obj, bool hidden) {
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace

ArtView::ArtView(Place place)
    : first_row_(place == Place::Stage ? kStageFirstRow : 0), rows_(place == Place::Stage ? kStageRows : kH), assets_(embedded()) {}

void ArtView::create(lv_obj_t *parent) {
    // The RGB565 plane, then the alpha plane LVGL reads right after it while the view dissolves (RGB565A8).
    stage_.assign(pixels() + pixels() / 2, 0);
    order_ = shuffled_blocks(rows_ * 2);
    for (lv_image_dsc_t *dsc : {&solid_, &holed_}) {
        const bool solid = dsc == &solid_;
        dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc->header.cf = solid ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_RGB565A8;
        dsc->header.w = kWidth;
        dsc->header.h = static_cast<uint32_t>(rows_ * 2);
        dsc->header.stride = kWidth * 2;
        dsc->data_size = static_cast<uint32_t>(pixels() * (solid ? 2 : 3));
        dsc->data = reinterpret_cast<const uint8_t *>(stage_.data());
    }
    img_ = lv_image_create(parent);
    lv_obj_remove_flag(img_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(img_, 0, 2 * first_row_);
    set_opaque(true);
    set_hidden(img_, true);
    title_ = theme::label(img_, "", theme::bold(), theme::kText);
    lv_label_set_long_mode(title_, LV_LABEL_LONG_MODE_DOTS);   // a long name never scrolls: it would redraw all the time
    lv_obj_set_height(title_, lv_font_get_line_height(theme::bold()));
    set_hidden(title_, true);
}

void ArtView::play(const std::string &scene, const std::string &title) {
    if (title != lv_label_get_text(title_)) lv_label_set_text(title_, title.c_str());
    playing_ = scene;
    // A scene that stops while on the screen is let go once it has dissolved away (step).
    if (scene == name_ || (scene.empty() && on_screen())) return;
    load(scene);
}

// Lets go of the scene shown (hidden at once) and takes up `scene`, not yet started: front() starts it.
void ArtView::load(const std::string &scene) {
    name_ = scene;
    front_ = started_ = false;
    step_ = kSteps;
    set_hidden(img_, true);
    set_hidden(title_, true);
    canvas_.clear();
    assets_.clear();
    scene_ = scene.empty() ? nullptr : make(scene);
    if (!scene_) return;
    const Spot spot = scene_->title_spot();
    lv_obj_set_pos(title_, 2 * spot.x, 2 * (spot.y - first_row_));
    lv_obj_set_width(title_, 2 * spot.w);
}

void ArtView::front(bool on, unsigned long now, bool at_once) {
    if (!scene_) on = false;
    if (on == front_ && !(at_once && step_ < kSteps)) return;
    front_ = on;
    if (on && !catch_up(now)) return;
    set_hidden(title_, true);
    if (at_once) {
        step_ = kSteps;
        set_opaque(true);
        set_hidden(img_, !on);
        set_hidden(title_, !on);
        if (!on && playing_ != name_) load(playing_);
        return;
    }
    if (!on_screen()) fill_alpha(0);       // in from nothing
    else if (opaque_) fill_alpha(0xFF);    // out from all of it (a dissolve under way goes on from where it is)
    set_opaque(false);
    set_hidden(img_, false);
    step_ = 0;
    next_step_ = now;
}

// The scene composed as it is at `now`: started (in the lighting of the local sunset: core/dusk.h) the first time,
// and again when the lighting has changed since.
bool ArtView::catch_up(unsigned long now) {
    const bool night = dusk::now_light().night;
    night_checked_ = now;
    if (!started_) clock_ = now;
    if (!started_ || night != night_) return begin(now - clock_, night);
    draw_frame(now);
    return true;
}

// Starts the scene in a lighting at `ms` on its clock, composed whole. False (the scene let go of) when its
// pictures are missing.
bool ArtView::begin(unsigned long ms, bool night) {
    canvas_.clear();
    assets_.clear();
    const Moment m{ms, night, level_};
    started_ = scene_->start(canvas_, assets_, m);
    if (!started_) {
        fprintf(stderr, "art: the %s scene's pictures are missing\n", name_.c_str());
        front_ = false;
        scene_.reset();
        return false;
    }
    night_ = night;
    frame_ = ms / scene_->period_ms();
    scene_->frame(canvas_, m);
    ++frames_;
    draw(canvas_.compose());
    return true;
}

void ArtView::draw_frame(unsigned long now) {
    const unsigned long ms = now - clock_;
    frame_ = ms / scene_->period_ms();
    scene_->frame(canvas_, {ms, night_, level_});
    ++frames_;
    draw(canvas_.compose());
}

// Copies what changed into the stage and has LVGL redraw exactly that.
void ArtView::draw(const std::vector<Rect> &areas) {
    for (const Rect &a : areas) {
        const Rect r = scale2x(canvas_, a, stage_.data(), first_row_, rows_);
        if (r.empty()) continue;
        const int top = 2 * first_row_ + r.y;
        const lv_area_t area{r.x, top, r.x + r.w - 1, top + r.h - 1};
        lv_obj_invalidate_area(img_, &area);
    }
}

void ArtView::tick(unsigned long now) {
    if (!scene_ || !on_screen()) return;
    if (step_ < kSteps) {
        if (now < next_step_) return;
        step();
        next_step_ = now + kStepMs;   // at least 50 ms apart, however late this one came
        return;
    }
    if (now - night_checked_ >= kLightingEveryMs && !catch_up(now)) {
        set_hidden(img_, true);
        return;
    }
    if ((now - clock_) / scene_->period_ms() != frame_) draw_frame(now);
}

unsigned long ArtView::ms_to_next(unsigned long now) const {
    if (!scene_ || !on_screen()) return kNever;
    const unsigned long at = step_ < kSteps ? next_step_ : clock_ + (frame_ + 1) * scene_->period_ms();
    return at > now ? at - now : 0;
}

bool ArtView::on_screen() const { return img_ && !lv_obj_has_flag(img_, LV_OBJ_FLAG_HIDDEN); }

// One step of the dissolve: a sixth of the blocks shown, or cleared. In front at the end, the view is opaque
// again (LVGL then draws nothing under it) with its title; gone, it is hidden (and a scene that has stopped
// meanwhile let go of).
void ArtView::step() {
    const size_t from = order_.size() * static_cast<size_t>(step_) / kSteps, to = order_.size() * static_cast<size_t>(step_ + 1) / kSteps;
    uint8_t *alpha = reinterpret_cast<uint8_t *>(stage_.data() + pixels());
    for (size_t i = from; i < to; ++i) {
        const int x = order_[i] % kBlockColumns * kBlock, y = order_[i] / kBlockColumns * kBlock;
        for (int row = y; row < y + kBlock && row < rows_ * 2; ++row)
            memset(alpha + static_cast<size_t>(row) * kWidth + static_cast<size_t>(x), front_ ? 0xFF : 0, kBlock);
    }
    lv_obj_invalidate(img_);
    if (++step_ < kSteps) return;
    if (front_) {
        set_opaque(true);
        set_hidden(title_, false);
        return;
    }
    set_hidden(img_, true);
    if (playing_ != name_) load(playing_);
}

// RGB565 while the view is in front (LVGL then draws nothing under it), RGB565A8 while it dissolves: two
// descriptions of the same pixels, each with a format of its own, so that nothing LVGL keeps of one goes stale.
void ArtView::set_opaque(bool opaque) {
    if (opaque == opaque_ && lv_image_get_src(img_)) return;
    opaque_ = opaque;
    lv_image_set_src(img_, opaque ? &solid_ : &holed_);
}

void ArtView::fill_alpha(uint8_t alpha) { memset(stage_.data() + pixels(), alpha, pixels()); }

void ArtView::show_at(unsigned long now, unsigned long ms, bool night) {
    if (!scene_) return;
    clock_ = now - ms;
    if (!begin(ms, night)) return;
    night_checked_ = now;
    front_ = true;
    step_ = kSteps;
    set_opaque(true);
    set_hidden(img_, false);
    set_hidden(title_, false);
}

}  // namespace art
