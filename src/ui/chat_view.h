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
    void add_user(const std::string &text, bool steering);
    void begin_answer();
    void append_answer(const std::string &delta);
    void end_answer(const std::string &full, bool interrupted);
    void discard_answer();
    void add_tool(const std::string &label);
    void add_note(const std::string &text, uint32_t color);
    void add_alert(const std::string &text);
    void add_image(const std::string &image_path);
    void clear();
    void scroll(int dy);
    bool empty() const { return count_ == 0; }

    // Status bar
    void set_status(const std::string &name, uint32_t dot_color);
    void set_indicators(bool online, int battery_percent, bool charging, const std::string &clock);
    void set_now_playing(const std::string &text);

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

    lv_obj_t *root_ = nullptr, *list_ = nullptr, *footer_ = nullptr;
    lv_obj_t *dot_ = nullptr, *name_ = nullptr, *playing_ = nullptr, *net_ = nullptr, *right_ = nullptr;
    lv_obj_t *foot_label_ = nullptr, *foot_dot_ = nullptr, *wave_ = nullptr, *input_ = nullptr, *prompt_ = nullptr;
    lv_obj_t *answer_ = nullptr;             // label currently being streamed into
    std::string answer_text_, busy_label_;
    bool answer_dirty_ = false, stick_bottom_ = true;
    Mode mode_ = Mode::Hints;
    int count_ = 0, anim_ = 0;
    std::deque<float> levels_;
    std::string hints_, last_playing_;
};
