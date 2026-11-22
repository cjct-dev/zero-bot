// The memory_delete tool: a saved fact forgotten.
#include "agent/tools/common.h"

#include "agent/tools/say_checks.h"
#include "core/store.h"

namespace tools {

namespace {

std::string t_memory_delete(const json &a, Context &ctx) {
    int id = 0;
    std::string err;
    if (!id_arg(a, "id", id, err, "memory ids are listed in your instructions")) return fail(err);
    if (const std::string more = claims_new(a, ctx, "memory_delete only forgets; it saves nothing new",
                                            "To change a memory, use memory_save with replace_id; or send say naming "
                                            "only what was forgotten");
        !more.empty())
        return fail(more);
    return Store::get().delete_memory(id) ? "deleted" : fail("no memory has id " + std::to_string(id) + "; memory ids are listed in your instructions");
}

}  // namespace

Tool make_memory_delete() {
    return {"memory_delete", "Forgetting", "Delete a saved memory by id when the user asks you to forget it or it is wrong.",
            obj({{"id", N("Memory id as listed in your instructions")}}, {"id"}), t_memory_delete};
}

}  // namespace tools
