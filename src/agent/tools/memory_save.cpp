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
// one is kept short and all of them together are capped. Counted as the bytes sent. 2,000 keeps the first request
// with every tool offered and a full memory under the loop test's tripwire (3,000 went 465 bytes past it).
constexpr size_t kMaxMemory = 400;
constexpr size_t kMaxMemories = 2000;

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
    return "memory is full: " + std::to_string(used) + " of its " + std::to_string(kMaxMemories) + " bytes are used, "
           "and this would make " + std::to_string(after) + ". Merge it into a saved memory on the same subject with "
           "replace_id, or ask the user which memory to forget and delete that one first";
}

// A memory steers every later request, so while text from outside is still in view (a web page or a photo may
// have asked for it) it is saved only at the user's own word, read back to them first (agent/consent.h); a
// replacement together with the memory it replaces, which would otherwise be lost without a word. The same call
// is the same text and the same memory replaced. "" when it may be saved now.
std::string wait_for_yes(const std::string &text, const Memory *old) {
    const std::string was = old ? clean_text(old->text) : "";
    consent::Question q;
    q.signature = "memory_save\n" + (old ? std::to_string(old->id) + "\n" + util::lower(was) : "0") + "\n" + util::lower(text);
    q.reason = consent::outside_reason(old ? "a memory is replaced only at the user's own word, with both read back to them in full"
                                           : "a memory is saved only at the user's own word, read back to them in full");
    q.question = old ? "Shall I replace '" + consent::read_back_text(was) + "' with '" + consent::read_back_text(text) + "'?"
                     : "Shall I remember this: " + consent::read_back_text(text) + "?";
    q.action = old ? "replace" : "save";
    q.naming = consent::read_back(old ? std::vector<std::string>{was, text} : std::vector<std::string>{text},
                                  {"remember", "save", "keep", "note", "replace", "change", "update", "swap"}, Settings::get().user_name);
    q.undone = old ? "replaced" : "saved";
    q.again = old ? "call memory_save again with the same text and replace_id" : "call memory_save again with the same text";
    q.pool = consent::Pool::Memory;
    return consent::ask_first(q, sys::now_ms());
}

std::string t_memory_save(const json &a, Context &) {
    const std::string text = clean_text(str_arg(a, "text"));
    if (text.empty()) return fail("text is required: the fact to remember");
    if (text.size() > kMaxMemory)
        return fail("keep each memory under " + std::to_string(kMaxMemory) + " bytes (about as many letters of English, fewer in "
                    "scripts that take more bytes)");
    if (const std::string kind = sensitive_number(text); !kind.empty())
        return fail("the text holds " + kind + ", and such numbers are never kept as memories (every memory is sent with "
                    "each request). Save the fact without the number, or tell the user it cannot be kept");
    const std::vector<Memory> memories = Store::get().memories();
    int id = 0;
    const Memory *old = nullptr;
    if (given(a, "replace_id")) {
        std::string err;
        if (!id_arg(a, "replace_id", id, err, kWhereIds)) return fail(err);
        const auto found = std::find_if(memories.begin(), memories.end(), [&](const Memory &m) { return m.id == id; });
        if (found == memories.end()) return fail("no memory has id " + std::to_string(id) + "; " + kWhereIds);
        old = &*found;
    } else {
        // The same fact twice would only fill the instructions; say where it already is (one saved before
        // its hidden characters were taken out too).
        for (const auto &m : memories)
            if (util::lower(clean_text(m.text)) == util::lower(text)) return "already saved as memory " + std::to_string(m.id);
    }
    if (const std::string full = too_full(memories, text, id); !full.empty()) return fail(full);
    if (const std::string ask = wait_for_yes(text, old); !ask.empty()) return ask;
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
