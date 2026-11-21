// The AI service's list of models, for the four places that read it: the app's own first choice
// (App::auto_select_models), the model pages in settings (pick_model), the setup guide's test of the
// service and the status page. The two rules for choosing from it differ, and stay two.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "agent/llm.h"
#include "core/settings.h"

namespace ui::models {

using List = std::vector<llm::ModelInfo>;

// The service's models, asked for now (up to 20 s): not on the UI thread. `err` says why there are none.
List list(std::string &err);

// Asks off the UI thread, then hands `done` the list, or the service's error, on the UI thread.
void fetch(std::function<void(const List &list, const std::string &err)> done);

// What the app picks by itself so that it works before settings are touched: each model setting still
// empty gets the first model the service labels for it.
void choose_first(Settings &s, const List &list);

// What the setup guide picks once the service has answered: the same, except that a model with no
// label also counts as a language model. (Making the two rules one would change what is chosen.)
void choose_for_setup(Settings &s, const List &list);

}  // namespace ui::models
