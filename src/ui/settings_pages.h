#pragma once
#include <functional>

#include "ui/menu.h"

namespace ui {

struct SettingsHooks {
    std::function<void()> new_conversation;
    std::function<void()> exit_app;
};

void open_settings(Menu &menu, const SettingsHooks &hooks);

}  // namespace ui
