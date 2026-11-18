// The keys: what each one does on the chat screen, and the guide that lists them all (H, or
// Settings → Keys). No screen code here, so it is tested without one (tests/unit/key_guide_test.cpp).
#pragma once
#include <string>
#include <vector>

#include "platform/keys.h"

namespace keyguide {

// One line of the guide: the keys as printed on the keyboard, and what they do.
struct Row {
    std::string keys, action;
};
struct Section {
    std::string title;
    std::vector<Row> rows;
};

// Every key action, in the order the guide shows them. The last section is the keys in settings.
const std::vector<Section> &sections();

// What a key does on the chat screen when nothing is being typed or recorded. TAB, ESC and the
// fn layer's control keys are handled before this is asked; a release does nothing here.
enum class ChatKey { None, Settings, Guide, ScrollUp, ScrollDown, PageUp, PageDown, Type, DeleteTap, TypeHint };
ChatKey chat_key(const KeyEvent &ev);

bool is_guide_key(const KeyEvent &ev);      // H or ?, pressed (in settings too, outside a text field)
bool is_settings_key(const KeyEvent &ev);   // SPACE, pressed: opens settings, and closes them again

// The bottom row of the chat screen while nothing is happening: the keys most wanted, and the one
// that shows the rest.
std::string idle_hint(bool configured);

}  // namespace keyguide
