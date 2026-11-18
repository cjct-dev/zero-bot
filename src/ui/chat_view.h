// The main screen: status bar, conversation and an activity footer.
#pragma once
#include <deque>
#include <string>

#include "lvgl.h"
#include "platform/keys.h"

class ChatView {
public:
    void create(lv_obj_t *parent);
    void show(bool visible);

    // Conversation
    lv_obj_t *add_user(const std::string &text, bool steering);   // returns the bubble
    void begin_answer();
    void append_answer(const std::string &delta);
    void end_answer(const std::string &full, bool interrupted);
    void discard_answer();
    void add_tool(const std::string &label);
    void add_note(const std::string &text, uint32_t color);
    void add_alert(const std::string &text);
    // With `owned`, the file is the row's own (a photo's thumbnail): it is deleted with the row.
    void add_image(const std::string &image_path, bool owned = false);
    void clear();
    void scroll(int dy);
    bool empty() const { return count_ == 0; }

    // Status bar. Each redraws only what has changed, so they can be called on every tick.
    void set_status(const std::string &name, uint32_t dot_color);
    void set_indicators(bool online, bool via_phone, int battery_percent, bool charging, const std::string &clock);
    // A track's title too long for its room scrolls. `still` (a radio station's name, shown for as
    // long as the station plays: hours) is cut short with dots instead, so the bar is not redrawn
    // many times a second for all that time.
    void set_now_playing(const std::string &text, bool still = false);
    void set_timer(const std::string &countdown);   // "4:59 +1" beside a stopwatch; "" hides it
    std::string status_name() const;                 // the name the status bar shows (for --shots checks)
    bool now_playing_moves() const;                  // whether the now-playing text is scrolling (for --shots checks)

    // Footer
    void footer_hints(const std::string &text);
    void footer_busy(const std::string &label, uint32_t color);
    void footer_listening(float level, double seconds, bool has_audio);
    void typing_begin(const std::string &initial);
    void typing_end();
    bool typing() const { return mode_ == Mode::Typing; }
    void typing_key(const KeyEvent &ev);     // editing keys only; Enter/Esc are handled by the app
    std::string typing_text() const;

    void tick();                             // call ~15 times a second

private:
    enum class Mode { Hints, Busy, Listening, Typing };
    void set_mode(Mode m);
    lv_obj_t *add_row(bool right);
    void after_add();
    void prune();
    void place_playing(int give_way);

    lv_obj_t *root_ = nullptr, *list_ = nullptr, *footer_ = nullptr;
    lv_obj_t *dot_ = nullptr, *name_ = nullptr, *playing_ = nullptr, *net_ = nullptr, *right_ = nullptr;
    lv_obj_t *timer_ = nullptr, *timer_label_ = nullptr;
    std::string status_name_ = "Zero", indicators_, timer_text_;   // what each part of the status bar shows
    uint32_t status_color_ = 0xFFFFFFFF;     // no colour: the first call sets it
    uint32_t busy_color_ = 0xFFFFFFFF;       // the busy dot's colour; none until the footer turns busy
    int timer_w_ = 0, give_way_ = 0;          // the countdown's width, taken from the now-playing text's room
    bool playing_still_ = false;             // the now-playing text never scrolls (a station's name)
    lv_obj_t *foot_label_ = nullptr, *foot_dot_ = nullptr, *wave_ = nullptr, *input_ = nullptr, *prompt_ = nullptr;
    lv_obj_t *answer_ = nullptr;             // label currently being streamed into
    std::string answer_text_, busy_label_;
    bool answer_dirty_ = false, stick_bottom_ = true;
    Mode mode_ = Mode::Hints;
    int count_ = 0, anim_ = 0;
    std::deque<float> levels_;
    std::string hints_, last_playing_;
};
