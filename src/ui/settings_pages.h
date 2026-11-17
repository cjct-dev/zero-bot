#pragma once
#include <functional>

#include "ui/menu.h"

namespace ui {

struct SettingsHooks {
    std::function<void()> new_conversation;
    std::function<void()> exit_app;
    std::function<void()> run_setup;
};

void open_settings(Menu &menu, const SettingsHooks &hooks);

// The first-run guide: connection, AI service, models, a few personal details. on_finish runs
// when the user completes or skips it.
void open_setup(Menu &menu, std::function<void()> on_finish);

}  // namespace ui
