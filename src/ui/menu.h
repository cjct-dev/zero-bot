// A small stack of list menus with inline editing, used for settings and data management.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "lvgl.h"
#include "platform/keys.h"

struct MenuItem {
    enum class Kind { Header, Action, Value };
    Kind kind = Kind::Action;
    std::string label;
    std::function<std::string()> value;        // text shown on the right (optional)
    std::function<void()> activate;            // Enter
    std::function<void(int)> adjust;           // Left (-1) / Right (+1), for sliders and toggles
    bool danger = false;                       // drawn in red

    static MenuItem header(const std::string &text);
    static MenuItem action(const std::string &text, std::function<void()> fn, bool danger = false);
    static MenuItem val(const std::string &text, std::function<std::string()> value, std::function<void()> fn,
                        std::function<void(int)> adjust = nullptr);
};

struct MenuPage {
    std::string title;
    std::function<std::vector<MenuItem>()> build;   // re-run whenever the page is shown or refreshed
    std::string hint;                               // overrides the default footer hint
};

class Menu {
public:
    void create(lv_obj_t *parent);
    bool open() const { return !stack_.empty(); }
    void push(MenuPage page);
    void pop();
    void close();
    void refresh();                 // rebuild the current page, keeping the selection
    void toast(const std::string &text);
    void key(const KeyEvent &ev);   // returns to the app automatically when the last page closes
    void tick();

    // Asks for a line of text. on_done receives the result when the user confirms.
    void edit_text(const std::string &title, const std::string &initial, bool secret,
                   std::function<void(const std::string &)> on_done, const std::string &help = "");
    void confirm(const std::string &question, const std::string &yes_label, std::function<void()> on_yes);
    void choose(const std::string &title, std::vector<std::string> options, const std::string &current,
                std::function<void(const std::string &)> on_pick);
    void message(const std::string &title, const std::string &body);   // a page of plain text

private:
    void render();
    void show_editor(bool on);
    struct Frame { MenuPage page; int selected = 0; int scroll = 0; };
    std::vector<Frame> stack_;
    std::vector<MenuItem> items_;
    lv_obj_t *root_ = nullptr, *title_ = nullptr, *list_ = nullptr, *hint_ = nullptr, *toast_ = nullptr;
    lv_obj_t *editor_ = nullptr, *edit_title_ = nullptr, *edit_field_ = nullptr, *edit_help_ = nullptr;
    bool editing_ = false;
    std::function<void(const std::string &)> edit_done_;
    int toast_ticks_ = 0;
};
