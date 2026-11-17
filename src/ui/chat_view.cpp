#include "ui/chat_view.h"

#include <cstdio>

#include "ui/theme.h"

using namespace theme;

namespace {
constexpr int kPadX = 6;
constexpr int kInnerW = kScreenW - 2 * kPadX;
constexpr int kBars = 30;
constexpr int kListenH = 46;
constexpr int kMaxRows = 80;
}  // namespace

void ChatView::create(lv_obj_t *parent) {
    root_ = box(parent, kScreenW, kScreenH, kBg);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);

    // ---- status bar
    lv_obj_t *status = box(root_, kScreenW, kStatusH, kPanel);
    dot_ = box(status, 7, 7, kAccent);
    lv_obj_set_style_radius(dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(dot_, LV_ALIGN_LEFT_MID, kPadX, 0);
    name_ = label(status, "Zero", bold(), kText);
    lv_obj_align(name_, LV_ALIGN_LEFT_MID, kPadX + 12, 0);
    playing_ = label(status, "", small(), kDim);
    lv_label_set_long_mode(playing_, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_width(playing_, 118);
    lv_obj_set_style_text_align(playing_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(playing_, LV_ALIGN_RIGHT_MID, -kPadX - 86, 0);
    right_ = label(status, "", small(), kDim);
    lv_obj_align(right_, LV_ALIGN_RIGHT_MID, -kPadX, 0);
    net_ = label(status, LV_SYMBOL_WIFI, icons(), kFaint);
    lv_obj_align(net_, LV_ALIGN_RIGHT_MID, -kPadX - 66, 0);

    // ---- conversation
    list_ = box(root_, kScreenW, 10, kBg);
    lv_obj_set_flex_grow(list_, 1);
    lv_obj_add_flag(list_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_flex_flow(list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(list_, kPadX, 0);
    lv_obj_set_style_pad_ver(list_, 5, 0);
    lv_obj_set_style_pad_row(list_, 5, 0);
    lv_obj_set_style_width(list_, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(list_, color(kFaint), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list_, LV_OPA_COVER, LV_PART_SCROLLBAR);

    // ---- footer
    footer_ = box(root_, kScreenW, kFooterH, kPanel);
    foot_dot_ = box(footer_, 6, 6, kThink);
    lv_obj_set_style_radius(foot_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(foot_dot_, LV_ALIGN_LEFT_MID, kPadX, 0);
    foot_label_ = label(footer_, "", small(), kDim);
    lv_label_set_long_mode(foot_label_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(foot_label_, kInnerW);
    lv_obj_align(foot_label_, LV_ALIGN_LEFT_MID, kPadX, 0);

    wave_ = box(footer_, kBars * 7, 26, kPanel, true);
    lv_obj_align(wave_, LV_ALIGN_TOP_MID, 0, 3);
    for (int i = 0; i < kBars; ++i) {
        lv_obj_t *bar = box(wave_, 4, 2, kListen);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_pos(bar, i * 7 + 1, 12);
    }

    prompt_ = label(footer_, LV_SYMBOL_RIGHT, icons(), kAccent);
    lv_obj_align(prompt_, LV_ALIGN_LEFT_MID, kPadX, 0);
    input_ = lv_textarea_create(footer_);
    lv_obj_remove_style_all(input_);
    lv_textarea_set_one_line(input_, true);
    lv_obj_set_size(input_, kInnerW - 14, 16);
    lv_obj_align(input_, LV_ALIGN_LEFT_MID, kPadX + 14, 0);
    lv_obj_set_style_text_font(input_, body(), 0);
    lv_obj_set_style_text_color(input_, color(kText), 0);
    lv_obj_set_style_bg_color(input_, color(kAccent), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(input_, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_set_style_width(input_, 1, LV_PART_CURSOR);
    lv_obj_set_style_anim_duration(input_, 0, LV_PART_CURSOR);
    lv_obj_add_state(input_, LV_STATE_FOCUSED);
    lv_obj_set_scrollbar_mode(input_, LV_SCROLLBAR_MODE_OFF);

    set_mode(Mode::Hints);
}

void ChatView::show(bool visible) {
    if (visible) lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------- conversation
lv_obj_t *ChatView::add_row(bool right) {
    lv_obj_t *row = box(list_, kInnerW, LV_SIZE_CONTENT, kBg, true);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, right ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    ++count_;
    return row;
}

void ChatView::prune() {
    while (lv_obj_get_child_count(list_) > kMaxRows) {
        lv_obj_t *oldest = lv_obj_get_child(list_, 0);
        if (answer_ && lv_obj_get_parent(answer_) == oldest) break;
        lv_obj_delete(oldest);
    }
}

void ChatView::after_add() {
    prune();
    if (stick_bottom_) {
        lv_obj_update_layout(list_);
        lv_obj_scroll_to_y(list_, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

void ChatView::add_user(const std::string &text, bool steering) {
    stick_bottom_ = true;
    lv_obj_t *row = add_row(true);
    lv_obj_t *bubble = label(row, text.c_str(), body(), kText);
    lv_label_set_long_mode(bubble, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(bubble, 236, 0);
    lv_obj_set_style_bg_color(bubble, color(steering ? kPanelHi : kUser), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, 7, 0);
    lv_obj_set_style_pad_hor(bubble, 7, 0);
    lv_obj_set_style_pad_ver(bubble, 3, 0);
    after_add();
}

void ChatView::begin_answer() {
    lv_obj_t *row = add_row(false);
    answer_ = label(row, "", body(), kText);
    lv_label_set_long_mode(answer_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(answer_, kInnerW);
    lv_obj_set_style_border_side(answer_, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(answer_, 2, 0);
    lv_obj_set_style_border_color(answer_, color(kAccent), 0);
    lv_obj_set_style_pad_left(answer_, 6, 0);
    lv_obj_set_style_text_line_space(answer_, 2, 0);
    answer_text_.clear();
    answer_dirty_ = false;
    after_add();
}

void ChatView::append_answer(const std::string &delta) {
    if (!answer_) begin_answer();
    answer_text_ += delta;
    answer_dirty_ = true;   // applied in tick() so a fast stream does not relayout per token
}

void ChatView::end_answer(const std::string &full, bool interrupted) {
    if (!answer_) {
        if (full.empty()) return;
        begin_answer();
    }
    std::string text = full.empty() ? answer_text_ : full;
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    if (text.empty()) {
        lv_obj_delete(lv_obj_get_parent(answer_));
        --count_;
    } else {
        lv_label_set_text(answer_, text.c_str());
        if (interrupted) lv_obj_set_style_text_color(answer_, color(kDim), 0);
    }
    answer_ = nullptr;
    answer_dirty_ = false;
    after_add();
}

void ChatView::discard_answer() {
    if (!answer_) return;
    lv_obj_delete(lv_obj_get_parent(answer_));
    --count_;
    answer_ = nullptr;
    answer_dirty_ = false;
}

void ChatView::add_tool(const std::string &text) {
    lv_obj_t *row = add_row(false);
    const std::string line = std::string(LV_SYMBOL_RIGHT "  ") + text;
    lv_obj_t *l = label(row, line.c_str(), small(), kFaint);
    lv_obj_set_style_text_font(l, small(), 0);
    lv_obj_set_style_pad_left(l, 2, 0);
    after_add();
}

void ChatView::add_note(const std::string &text, uint32_t col) {
    lv_obj_t *row = add_row(false);
    lv_obj_t *l = label(row, text.c_str(), small(), col);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, kInnerW);
    after_add();
}

void ChatView::add_alert(const std::string &text) {
    stick_bottom_ = true;
    lv_obj_t *row = add_row(false);
    const std::string line = std::string(LV_SYMBOL_BELL "  ") + text;
    lv_obj_t *l = label(row, line.c_str(), body(), kBg);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, kInnerW);
    lv_obj_set_style_bg_color(l, color(kThink), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(l, 6, 0);
    lv_obj_set_style_pad_hor(l, 7, 0);
    lv_obj_set_style_pad_ver(l, 4, 0);
    after_add();
}

void ChatView::add_image(const std::string &image_path) {
    lv_obj_t *row = add_row(false);
    lv_obj_t *img = lv_image_create(row);
    const std::string src = "A:" + image_path;
    lv_image_set_src(img, src.c_str());
    lv_obj_set_style_radius(img, 5, 0);
    lv_obj_set_style_clip_corner(img, true, 0);
    lv_obj_set_style_border_width(img, 1, 0);
    lv_obj_set_style_border_color(img, color(kLine), 0);
    after_add();
}

void ChatView::clear() {
    lv_obj_clean(list_);
    answer_ = nullptr;
    count_ = 0;
    stick_bottom_ = true;
}

void ChatView::scroll(int dy) {
    lv_obj_scroll_by_bounded(list_, 0, dy, LV_ANIM_ON);
    lv_obj_update_layout(list_);
    // Following new messages resumes once the user is back at the bottom.
    stick_bottom_ = dy < 0 && lv_obj_get_scroll_bottom(list_) <= -dy;
}

// ---------------------------------------------------------------- status bar
void ChatView::set_status(const std::string &name, uint32_t dot_color) {
    lv_label_set_text(name_, name.c_str());
    lv_obj_set_style_bg_color(dot_, color(dot_color), 0);
}

void ChatView::set_indicators(bool online, int battery, bool charging, const std::string &clock) {
    lv_label_set_text(net_, online ? LV_SYMBOL_WIFI : LV_SYMBOL_WARNING);
    lv_obj_set_style_text_color(net_, color(online ? kDim : kListen), 0);
    char buf[64];
    if (battery >= 0) snprintf(buf, sizeof(buf), "%s  %s%d%%", clock.c_str(), charging ? "+" : "", battery);
    else snprintf(buf, sizeof(buf), "%s", clock.c_str());
    lv_label_set_text(right_, buf);
    lv_obj_set_style_text_color(right_, color(battery >= 0 && battery <= 15 && !charging ? kListen : kDim), 0);
}

void ChatView::set_now_playing(const std::string &text) {
    if (text == last_playing_) return;
    last_playing_ = text;
    const std::string line = text.empty() ? "" : "\xE2\x99\xAA " + text;   // musical note
    lv_label_set_text(playing_, line.c_str());
}

// ---------------------------------------------------------------- footer
void ChatView::set_mode(Mode m) {
    mode_ = m;
    lv_obj_set_height(footer_, m == Mode::Listening ? kListenH : kFooterH);
    auto vis = [](lv_obj_t *o, bool on) {
        if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    };
    vis(wave_, m == Mode::Listening);
    vis(input_, m == Mode::Typing);
    vis(prompt_, m == Mode::Typing);
    vis(foot_label_, m != Mode::Typing);
    vis(foot_dot_, m == Mode::Busy);
    if (m == Mode::Listening) {
        lv_obj_align(foot_label_, LV_ALIGN_BOTTOM_MID, 0, -2);
        lv_obj_set_style_text_align(foot_label_, LV_TEXT_ALIGN_CENTER, 0);
    } else {
        lv_obj_align(foot_label_, LV_ALIGN_LEFT_MID, m == Mode::Busy ? kPadX + 11 : kPadX, 0);
        lv_obj_set_style_text_align(foot_label_, LV_TEXT_ALIGN_LEFT, 0);
    }
    if (stick_bottom_) {
        lv_obj_update_layout(root_);
        lv_obj_scroll_to_y(list_, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

void ChatView::footer_hints(const std::string &text) {
    if (mode_ == Mode::Typing) return;
    if (mode_ != Mode::Hints) set_mode(Mode::Hints);
    if (text != hints_) {
        hints_ = text;
        lv_label_set_text(foot_label_, text.c_str());
        lv_obj_set_style_text_color(foot_label_, color(kFaint), 0);
    }
}

void ChatView::footer_busy(const std::string &text, uint32_t col) {
    if (mode_ == Mode::Typing) return;   // keep the text field while the user types over a running request
    if (mode_ != Mode::Busy) { set_mode(Mode::Busy); hints_.clear(); }
    busy_label_ = text;
    lv_obj_set_style_bg_color(foot_dot_, color(col), 0);
    lv_obj_set_style_text_color(foot_label_, color(kDim), 0);
}

void ChatView::footer_listening(float level, double seconds, bool has_audio) {
    if (mode_ != Mode::Listening) {
        if (mode_ == Mode::Typing) lv_textarea_set_text(input_, "");
        levels_.assign(kBars, 0.f);
        set_mode(Mode::Listening);
        hints_.clear();
        lv_obj_set_style_text_color(foot_label_, color(kDim), 0);
    }
    levels_.push_back(level);
    while (levels_.size() > kBars) levels_.pop_front();
    for (int i = 0; i < kBars; ++i) {
        lv_obj_t *bar = lv_obj_get_child(wave_, i);
        const int h = 2 + static_cast<int>(levels_[static_cast<size_t>(i)] * 24);
        lv_obj_set_height(bar, h);
        lv_obj_set_y(bar, 13 - h / 2);
        lv_obj_set_style_bg_color(bar, color(has_audio ? kListen : kFaint), 0);
    }
    char buf[96];
    if (!has_audio) snprintf(buf, sizeof(buf), "Starting microphone…");
    else snprintf(buf, sizeof(buf), "Listening  %d:%02d   ·   TAB send   ·   ESC cancel", static_cast<int>(seconds) / 60,
                  static_cast<int>(seconds) % 60);
    lv_label_set_text(foot_label_, buf);
}

void ChatView::typing_begin(const std::string &initial) {
    lv_textarea_set_text(input_, initial.c_str());
    set_mode(Mode::Typing);
    hints_.clear();
}

void ChatView::typing_end() {
    lv_textarea_set_text(input_, "");
    set_mode(Mode::Hints);
    hints_.clear();
}

void ChatView::typing_key(const KeyEvent &ev) {
    switch (ev.key) {
        case Key::Char: lv_textarea_add_text(input_, ev.text.c_str()); break;
        case Key::Backspace: lv_textarea_delete_char(input_); break;
        case Key::Delete: lv_textarea_delete_char_forward(input_); break;
        case Key::Left: lv_textarea_cursor_left(input_); break;
        case Key::Right: lv_textarea_cursor_right(input_); break;
        case Key::Home: lv_textarea_set_cursor_pos(input_, 0); break;
        case Key::End: lv_textarea_set_cursor_pos(input_, LV_TEXTAREA_CURSOR_LAST); break;
        default: break;
    }
}

std::string ChatView::typing_text() const { return lv_textarea_get_text(input_); }

void ChatView::tick() {
    ++anim_;
    if (answer_ && answer_dirty_) {
        lv_label_set_text(answer_, answer_text_.c_str());
        answer_dirty_ = false;
        after_add();
    }
    if (mode_ == Mode::Busy && anim_ % 4 == 0) {
        static const char *dots[] = {"", ".", "..", "..."};
        // Labels that already carry a hint (after a separator) are shown as they are.
        const bool plain = busy_label_.find("\xC2\xB7") != std::string::npos;
        const std::string text = busy_label_ + (plain ? "" : dots[(anim_ / 4) % 4]);
        lv_label_set_text(foot_label_, text.c_str());
    }
}
