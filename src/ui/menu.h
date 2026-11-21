// A small stack of list menus with inline editing, used for settings and data management.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "lvgl.h"
#include "platform/keys.h"

struct MenuItem {
    // Header, Text and Pair rows cannot be selected. A Pair is two columns: a key (in `label`) and
    // what it does (in `value`), for the key guide.
    enum class Kind { Header, Text, Action, Value, Pair };
    Kind kind = Kind::Action;
    std::string label;
    std::function<std::string()> value;        // text shown on the right (optional)
    std::function<void()> activate;            // Enter
    std::function<void(int)> adjust;           // Left (-1) / Right (+1), for sliders and toggles
    bool danger = false;                       // drawn in red

    static MenuItem header(const std::string &text);
    static MenuItem text(const std::string &text);       // one line of explanatory text
    bool selectable() const { return kind == Kind::Action || kind == Kind::Value; }
    static MenuItem action(const std::string &text, std::function<void()> fn, bool danger = false);
    static MenuItem val(const std::string &text, std::function<std::string()> value, std::function<void()> fn,
                        std::function<void(int)> adjust = nullptr);
    static MenuItem pair(const std::string &key, const std::string &what);
};

struct MenuPage {
    std::string title;
    std::function<std::vector<MenuItem>()> build;   // re-run whenever the page is shown or refreshed
    std::string hint;                               // overrides the default footer hint
    // The values change by themselves (a timer's time left): tick() reads the ones on screen
    // again and redraws those whose text has changed. Their value functions must be cheap.
    bool live = false;
    // Optional, cheap: a number that changes when the page's rows do (a timer ended or was
    // cancelled elsewhere). tick() rebuilds the page when it changes.
    std::function<int()> revision;
    // A page to read rather than choose from (the key guide): no cursor, closer rows, up and down
    // scroll it, ENTER or ESC goes back. `top` is the row shown first.
    bool reading = false;
    int top = 0;
    // A page of the first-run setup guide. Outside a text field SPACE closes settings, but not the
    // guide or a page opened from it: a new user takes SPACE for "select", and left the guide.
    bool guide = false;
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
    void problem(const std::string &title, const std::string &text);   // wrapped text that stays until ESC
    size_t depth() const { return stack_.size(); }
    bool editing() const { return editing_; }   // a text field is open: every key types
    bool in_guide() const;          // a page of the setup guide is open, on top or under the one shown
    std::string title() const;      // the top page's title, and the label of the row the cursor
    std::string selected() const;   // is on ("" with no page): for the checks in --shots
    int first_shown() const;        // the row at the top of the screen (for --shots)
    // For the walks in --shots: where the cursor is on the page on top, and the first row there with
    // this label that the cursor can stop on (-1 when there is none). Rows count as in the page's build.
    int cursor() const;
    int row_of(const std::string &label) const;
    // The width a Pair row gives its key (the rest is for what it does): for the checks in --shots.
    static constexpr int kPairKeyW = 96;   // DIAGNOSTICS (temporary): 84 without the guide's "SPACE SPACE" line
    static int pair_text_width();
    // Splits a paragraph into Text rows that fit the screen width.
    static void add_paragraph(std::vector<MenuItem> &items, const std::string &paragraph);
    static constexpr size_t kParagraphChars = 47;   // characters of body text that fit across the screen
    static constexpr int kToastTicks = 28;          // ticks a toast stays: about 2 s

private:
    void render();
    void show_editor(bool on);
    struct Frame { MenuPage page; int selected = 0; int scroll = 0; int revision = 0; };
    void build_items();             // runs the top page's build, noting its revision
    void settle();                  // after a rebuild: the cursor on a row that exists and can be selected
    std::vector<Frame> stack_;
    std::vector<MenuItem> items_;
    lv_obj_t *root_ = nullptr, *title_ = nullptr, *list_ = nullptr, *hint_ = nullptr, *toast_ = nullptr;
    lv_obj_t *editor_ = nullptr, *edit_title_ = nullptr, *edit_field_ = nullptr, *edit_help_ = nullptr;
    bool editing_ = false;
    std::function<void(const std::string &)> edit_done_;
    int toast_ticks_ = 0;
    // On a live page: the rows on screen with a value, and the text each shows.
    struct LiveValue { size_t item; lv_obj_t *label; std::string text; };
    std::vector<LiveValue> live_;
};
