#pragma once
#include <string>

namespace ui {
// Runs the on-screen app. If shots_dir is non-empty, renders sample screens to PNGs and exits.
int run_app(const std::string &shots_dir, bool keydump);
}
