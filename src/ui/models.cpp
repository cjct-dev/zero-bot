#include "ui/models.h"

#include <utility>

#include "ui/settings_internal.h"

namespace ui::models {

List list(std::string &err) { return llm::list_models(err); }

void fetch(std::function<void(const List &list, const std::string &err)> done) {
    background(
        [] {
            std::string err;
            auto found = list(err);
            return std::make_pair(found, err);
        },
        [done](const std::pair<List, std::string> &r) { done(r.first, r.second); });
}

}  // namespace ui::models
