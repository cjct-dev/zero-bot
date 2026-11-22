#include "ui/models.h"

#include <utility>

#include "ui/settings_internal.h"

namespace ui::models {

List list(const llm::Service &service, std::string &err) { return llm::list_models(service, err); }

void fetch(std::function<void(const List &list, const std::string &err)> done) {
    const llm::Service asked = llm::Service::of(Settings::get());
    background(
        [asked] {
            std::string err;
            auto found = list(asked, err);
            return std::make_pair(found, err);
        },
        [done, asked](const std::pair<List, std::string> &r) {
            if (llm::Service::of(Settings::get()) == asked) done(r.first, r.second);
        });
}

}  // namespace ui::models
