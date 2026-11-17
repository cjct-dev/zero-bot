#include "ui/breath_view.h"

#include <algorithm>
#include <cstdio>

#include "ui/theme.h"

namespace {
constexpr int kOrbMin = 30, kOrbMax = 104;     // diameters in pixels
constexpr int kOrbCentreY = 76;
constexpr unsigned long kLingerMs = 2500;      // how long "Well done" stays up
}  // namespace

void BreathView::create(lv_obj_t *parent) {
    root_ = theme::box(parent, theme::kScreenW, theme::kScreenH, theme::kBg);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);

    title_ = theme::label(root_, "", theme::small(), theme::kFaint);
    lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, 4);

    // A faint ring marks a full breath, so the eye knows how far the orb will grow.
    ring_ = theme::box(root_, kOrbMax + 8, kOrbMax + 8, theme::kBg, true);
    lv_obj_set_style_radius(ring_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring_, 1, 0);
    lv_obj_set_style_border_color(ring_, theme::color(theme::kLine), 0);
    lv_obj_align(ring_, LV_ALIGN_TOP_MID, 0, kOrbCentreY - (kOrbMax + 8) / 2);

    orb_ = theme::box(root_, kOrbMin, kOrbMin, theme::kAccent);
    lv_obj_set_style_radius(orb_, LV_RADIUS_CIRCLE, 0);

    count_ = theme::label(root_, "", theme::bold(), theme::kBg);
    phase_ = theme::label(root_, "", theme::bold(), theme::kText);
    lv_obj_align(phase_, LV_ALIGN_TOP_MID, 0, kOrbCentreY + kOrbMax / 2 + 10);
    foot_ = theme::label(root_, "", theme::small(), theme::kFaint);
    lv_obj_align(foot_, LV_ALIGN_BOTTOM_MID, 0, -3);
}

void BreathView::start(const std::string &pattern_id, int minutes) {
    pattern_ = &breath::pattern(pattern_id);
    total_ms_ = breath::session_ms(*pattern_, breath::clamp_minutes(minutes));
    started_ = 0;                      // taken from the first tick, so a slow start does not eat the session
    done_at_ = 0;
    active_ = true;
    shown_step_ = shown_seconds_ = shown_size_ = shown_minute_ = -1;
    lv_label_set_text(title_, pattern_->title);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root_);
    show_at(0);
}

void BreathView::stop() {
    active_ = false;
    if (root_) lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void BreathView::show_at(long elapsed_ms) {
    if (!pattern_) return;
    const breath::Moment m = breath::at(*pattern_, elapsed_ms, total_ms_);
    // Even sizes keep the circle centred on a whole pixel, so it does not wobble as it grows.
    const int size = (kOrbMin + static_cast<int>(m.level * (kOrbMax - kOrbMin) + 0.5f)) & ~1;
    if (size != shown_size_) {
        shown_size_ = size;
        lv_obj_set_size(orb_, size, size);
        lv_obj_align(orb_, LV_ALIGN_TOP_MID, 0, kOrbCentreY - size / 2);
        // Brighter when full, softer when empty.
        lv_obj_set_style_bg_opa(orb_, static_cast<lv_opa_t>(120 + m.level * 135), 0);
    }
    if (m.step != shown_step_ || m.finished) {
        shown_step_ = m.finished ? -2 : m.step;
        lv_label_set_text(phase_, m.label);
    }
    const int seconds = m.finished ? 0 : m.seconds_left;
    if (seconds != shown_seconds_) {
        shown_seconds_ = seconds;
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", seconds);
        lv_label_set_text(count_, seconds > 0 ? buf : "");
    }
    lv_obj_align(count_, LV_ALIGN_TOP_MID, 0, kOrbCentreY - 7);
    // The time left changes once a minute on screen; a ticking clock would be one more thing to watch.
    // Rounded to the nearest minute: a session runs a few seconds over so that it ends on a whole breath.
    const int minutes_left = m.finished ? 0 : std::max(1, static_cast<int>((m.remaining_ms + 30000) / 60000));
    if (minutes_left != shown_minute_) {
        shown_minute_ = minutes_left;
        char buf[64];
        if (m.finished) snprintf(buf, sizeof(buf), " ");
        else snprintf(buf, sizeof(buf), "%d min left   ·   ESC to finish", minutes_left);
        lv_label_set_text(foot_, buf);
    }
}

bool BreathView::tick(unsigned long now_ms) {
    if (!active_) return false;
    if (!started_) started_ = now_ms ? now_ms : 1;
    const long elapsed = static_cast<long>(now_ms - started_);
    show_at(elapsed);
    if (elapsed < total_ms_) return false;
    if (!done_at_) { done_at_ = now_ms; return true; }
    if (now_ms - done_at_ > kLingerMs) stop();
    return false;
}
