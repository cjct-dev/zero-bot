#pragma once
#include <functional>

#include "ui/menu.h"

namespace ui {

struct SettingsHooks {
    std::function<void()> new_conversation;
    std::function<void()> exit_app;
    std::function<void()> run_setup;
};

// A short top page of sections (Assistant, Device, Your data...), each opening a page of its own.
void open_settings(Menu &menu, const SettingsHooks &hooks);

// Every key and what it does, as a page to read. From settings it opens at the keys for settings.
// Nothing happens when the guide is already the page on top.
void open_key_guide(Menu &menu, bool from_settings);
inline constexpr const char *kKeyGuideTitle = "Keys";

// The first-run guide: connection, AI service, models, a few personal details. on_finish runs
// when the user completes or skips it.
void open_setup(Menu &menu, std::function<void()> on_finish);

}  // namespace ui
