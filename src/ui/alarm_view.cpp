#include "ui/alarm_view.h"

#include "ui/theme.h"

using namespace theme;

namespace {
constexpr int kW = 296, kH = 100, kPad = 12;
}  // namespace

void AlarmView::create(lv_obj_t *parent) {
    // Nothing on it moves: it is drawn once when it appears and once when it goes.
    root_ = box(parent, kW, kH, kThink);
    lv_obj_set_style_radius(root_, 8, 0);
    lv_obj_set_style_border_width(root_, 2, 0);
    lv_obj_set_style_border_color(root_, color(kText), 0);
    lv_obj_align(root_, LV_ALIGN_CENTER, 0, kStatusH / 2);   // between the status bar and the footer
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *bell = label(root_, LV_SYMBOL_BELL, icons(), kBg);
    lv_obj_align(bell, LV_ALIGN_TOP_LEFT, kPad, 10);
    title_ = label(root_, "", bold(), kBg);
    lv_obj_align(title_, LV_ALIGN_TOP_LEFT, kPad + 19, 9);

    // Two lines of the timers' names at most, cut with an ellipsis beyond that.
    labels_ = label(root_, "", large(), kBg);
    lv_label_set_long_mode(labels_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(labels_, kW - 2 * kPad, 2 * lv_font_get_line_height(large()));
    lv_obj_align(labels_, LV_ALIGN_TOP_LEFT, kPad, 29);

    lv_obj_t *hint = label(root_, "Any key to stop   \xC2\xB7   TAB to talk", small(), kPanel);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, kPad, -8);
}

void AlarmView::show(const std::string &title, const std::string &labels) {
    if (title != shown_title_) {
        shown_title_ = title;
        lv_label_set_text(title_, title.c_str());
    }
    if (labels != shown_labels_) {
        shown_labels_ = labels;
        lv_label_set_text(labels_, labels.c_str());
    }
    if (visible_) return;
    visible_ = true;
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    raise();
}

void AlarmView::hide() {
    if (!visible_) return;
    visible_ = false;
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void AlarmView::raise() { lv_obj_move_foreground(root_); }
