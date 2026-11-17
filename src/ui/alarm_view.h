// The banner shown while a timer's alarm rings: which timer has ended, in large type, and how to
// stop it. It lies over whatever is on screen: the chat, a menu or the breathing exercise.
#pragma once
#include <string>

#include "lvgl.h"

class AlarmView {
public:
    void create(lv_obj_t *parent);     // create it after the other views, so that it starts in front
    // Shows the banner, or changes what it says; only what has changed is redrawn.
    void show(const std::string &title, const std::string &labels);
    void hide();
    bool visible() const { return visible_; }
    void raise();                      // in front again, after another view has brought itself forward

private:
    lv_obj_t *root_ = nullptr, *title_ = nullptr, *labels_ = nullptr;
    std::string shown_title_, shown_labels_;
    bool visible_ = false;
};
