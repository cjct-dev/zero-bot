// The memory_save tool: a lasting fact about the user, kept for every later conversation.
#include "agent/tools/common.h"

#include <algorithm>

#include "agent/consent.h"
#include "agent/tools/sensitive.h"
#include "core/settings.h"
#include "core/store.h"
#include "platform/sys.h"

namespace tools {

namespace {

// Every memory is in the instructions of every request, sent with each call (on a phone's tether too), so
// one is kept short and all of them together are capped. Counted as the bytes sent.
constexpr size_t kMaxMemory = 400;
constexpr size_t kMaxMemories = 3000;

const char *const kWhereIds = "memory ids are listed in your instructions";

// Why the memories cannot take `text` in place of memory `replace_id` (0: as a new one), or "". A save that
// would take them past the cap waits; memories already past it stay, and a save that shortens them is fine.
std::string too_full(const std::vector<Memory> &memories, const std::string &text, int replace_id) {
    size_t used = 0, replaced = 0;
    for (const auto &m : memories) {
        used += m.text.size();
        if (m.id == replace_id) replaced = m.text.size();
    }
    const size_t after = used - replaced + text.size();
    if (after <= kMaxMemories || after <= used) return "";
    return "memory is full: " + std::to_string(used) + " of its " + std::to_string(kMaxMemories) + " characters are used, "
           "and this would make " + std::to_string(after) + ". Merge it into a saved memory on the same subject with "
           "replace_id, or ask the user which memory to forget and delete that one first";
}

// A memory steers every later request, so while text from outside is still in view (a web page or a photo may
// have asked for it) it is saved only at the user's own word, read back to them first (agent/consent.h).
// "" when it may be saved now.
std::string wait_for_yes(const std::string &text, int replace_id) {
    return consent::ask_first({"memory_save\n" + std::to_string(replace_id) + "\n" + util::plain(text),
                               consent::outside_reason("a memory is saved only at the user's own word, read back to them in full"),
                               consent::read_back_question("Shall I remember this: ", text), "save",
                               consent::read_back(text, {"remember", "save", "keep", "note"}, Settings::get().user_name), "saved",
                               "call memory_save again with the same text"},
                              sys::now_ms());
}

std::string t_memory_save(const json &a, Context &) {
    const std::string text = clean_text(str_arg(a, "text"));
    if (text.empty()) return fail("text is required: the fact to remember");
    if (text.size() > kMaxMemory) return fail("keep each memory under " + std::to_string(kMaxMemory) + " characters");
    if (const std::string kind = sensitive_number(text); !kind.empty())
        return fail("the text holds " + kind + ", and such numbers are never kept as memories (every memory is sent with "
                    "each request). Save the fact without the number, or tell the user it cannot be kept");
    const std::vector<Memory> memories = Store::get().memories();
    int id = 0;
    if (given(a, "replace_id")) {
        std::string err;
        if (!id_arg(a, "replace_id", id, err, kWhereIds)) return fail(err);
        if (std::none_of(memories.begin(), memories.end(), [&](const Memory &m) { return m.id == id; }))
            return fail("no memory has id " + std::to_string(id) + "; " + kWhereIds);
    } else {
        // The same fact twice would only fill the instructions; say where it already is.
        for (const auto &m : memories)
            if (util::lower(m.text) == util::lower(text)) return "already saved as memory " + std::to_string(m.id);
    }
    if (const std::string full = too_full(memories, text, id); !full.empty()) return fail(full);
    if (const std::string ask = wait_for_yes(text, id); !ask.empty()) return ask;
    if (!id) return "saved as memory " + std::to_string(Store::get().add_memory(text));
    if (!Store::get().update_memory(id, text)) return fail("no memory has id " + std::to_string(id) + "; " + kWhereIds);
    return "updated memory " + std::to_string(id);
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
