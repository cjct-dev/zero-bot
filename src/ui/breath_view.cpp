#include "ui/breath_view.h"

#include <algorithm>
#include <cstdio>

#include "ui/theme.h"

namespace {
constexpr const char *kScene = "breathe";       // the art's scene (ui/art/scenes/breathe.cpp)
constexpr int kCountY = 44, kPhaseY = 100;      // the counter's seconds, and the phase under them
constexpr unsigned long kLingerMs = 2500;      // how long "Well done" stays up
}  // namespace

void BreathView::create(lv_obj_t *parent) {
    root_ = theme::box(parent, theme::kScreenW, theme::kScreenH, theme::kBg);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    art_.create(root_);                          // under the words
    title_ = theme::label(root_, "", theme::small(), theme::kFaint);
    count_ = theme::label(root_, "", theme::huge(), theme::kText);
    phase_ = theme::label(root_, "", theme::bold(), theme::kText);
    foot_ = theme::label(root_, "", theme::small(), theme::kFaint);
    lay_out(false);
}

void BreathView::start(const std::string &pattern_id, int minutes) {
    pattern_ = &breath::pattern(pattern_id);
    total_ms_ = breath::session_ms(*pattern_, breath::clamp_minutes(minutes));
    started_ = 0;                      // taken from the first tick, so a slow start does not eat the session
    done_at_ = 0;
    active_ = true;
    lv_label_set_text(title_, pattern_->title);
    lay_out(false);                    // the counter, until follow_art finds the art allowed
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root_);
    show(moment(0));
}

void BreathView::stop() {
    active_ = false;
    if (!root_) return;
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    art_.play("", "");
    art_.front(false, 0, true);        // lets go of the scene's pictures
}

// Over the scene the words lie on its dark bands: the session and the time left at the top, the phase and its seconds
// at the foot, next to the waves, whose rows are sent to the panel anyway (AGENTS.md section 2). On the counter: the
// session at the top, the seconds large in the middle with the phase under them, the time left at the foot.
void BreathView::lay_out(bool scene) {
    scene_ = scene;
    shown_step_ = shown_seconds_ = shown_minute_ = -1;   // every word again
    const lv_color_t quiet = theme::color(scene ? theme::kDim : theme::kFaint);
    lv_obj_set_style_text_color(title_, quiet, 0);
    lv_obj_set_style_text_color(foot_, quiet, 0);
    if (scene) {
        lv_obj_align(title_, LV_ALIGN_TOP_LEFT, 6, 4);
        lv_obj_align(foot_, LV_ALIGN_TOP_RIGHT, -6, 4);
        lv_obj_align(phase_, LV_ALIGN_BOTTOM_MID, 0, -1);
        lv_obj_add_flag(count_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_align(foot_, LV_ALIGN_BOTTOM_MID, 0, -3);
    lv_obj_align(count_, LV_ALIGN_TOP_MID, 0, kCountY);
    lv_obj_align(phase_, LV_ALIGN_TOP_MID, 0, kPhaseY);
    lv_obj_remove_flag(count_, LV_OBJ_FLAG_HIDDEN);
}

breath::Moment BreathView::moment(unsigned long now_ms) const {
    return breath::at(*pattern_, started_ ? static_cast<long>(now_ms - started_) : 0, total_ms_);
}

void BreathView::show(const breath::Moment &m) {
    const int step = m.finished ? -2 : m.step;
    const int seconds = m.finished ? 0 : m.seconds_left;
    char count[16] = "";
    if (seconds > 0) snprintf(count, sizeof(count), "%d", seconds);
    // Over the scene the phase and its seconds share one line; on the counter the seconds stand alone.
    if (step != shown_step_ || (scene_ && seconds != shown_seconds_)) {
        const std::string phase = scene_ && seconds > 0 ? std::string(m.label) + "   " + count : m.label;
        lv_label_set_text(phase_, phase.c_str());
    }
    if (!scene_ && seconds != shown_seconds_) lv_label_set_text(count_, count);
    shown_step_ = step;
    shown_seconds_ = seconds;
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
    const breath::Moment m = moment(now_ms);
    show(m);
    if (!m.finished) return false;
    if (!done_at_) { done_at_ = now_ms; return true; }
    if (now_ms - done_at_ > kLingerMs) stop();
    return false;
}

void BreathView::follow_art(bool art, unsigned long now_ms) {
    if (!active_) return;
    const breath::Moment m = moment(now_ms);
    art_.set_level(m.level);
    art_.play(art ? kScene : "", "");
    art_.front(art, now_ms, true);
    if (art_.in_front() != scene_) {   // the counter after all when the scene cannot start
        lay_out(art_.in_front());
        show(m);
    }
    art_.tick(now_ms);
}

void BreathView::show_at(unsigned long now_ms, long elapsed_ms, bool night) {
    if (!pattern_) return;
    const breath::Moment m = breath::at(*pattern_, elapsed_ms, total_ms_);
    show(m);
    if (!scene_) return;
    art_.set_level(m.level);
    art_.show_at(now_ms, static_cast<unsigned long>(elapsed_ms), night);
}
