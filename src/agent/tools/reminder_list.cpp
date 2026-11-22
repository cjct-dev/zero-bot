// The reminder_list tool: the pending reminders.
#include "agent/tools/common.h"

#include "core/store.h"

namespace tools {

namespace {

std::string t_reminder_list(const json &, Context &) {
    json arr = json::array();
    for (auto &r : Store::get().reminders())
        arr.push_back({{"id", r.id}, {"due", util::format_time(r.due, "%Y-%m-%d %H:%M")}, {"text", r.text}});
    return json{{"reminders", arr}}.dump(-1, ' ', false, json::error_handler_t::replace);
}

}  // namespace

Tool make_reminder_list() {
    return {"reminder_list", "Checking reminders", "List pending reminders with their ids (timers: timer with action list).",
            obj({}), t_reminder_list};
}

}  // namespace tools
