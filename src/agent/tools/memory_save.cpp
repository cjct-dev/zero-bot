// The memory_save tool: a lasting fact about the user, kept for every later conversation.
#include "agent/tools/common.h"

#include "core/store.h"

namespace tools {

namespace {

std::string t_memory_save(const json &a, Context &) {
    const std::string text = clean_text(str_arg(a, "text"));
    if (text.empty()) return fail("text is required: the fact to remember");
    if (text.size() > 400) return fail("keep each memory under 400 characters");
    if (given(a, "replace_id")) {
        int id = 0;
        std::string err;
        if (!id_arg(a, "replace_id", id, err, "memory ids are listed in your instructions")) return fail(err);
        if (!Store::get().update_memory(id, text)) return fail("no memory has id " + std::to_string(id) + "; memory ids are listed in your instructions");
        return "updated memory " + std::to_string(id);
    }
    // The same fact twice would only fill the instructions; say where it already is.
    for (const auto &m : Store::get().memories())
        if (util::lower(m.text) == util::lower(text)) return "already saved as memory " + std::to_string(m.id);
    return "saved as memory " + std::to_string(Store::get().add_memory(text));
}

}  // namespace

Tool make_memory_save() {
    return {"memory_save", "Remembering",
            "Save a lasting fact about the user so it is available in every future conversation: preferences, "
            "diet, allergies, names, travel plans, anything they ask you to remember. Calling this is the only way "
            "something is remembered; nothing is kept otherwise. One fact per call, written so it makes sense on its "
            "own. Use replace_id to correct an existing memory instead of adding a duplicate.",
            obj({{"text", S("The fact, e.g. \"Allergic to shellfish\"")},
                 {"replace_id", N("Optional id of an existing memory to overwrite")}}, {"text"}), t_memory_save, takes_say()};
}

}  // namespace tools
