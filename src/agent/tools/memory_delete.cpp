// The memory_delete tool: a saved fact forgotten.
#include "agent/tools/common.h"

#include "agent/consent.h"
#include "agent/tools/say_checks.h"
#include "core/settings.h"
#include "core/store.h"
#include "platform/sys.h"

namespace tools {

namespace {

const char *const kWhereIds = "memory ids are listed in your instructions";

// As a memory is saved (memory_save.cpp), it is forgotten only at the user's own word while text from outside
// is still in view: a web page or a photo may have asked for it (agent/consent.h). The same call forgets the
// same memory, by id and words. "" when it may go now.
std::string wait_for_yes(int id, const std::string &saved) {
    const std::string text = clean_text(saved);
    consent::Question q;
    q.signature = "memory_delete\n" + std::to_string(id) + "\n" + util::lower(text);
    q.reason = consent::outside_reason("a memory is forgotten only at the user's own word, read back to them in full");
    q.question = "Shall I forget this: " + consent::read_back_text(text) + "?";
    q.action = "forget";
    q.naming = consent::read_back({text}, {"forget", "delete", "remove"}, Settings::get().user_name);
    q.undone = "forgotten";
    q.again = "call memory_delete again with the same id";
    q.pool = consent::Pool::Memory;
    return consent::ask_first(q, sys::now_ms());
}

std::string t_memory_delete(const json &a, Context &ctx) {
    int id = 0;
    std::string err;
    if (!id_arg(a, "id", id, err, kWhereIds)) return fail(err);
    if (const std::string more = claims_new(a, ctx, "memory_delete only forgets; it saves nothing new",
                                            "To change a memory, use memory_save with replace_id; or send say naming "
                                            "only what was forgotten");
        !more.empty())
        return fail(more);
    std::string text;
    for (const auto &m : Store::get().memories())
        if (m.id == id) text = m.text;
    const std::string none = "no memory has id " + std::to_string(id) + "; " + kWhereIds;
    if (text.empty()) return fail(none);
    if (const std::string ask = wait_for_yes(id, text); !ask.empty()) return ask;
    return Store::get().delete_memory(id) ? "deleted" : fail(none);
}

}  // namespace

Tool make_memory_delete() {
    return {"memory_delete", "Forgetting", "Delete a saved memory by id when the user asks you to forget it or it is wrong.",
            obj({{"id", N("Memory id as listed in your instructions")}}, {"id"}), t_memory_delete, takes_say()};
}

}  // namespace tools
