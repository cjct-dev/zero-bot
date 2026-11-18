#include "ui/menu.h"

#include <algorithm>
#include <cstdlib>

#include "ui/theme.h"

using namespace theme;

namespace {
constexpr int kRowH = 19;
constexpr int kTitleH = 18;
constexpr int kHintH = 15;
constexpr int kListH = kScreenH - kTitleH - kHintH;
constexpr int kVisible = kListH / kRowH;
constexpr int kReadRowH = 16;                   // rows on a page to read sit closer together
constexpr int kReadVisible = kListH / kReadRowH;
constexpr int kPairX = 10 + Menu::kPairKeyW;    // where a Pair row's second column starts
}  // namespace

int Menu::pair_text_width() { return kScreenW - kPairX - 8; }

MenuItem MenuItem::pair(const std::string &key, const std::string &what) {
    MenuItem i;
    i.kind = Kind::Pair;
    i.label = key;
    i.value = [what] { return what; };
    return i;
}

MenuItem MenuItem::header(const std::string &text) {
    MenuItem i;
    i.kind = Kind::Header;
    i.label = text;
    return i;
}

MenuItem MenuItem::text(const std::string &text) {
    MenuItem i;
    i.kind = Kind::Text;
    i.label = text;
    return i;
}

void Menu::add_paragraph(std::vector<MenuItem> &items, const std::string &paragraph) {
    const size_t width = 47;   // characters of body text that fit across the screen
    size_t pos = 0;
    while (pos < paragraph.size()) {
        size_t len = std::min(width, paragraph.size() - pos);
        if (pos + len < paragraph.size()) {
            const size_t space = paragraph.rfind(' ', pos + len);
            if (space != std::string::npos && space > pos) len = space - pos;
        }
        items.push_back(MenuItem::text(paragraph.substr(pos, len)));
        pos += len;
        while (pos < paragraph.size() && paragraph[pos] == ' ') ++pos;
    }
}

MenuItem MenuItem::action(const std::string &text, std::function<void()> fn, bool danger) {
    MenuItem i;
    i.kind = Kind::Action;
    i.label = text;
    i.activate = std::move(fn);
    i.danger = danger;
    return i;
}

MenuItem MenuItem::val(const std::string &text, std::function<std::string()> value, std::function<void()> fn,
                       std::function<void(int)> adjust) {
    MenuItem i;
    i.kind = Kind::Value;
    i.label = text;
    i.value = std::move(value);
    i.activate = std::move(fn);
    i.adjust = std::move(adjust);
    return i;
}

void Menu::create(lv_obj_t *parent) {
    root_ = box(parent, kScreenW, kScreenH, kBg);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *bar = box(root_, kScreenW, kTitleH, kPanel);
    title_ = label(bar, "", bold(), kText);
    lv_obj_align(title_, LV_ALIGN_LEFT_MID, 6, 0);

    list_ = box(root_, kScreenW, kListH, kBg);
    lv_obj_set_pos(list_, 0, kTitleH);

    lv_obj_t *foot = box(root_, kScreenW, kHintH, kPanel);
    lv_obj_set_pos(foot, 0, kScreenH - kHintH);
    hint_ = label(foot, "", small(), kFaint);
    lv_obj_align(hint_, LV_ALIGN_LEFT_MID, 6, 0);

    // Text editor overlay
    editor_ = box(root_, kScreenW, kScreenH - kTitleH, kBg);
    lv_obj_set_pos(editor_, 0, kTitleH);
    lv_obj_add_flag(editor_, LV_OBJ_FLAG_HIDDEN);
    edit_title_ = label(editor_, "", small(), kDim);
    lv_obj_set_pos(edit_title_, 8, 10);
    lv_obj_t *field_bg = box(editor_, kScreenW - 16, 26, kPanel);
    lv_obj_set_pos(field_bg, 8, 28);
    lv_obj_set_style_radius(field_bg, 5, 0);
    lv_obj_set_style_border_width(field_bg, 1, 0);
    lv_obj_set_style_border_color(field_bg, color(kAccent), 0);
    edit_field_ = lv_textarea_create(field_bg);
    lv_obj_remove_style_all(edit_field_);
    lv_textarea_set_one_line(edit_field_, true);
    lv_obj_set_size(edit_field_, kScreenW - 30, 16);
    lv_obj_align(edit_field_, LV_ALIGN_LEFT_MID, 7, 0);
    lv_obj_set_style_text_font(edit_field_, body(), 0);
    lv_obj_set_style_text_color(edit_field_, color(kText), 0);
    lv_obj_set_style_bg_color(edit_field_, color(kAccent), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(edit_field_, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_set_style_width(edit_field_, 1, LV_PART_CURSOR);
    lv_obj_set_style_anim_duration(edit_field_, 0, LV_PART_CURSOR);
    lv_obj_add_state(edit_field_, LV_STATE_FOCUSED);
    lv_obj_set_scrollbar_mode(edit_field_, LV_SCROLLBAR_MODE_OFF);
    edit_help_ = label(editor_, "", small(), kFaint);
    lv_label_set_long_mode(edit_help_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(edit_help_, kScreenW - 16);
    lv_obj_set_pos(edit_help_, 8, 62);

    toast_ = label(root_, "", small(), kBg);
    lv_obj_set_style_bg_color(toast_, color(kOk), 0);
    lv_obj_set_style_bg_opa(toast_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(toast_, 6, 0);
    lv_obj_set_style_pad_hor(toast_, 8, 0);
    lv_obj_set_style_pad_ver(toast_, 3, 0);
    lv_obj_set_style_max_width(toast_, kScreenW - 24, 0);
    lv_obj_align(toast_, LV_ALIGN_BOTTOM_MID, 0, -kHintH - 6);
    lv_obj_add_flag(toast_, LV_OBJ_FLAG_HIDDEN);
}

void Menu::build_items() {
    Frame &f = stack_.back();
    if (f.page.revision) f.revision = f.page.revision();
    items_ = f.page.build();
}

void Menu::push(MenuPage page) {
    Frame frame;
    frame.page = std::move(page);
    stack_.push_back(std::move(frame));
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    build_items();
    Frame &f = stack_.back();
    if (f.page.reading) {
        f.selected = -1;   // nothing to choose
        f.scroll = f.page.top;
    } else {
        // Start on the first row that can be selected.
        for (size_t i = 0; i < items_.size(); ++i)
            if (items_[i].selectable()) { f.selected = static_cast<int>(i); break; }
    }
    render();
}

void Menu::pop() {
    if (editing_) { show_editor(false); return; }
    if (stack_.empty()) return;
    stack_.pop_back();
    if (stack_.empty()) { lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN); return; }
    refresh();
}

void Menu::close() {
    show_editor(false);
    stack_.clear();
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void Menu::refresh() {
    if (stack_.empty()) return;
    build_items();
    settle();
    render();
}

void Menu::settle() {
    Frame &f = stack_.back();
    if (f.page.reading) { f.selected = -1; return; }
    if (f.selected >= static_cast<int>(items_.size())) f.selected = static_cast<int>(items_.size()) - 1;
    if (f.selected < 0) f.selected = 0;
    while (f.selected < static_cast<int>(items_.size()) - 1 && !items_[static_cast<size_t>(f.selected)].selectable())
        ++f.selected;
}

std::string Menu::title() const { return stack_.empty() ? std::string() : stack_.back().page.title; }

int Menu::first_shown() const { return stack_.empty() ? 0 : stack_.back().scroll; }

std::string Menu::selected() const {
    if (stack_.empty()) return "";
    const int at = stack_.back().selected;
    return at >= 0 && at < static_cast<int>(items_.size()) ? items_[static_cast<size_t>(at)].label : std::string();
}

void Menu::render() {
    if (stack_.empty()) return;
    Frame &f = stack_.back();
    lv_label_set_text(title_, f.page.title.c_str());
    const int n = static_cast<int>(items_.size());
    const bool reading = f.page.reading;
    const int row_h = reading ? kReadRowH : kRowH;
    const int visible = reading ? kReadVisible : kVisible;
    if (reading) {
        f.scroll = std::max(0, std::min(f.scroll, n - visible));
    } else {
        if (f.selected < f.scroll) f.scroll = f.selected;
        if (f.selected >= f.scroll + kVisible) f.scroll = f.selected - kVisible + 1;
        if (f.scroll > 0 && !items_[static_cast<size_t>(f.scroll - 1)].selectable() && f.selected == f.scroll)
            --f.scroll;   // keep a section title visible above its first row
        if (f.selected < kVisible) f.scroll = 0;   // show any introduction above the first choices
        if (f.scroll < 0) f.scroll = 0;
    }

    lv_obj_clean(list_);
    live_.clear();
    for (int row = 0; row < visible && f.scroll + row < n; ++row) {
        const MenuItem &it = items_[static_cast<size_t>(f.scroll + row)];
        const bool selected = f.scroll + row == f.selected;
        lv_obj_t *line = box(list_, kScreenW, row_h, selected ? kPanelHi : kBg);
        lv_obj_set_pos(line, 0, row * row_h);
        if (it.kind == MenuItem::Kind::Pair) {
            lv_obj_t *k = label(line, it.label.c_str(), bold(), kAccent);
            lv_label_set_long_mode(k, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_width(k, kPairKeyW - 4);
            lv_obj_align(k, LV_ALIGN_LEFT_MID, 10, 0);
            const std::string what = it.value ? it.value() : "";
            lv_obj_t *w = label(line, what.c_str(), body(), kText);
            lv_label_set_long_mode(w, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_width(w, pair_text_width());
            lv_obj_align(w, LV_ALIGN_LEFT_MID, kPairX, 0);
            continue;
        }
        if (it.kind == MenuItem::Kind::Text) {
            lv_obj_t *l = label(line, it.label.c_str(), body(), kDim);
            lv_obj_align(l, LV_ALIGN_LEFT_MID, 8, 0);
            continue;
        }
        if (it.kind == MenuItem::Kind::Header) {
            lv_obj_t *l = label(line, it.label.c_str(), small(), kAccent);
            lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 6, -2);
            continue;
        }
        if (selected) {
            lv_obj_t *mark = box(line, 3, kRowH, kAccent);
            lv_obj_set_pos(mark, 0, 0);
        }
        lv_obj_t *l = label(line, it.label.c_str(), body(), it.danger ? kListen : kText);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);
        std::string value = it.value ? it.value() : "";
        if (!value.empty()) {
            lv_obj_t *v = label(line, value.c_str(), body(), selected ? kText : kDim);
            // Long values (addresses, model names) get more room and are cut with an ellipsis
            // rather than wrapping onto a second line.
            const int value_w = value.size() > 22 ? 190 : 150;
            lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_size(v, value_w, lv_font_get_line_height(body()));
            lv_obj_align(v, LV_ALIGN_RIGHT_MID, -8, 0);
            lv_obj_set_size(l, kScreenW - 24 - value_w, lv_font_get_line_height(body()));
            if (f.page.live) live_.push_back({static_cast<size_t>(f.scroll + row), v, value});
        } else {
            lv_obj_set_width(l, kScreenW - 20);
        }
    }
    // Scroll position marker
    if (n > visible) {
        const int track = kListH - 4;
        const int h = track * visible / n < 8 ? 8 : track * visible / n;
        lv_obj_t *thumb = box(list_, 2, h, kFaint);
        lv_obj_set_pos(thumb, kScreenW - 3, 2 + (track - h) * f.scroll / (n - visible));
    }
    std::string hint = f.page.hint;
    if (hint.empty() && reading) hint = n > visible ? "F/X scroll   ·   ESC back" : "ESC back";
    if (hint.empty()) {
        const MenuItem *cur = f.selected >= 0 && f.selected < n ? &items_[static_cast<size_t>(f.selected)] : nullptr;
        hint = cur && cur->adjust ? "Left/Right change   ·   ESC back" : "ENTER select   ·   ESC back";
    }
    lv_label_set_text(hint_, hint.c_str());
}

void Menu::show_editor(bool on) {
    editing_ = on;
    if (on) {
        lv_obj_remove_flag(editor_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(editor_);
        lv_label_set_text(hint_, "ENTER save   ·   ESC cancel");
    } else {
        lv_obj_add_flag(editor_, LV_OBJ_FLAG_HIDDEN);
        edit_done_ = nullptr;
        if (!stack_.empty()) render();
    }
}

void Menu::edit_text(const std::string &title, const std::string &initial, bool secret,
                     std::function<void(const std::string &)> on_done, const std::string &help) {
    lv_label_set_text(edit_title_, title.c_str());
    lv_label_set_text(edit_help_, help.c_str());
    lv_textarea_set_password_mode(edit_field_, false);
    lv_textarea_set_text(edit_field_, secret ? "" : initial.c_str());
    edit_done_ = std::move(on_done);
    show_editor(true);
}

void Menu::confirm(const std::string &question, const std::string &yes_label, std::function<void()> on_yes) {
    MenuPage page;
    page.title = question;
    page.build = [this, yes_label, on_yes]() {
        std::vector<MenuItem> items;
        items.push_back(MenuItem::action("Cancel", [this] { pop(); }));
        items.push_back(MenuItem::action(yes_label, [this, on_yes] { pop(); on_yes(); refresh(); }, true));
        return items;
    };
    push(std::move(page));
}

void Menu::choose(const std::string &title, std::vector<std::string> options, const std::string &current,
                  std::function<void(const std::string &)> on_pick) {
    MenuPage page;
    page.title = title;
    page.build = [this, options, current, on_pick]() {
        std::vector<MenuItem> items;
        for (auto &opt : options)
            items.push_back(MenuItem::val(opt, [opt, current] { return std::string(opt == current ? LV_SYMBOL_OK : ""); },
                                          [this, opt, on_pick] { on_pick(opt); pop(); }));
        if (items.empty()) items.push_back(MenuItem::header("Nothing to choose from"));
        return items;
    };
    push(std::move(page));
    // Put the cursor on the current choice.
    for (size_t i = 0; i < options.size(); ++i)
        if (options[i] == current) { stack_.back().selected = static_cast<int>(i); render(); break; }
}

void Menu::message(const std::string &title, const std::string &body_text) {
    MenuPage page;
    page.title = title;
    page.hint = "ESC back";
    page.build = [body_text]() {
        std::vector<MenuItem> items;
        size_t start = 0;
        while (start <= body_text.size()) {
            size_t end = body_text.find('\n', start);
            if (end == std::string::npos) end = body_text.size();
            items.push_back(MenuItem::action(body_text.substr(start, end - start), nullptr));
            start = end + 1;
        }
        return items;
    };
    push(std::move(page));
}

void Menu::problem(const std::string &title, const std::string &text) {
    toast_ticks_ = 0;
    lv_obj_add_flag(toast_, LV_OBJ_FLAG_HIDDEN);
    MenuPage page;
    page.title = title;
    page.hint = "ENTER or ESC to go back";
    page.build = [this, text]() {
        std::vector<MenuItem> items;
        add_paragraph(items, text);
        items.push_back(MenuItem::action("Back", [this] { pop(); }));
        return items;
    };
    push(std::move(page));
}

void Menu::toast(const std::string &text) {
    lv_label_set_text(toast_, text.c_str());
    lv_obj_remove_flag(toast_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(toast_);
    toast_ticks_ = 28;
}

void Menu::tick() {
    if (toast_ticks_ > 0 && --toast_ticks_ == 0) lv_obj_add_flag(toast_, LV_OBJ_FLAG_HIDDEN);
    if (stack_.empty() || editing_ || !stack_.back().page.live) return;
    Frame &f = stack_.back();
    if (f.page.revision && f.page.revision() != f.revision) {
        // Rows have come or gone by themselves (a timer ended). The cursor stays on the row it was
        // on, wherever that has moved to, rather than on whatever has taken its place.
        const std::string was = selected();
        const int at = f.selected;
        build_items();
        int best = -1;
        for (int i = 0; i < static_cast<int>(items_.size()); ++i)
            if (items_[static_cast<size_t>(i)].selectable() && items_[static_cast<size_t>(i)].label == was &&
                (best < 0 || std::abs(i - at) < std::abs(best - at)))
                best = i;
        if (best >= 0) f.selected = best;
        settle();
        render();
        return;
    }
    for (LiveValue &shown : live_) {
        const MenuItem &it = items_[shown.item];
        const std::string text = it.value ? it.value() : "";
        if (text == shown.text) continue;
        shown.text = text;
        lv_label_set_text(shown.label, text.c_str());
    }
}

void Menu::key(const KeyEvent &ev) {
    if (ev.state == 0 || stack_.empty()) return;
    if (editing_) {
        switch (ev.key) {
            case Key::Enter: {
                auto done = edit_done_;
                const std::string text = lv_textarea_get_text(edit_field_);
                show_editor(false);
                if (done) done(text);
                refresh();
                break;
            }
            case Key::Esc: if (ev.state == 1) show_editor(false); break;
            case Key::Char: lv_textarea_add_text(edit_field_, ev.text.c_str()); break;
            case Key::Backspace: lv_textarea_delete_char(edit_field_); break;
            case Key::Delete: lv_textarea_delete_char_forward(edit_field_); break;
            case Key::Left: lv_textarea_cursor_left(edit_field_); break;
            case Key::Right: lv_textarea_cursor_right(edit_field_); break;
            case Key::Home: lv_textarea_set_cursor_pos(edit_field_, 0); break;
            case Key::End: lv_textarea_set_cursor_pos(edit_field_, LV_TEXTAREA_CURSOR_LAST); break;
            default: break;
        }
        return;
    }
    Frame &f = stack_.back();
    const int n = static_cast<int>(items_.size());
    auto move = [&](int dir) {
        int i = f.selected;
        for (int step = 0; step < n; ++step) {
            i += dir;
            if (i < 0 || i >= n) return;
            if (items_[static_cast<size_t>(i)].selectable()) { f.selected = i; break; }
        }
        render();
    };
    Key k = ev.key;
    // With no text field open, the letter keys under the arrow legends work as arrows too.
    if (k == Key::Char && ev.text.size() == 1) {
        switch (ev.text[0]) {
            case 'f': case 'F': k = Key::Up; break;
            case 'x': case 'X': k = Key::Down; break;
            case 'z': case 'Z': k = Key::Left; break;
            case 'c': case 'C': k = Key::Right; break;
            default: break;
        }
    }
    if (f.page.reading) {
        const int before = f.scroll;
        switch (k) {
            case Key::Up: --f.scroll; break;
            case Key::Down: ++f.scroll; break;
            case Key::PgUp: f.scroll -= kReadVisible - 1; break;
            case Key::PgDn: f.scroll += kReadVisible - 1; break;
            case Key::Enter: case Key::Esc: case Key::Backspace:
                if (ev.state == 1) pop();
                return;
            default: return;
        }
        f.scroll = std::max(0, std::min(f.scroll, n - kReadVisible));
        if (f.scroll != before) render();
        return;
    }
    const MenuItem *cur = f.selected >= 0 && f.selected < n ? &items_[static_cast<size_t>(f.selected)] : nullptr;
    switch (k) {
        case Key::Up: move(-1); break;
        case Key::Down: move(1); break;
        case Key::PgUp: for (int i = 0; i < kVisible - 1; ++i) move(-1); break;
        case Key::PgDn: for (int i = 0; i < kVisible - 1; ++i) move(1); break;
        case Key::Left: case Key::Right:
            if (cur && cur->adjust) { auto fn = cur->adjust; fn(k == Key::Left ? -1 : 1); refresh(); }
            break;
        case Key::Enter:
            if (ev.state != 1) break;
            if (cur && cur->activate) { auto fn = cur->activate; fn(); if (!editing_ && !stack_.empty()) refresh(); }
            else if (cur && cur->adjust) { auto fn = cur->adjust; fn(1); refresh(); }
            break;
        case Key::Esc: case Key::Backspace:
            if (ev.state == 1) pop();
            break;
        default: break;
    }
}
