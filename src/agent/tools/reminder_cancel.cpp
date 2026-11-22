// The reminder_cancel tool: a pending reminder cancelled.
#include "agent/tools/common.h"

#include "agent/tools/say_checks.h"
#include "core/store.h"

namespace tools {

namespace {

std::string t_reminder_cancel(const json &a, Context &ctx) {
    int id = 0;
    std::string err;
    if (!id_arg(a, "id", id, err, "use reminder_list to find it")) return fail(err);
    if (const std::string more = claims_new(a, ctx, "reminder_cancel only cancels; it sets no new time",
                                            "To move the reminder, send reminder_set for the new time and this cancel "
                                            "together, in one reply; or send say naming only the cancellation");
        !more.empty())
        return fail(more);
    return Store::get().delete_reminder(id) ? "cancelled"
                                            : fail("no reminder has id " + std::to_string(id) + "; use reminder_list to find it (timers are cancelled with timer)");
}

}  // namespace

Tool make_reminder_cancel() {
    return {"reminder_cancel", "Cancelling a reminder", "Cancel a pending reminder by id (from reminder_list). Timers are "
            "cancelled with timer.", obj({{"id", N("Reminder id")}}, {"id"}), t_reminder_cancel, takes_say()};
}

}  // namespace tools
