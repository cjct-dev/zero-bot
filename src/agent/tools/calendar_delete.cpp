// The calendar_delete tool: an event deleted.
#include "agent/tools/common.h"

#include "agent/tools/say_checks.h"
#include "core/store.h"

namespace tools {

namespace {

std::string t_calendar_delete(const json &a, Context &ctx) {
    int id = 0;
    std::string err;
    if (!id_arg(a, "id", id, err, "use calendar_list to find it")) return fail(err);
    if (const std::string more = claims_new(a, ctx, "calendar_delete only deletes; it puts nothing at a new time",
                                            "To move the event, change it with calendar_update (the event is kept); "
                                            "or send say naming only the deletion");
        !more.empty())
        return fail(more);
    return Store::get().delete_event(id) ? "deleted" : fail("no event has id " + std::to_string(id) + "; use calendar_list to find it");
}

}  // namespace

Tool make_calendar_delete() {
    return {"calendar_delete", "Updating calendar", "Delete a calendar event by id (from calendar_list). A repeating event goes as a whole.",
            obj({{"id", N("Event id")}}, {"id"}), t_calendar_delete};
}

}  // namespace tools
