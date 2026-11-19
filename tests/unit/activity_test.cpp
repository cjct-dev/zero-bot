// Unit tests for the diagnostic module (src/core/activity.cpp; DIAGNOSTICS, temporary: ZB-052
// removes it with this test): saved conversations are written only when asked, in a 0700 folder
// as 0600 files, hold what the model saw (each user message with its time, each reply, each tool
// call with its arguments and a shortened result), never a key from the settings, stay under
// their size limits and are pruned oldest first. Everything is written under the folder given as
// the first argument. Each check prints one line on failure; the exit status is non-zero when any fails.
#include "core/activity.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "core/settings.h"

namespace {

int g_passed = 0, g_failed = 0;

void check(bool ok, const std::string &what) {
    if (ok) ++g_passed;
    else { ++g_failed; printf("FAIL %s\n", what.c_str()); }
}

bool exists(const std::string &path) { return access(path.c_str(), F_OK) == 0; }

int mode_of(const std::string &path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 ? static_cast<int>(st.st_mode & 07777) : -1;
}

json user(const std::string &stamp, const std::string &words) { return {{"role", "user"}, {"content", "[" + stamp + "] " + words}}; }
json reply(const std::string &words) { return {{"role", "assistant"}, {"content", words}}; }
json call(const std::string &id, const std::string &name, const std::string &args) {
    return {{"id", id}, {"type", "function"}, {"function", {{"name", name}, {"arguments", args}}}};
}
json asks(const std::string &said, std::vector<json> calls) {
    return {{"role", "assistant"}, {"content", said}, {"tool_calls", calls}};
}
json result(const std::string &id, const std::string &text) { return {{"role", "tool"}, {"tool_call_id", id}, {"content", text}}; }

// A conversation like the ones on the device: a search, a reply, a follow-up with two calls.
const std::string kLongResult = "{\"summary\":\"" + std::string(3000, 'x') + "\"}";
json sample() {
    return json::array({user("Wed 18 Nov 2026, 11:14", "when does the harbour company list its shares"),
                        asks("", {call("c1", "web_search", "{\"query\":\"harbour company IPO date\"}")}),
                        result("c1", kLongResult),
                        reply("It is expected to price on Thursday."),
                        user("Wed 18 Nov 2026, 11:16", "and what time is that here\n\n(Automatic note, not from the user: a timer ended.)"),
                        asks("Let me check.", {call("c2", "world_time", "{\"place\":\"New York\"}"), call("c3", "calculator", "not json at all")}),
                        result("c2", "{\"time\":\"06:16\"}"),
                        result("c3", "ERROR: expression is required"),
                        reply("Trading starts at 2:30 in the afternoon, UK time.")});
}

void test_record_shape() {
    const json r = activity::conversation_record(sample(), 0, 1795003200, {});
    check(r.value("format", 0) == 1, "the record has a format number");
    check(r.value("earlier_messages_not_kept", -1) == 0, "nothing earlier was left out");
    check(!r.value("saved_at", std::string()).empty(), "the record says when it was saved");
    const json &m = r["messages"];
    check(m.is_array() && m.size() == 6, "tool results go with their calls: 6 entries for 9 messages, got " + std::to_string(m.size()));
    if (!m.is_array() || m.size() != 6) return;
    check(m[0]["role"] == "user" && m[0]["time"] == "2026-11-18 11:14", "a user message has the time of its stamp");
    check(m[0]["content"] == "[Wed 18 Nov 2026, 11:14] when does the harbour company list its shares",
          "a user message is kept as the model saw it, stamp included");
    check(m[3]["content"].get<std::string>().find("(Automatic note") != std::string::npos, "notes the model saw are kept");
    const json &c1 = m[1]["tool_calls"][0];
    check(c1["tool"] == "web_search" && c1["arguments"]["query"] == "harbour company IPO date", "a call has its tool and arguments");
    check(c1["result"].get<std::string>().size() <= activity::kResultBytes + 8 && c1.value("result_bytes", size_t(0)) == kLongResult.size(),
          "a long result is shortened and its full size given");
    check(m[2]["role"] == "assistant" && m[2]["content"] == "It is expected to price on Thursday." && !m[2].contains("tool_calls"),
          "a reply is kept");
    const json &calls = m[4]["tool_calls"];
    check(m[4]["content"] == "Let me check." && calls.size() == 2, "words said before calls are kept with them");
    check(calls[0]["result"] == "{\"time\":\"06:16\"}" && !calls[0].contains("result_bytes"), "a short result is kept whole");
    check(calls[1]["arguments"] == "not json at all" && calls[1]["result"] == "ERROR: expression is required",
          "arguments that are not JSON are kept as text; an error result is kept");
    check(m[5]["content"] == "Trading starts at 2:30 in the afternoon, UK time.", "the last reply is kept");
}

void test_unusual_messages() {
    json msgs = json::array({result("old", "a result whose call was trimmed away"),
                             user("not a stamp", "hello"),
                             asks("", {call("c9", "timer", "{\"action\":\"list\"}")}),   // still running
                             json{{"role", "user"},
                                  {"content", json::array({json{{"type", "text"}, {"text", "what is this"}},
                                                           json{{"type", "image_url"}, {"image_url", {{"url", "data:image/jpeg;base64,AAAA"}}}}})}},
                             json("not a message")});
    const json r = activity::conversation_record(msgs, 3, 1795003200, {});
    const json &m = r["messages"];
    check(r["earlier_messages_not_kept"] == 3, "messages dropped before are counted");
    check(m.size() == 4, "four entries (the stray value is skipped), got " + std::to_string(m.size()));
    if (m.size() != 4) return;
    check(m[0]["role"] == "tool" && m[0]["result"] == "a result whose call was trimmed away", "a result without its call stands alone");
    check(!m[1].contains("time"), "no time without a stamp");
    check(m[2]["tool_calls"][0].value("pending", false), "a call saved while it ran is marked pending");
    const std::string parts = m[3]["content"];
    check(parts.find("what is this") != std::string::npos && parts.find("base64") == std::string::npos, "an image is not saved");
}

void test_secrets() {
    json msgs = sample();
    msgs[0]["content"] = "[Wed 18 Nov 2026, 11:14] my key is sk-test-1234567 and the other is tvly-ABCDEFG";
    msgs[2]["content"] = "HTTP 401: invalid key sk-test-1234567";
    const json r = activity::conversation_record(msgs, 0, 1795003200, {"sk-test-1234567", "tvly-ABCDEFG", "", "har"});
    const std::string text = r.dump();
    check(text.find("sk-test-1234567") == std::string::npos && text.find("tvly-ABCDEFG") == std::string::npos,
          "the keys are blanked out everywhere");
    check(text.find("my key is [removed] and the other is [removed]") != std::string::npos, "and the rest of the words kept");
    check(text.find("harbour") != std::string::npos, "short or empty secrets blank nothing");

    // A key found inside "[removed]" itself: replaced once, never in a loop (this hung before; the
    // alarm set in main ends the test should it come back).
    json odd = json::array({user("Wed 18 Nov 2026, 11:14", "the key removed, removed again, and [removed]")});
    const json q = activity::conversation_record(odd, 0, 1795003200, {"removed", "[removed]", "emoved"});
    const std::string said = q["messages"][0]["content"];
    check(said.find("the key [removed], [removed] again, and [removed]") != std::string::npos,
          "a key that \"[removed]\" contains is blanked once: " + said);
}

void test_file_cap() {
    json msgs = json::array();
    for (int i = 0; i < 400; ++i) {
        msgs.push_back(user("Wed 18 Nov 2026, 11:14", "question " + std::to_string(i) + " " + std::string(600, 'q')));
        msgs.push_back(reply("answer " + std::to_string(i) + " " + std::string(600, 'a')));
    }
    const json r = activity::conversation_record(msgs, 0, 1795003200, {});
    const size_t size = r.dump(1).size();
    check(size <= activity::kMaxFileBytes, "one save stays under its limit: " + std::to_string(size) + " bytes");
    check(r["earlier_messages_not_kept"].get<size_t>() > 0 && r["messages"][0]["role"] == "user",
          "the oldest go first, and what is kept begins with the user");
    check(r["messages"].back()["content"].get<std::string>().rfind("answer 399", 0) == 0, "the newest are kept");
}

void test_compact() {
    const json long_result = result("c1", std::string(5000, 'r'));
    const json kept = activity::compact(long_result);
    check(kept["content"].get<std::string>().size() <= activity::kResultBytes + 8 && kept["content_bytes"] == 5000,
          "a tool result kept for later is shortened, its size noted");
    check(activity::compact(activity::compact(long_result))["content_bytes"] == 5000, "shortening twice keeps the first size");
    check(activity::compact(user("Wed 18 Nov 2026, 11:14", std::string(5000, 'u'))) == user("Wed 18 Nov 2026, 11:14", std::string(5000, 'u')),
          "other messages are kept as they are");
    json msgs = json::array({user("Wed 18 Nov 2026, 11:14", "look it up"), asks("", {call("c1", "web_search", "{}")}), kept});
    const json r = activity::conversation_record(msgs, 0, 1795003200, {});
    check(r["messages"][1]["tool_calls"][0].value("result_bytes", 0) == 5000, "a save gives a shortened result's full size");
}

void test_saving(const std::string &root) {
    const std::string dir = activity::conversations_dir();
    check(dir == root + "/data/zero-bot/conversations", "saves go to the data folder: " + dir);

    // Nothing is written unless asked: the activity log is, a conversation is not.
    activity::request();
    activity::tool("timer", "{}", "{}", 3);
    check(exists(activity::path()) && !exists(dir), "the activity log writes no conversation");
    const activity::SaveResult none = activity::save_conversation(json::array({reply("Hello.")}), 0);
    check(none.nothing && none.path.empty() && !exists(dir), "a conversation with nothing said is not saved");
    check(activity::saved_toast(none) == "Nothing to save yet", "and the toast says so");
    check(activity::saved_count() == 0, "nothing saved yet");

    Settings::update([](Settings &s) { s.api_key = "sk-secret-0123456789"; s.search_key = "tvly-secret-987654"; }, false);
    json msgs = sample();
    msgs[2]["content"] = "an answer that quotes sk-secret-0123456789 and tvly-secret-987654";
    umask(022);
    const activity::SaveResult saved = activity::save_conversation(msgs, 0);
    check(!saved.path.empty() && saved.error.empty() && activity::saved_toast(saved) == "Conversation saved", "a save is written");
    check(saved.path.rfind(dir + "/", 0) == 0 && saved.path.size() > 5 && saved.path.compare(saved.path.size() - 5, 5, ".json") == 0,
          "in the conversations folder, as JSON: " + saved.path);
    check(mode_of(dir) == 0700, "the folder is 0700, got " + std::to_string(mode_of(dir)));
    check(mode_of(saved.path) == 0600, "the file is 0600, got " + std::to_string(mode_of(saved.path)));
    const json back = util::load_json(saved.path, json());
    check(back.is_object() && back["messages"].size() == 6 && back.contains("app_version") && back.contains("fast_mode"),
          "the file reads back as the record");
    std::string text;
    util::read_file(saved.path, text);
    check(text.find("secret") == std::string::npos, "no key from the settings is in the file");
    check(activity::saved_count() == 1, "one saved");

    // At most kMaxSaved files: the oldest go first, the newest stays.
    std::string first = saved.path, last;
    for (size_t i = 0; i < activity::kMaxSaved + 4; ++i) last = activity::save_conversation(msgs, 0).path;
    check(activity::saved_count() == activity::kMaxSaved, "at most " + std::to_string(activity::kMaxSaved) + " kept, got " +
                                                              std::to_string(activity::saved_count()));
    check(!exists(first) && exists(last), "the oldest went, the newest stayed");

    // At most kMaxSavedBytes together.
    activity::delete_saved();
    json big = json::array();
    for (int i = 0; i < 150; ++i) {
        big.push_back(user("Wed 18 Nov 2026, 11:14", std::string(700, 'q')));
        big.push_back(reply(std::string(700, 'a')));
    }
    for (int i = 0; i < 25; ++i) last = activity::save_conversation(big, 0).path;
    size_t total = 0;
    struct stat st{};
    const size_t n = activity::saved_count();
    if (stat(last.c_str(), &st) == 0) total = static_cast<size_t>(st.st_size) * n;
    check(n < 25 && total <= activity::kMaxSavedBytes + static_cast<size_t>(st.st_size) && exists(last),
          "together they stay under the size limit: " + std::to_string(n) + " files of " + std::to_string(st.st_size) + " bytes");

    // Pruning and deleting touch only what the saver wrote: another file in the folder stays.
    util::write_file_atomic(dir + "/notes.json", "{}");
    util::write_file_atomic(dir + "/2026-11-18-120000-copy.json", "{}");
    util::write_file_atomic(dir + "/notes.json.tmp", "{");
    for (size_t i = 0; i < activity::kMaxSaved + 2; ++i) last = activity::save_conversation(msgs, 0).path;
    check(exists(dir + "/notes.json") && exists(dir + "/2026-11-18-120000-copy.json") && exists(dir + "/notes.json.tmp"),
          "pruning leaves files it did not write alone");
    check(activity::saved_count() == activity::kMaxSaved, "and does not count them");
    const size_t saves = activity::saved_count();
    util::write_file_atomic(dir + "/2026-11-18-120000.json.tmp", "{");          // a save cut short, before 0.4.6
    util::write_file_atomic(dir + "/2026-11-18-120001.json.tmp.Ab12Cd", "{");   // and since (a temporary file of its own)
    util::write_file_atomic(dir + "/notes.json.tmp.Ab12Cd", "{");
    const size_t deleted_with_others = activity::delete_saved();
    check(deleted_with_others == saves && activity::saved_count() == 0 && exists(dir + "/notes.json") &&
              exists(dir + "/2026-11-18-120000-copy.json") && exists(dir + "/notes.json.tmp") &&
              exists(dir + "/notes.json.tmp.Ab12Cd") && !exists(dir + "/2026-11-18-120000.json.tmp") &&
              !exists(dir + "/2026-11-18-120001.json.tmp.Ab12Cd"),
          "delete removes every save and half-written save, and nothing else");
    unlink((dir + "/notes.json.tmp.Ab12Cd").c_str());
    unlink((dir + "/notes.json").c_str());
    unlink((dir + "/2026-11-18-120000-copy.json").c_str());
    unlink((dir + "/notes.json.tmp").c_str());

    // Deleting them all removes the folder too.
    for (int i = 0; i < 3; ++i) last = activity::save_conversation(msgs, 0).path;
    util::write_file_atomic(last + ".tmp", "{");
    const size_t deleted = activity::delete_saved();
    check(deleted == 3 && activity::saved_count() == 0 && !exists(dir), "delete removes every save and the folder");
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: activity_test STATE_FOLDER\n"); return 2; }
    alarm(120);   // a save that loops (it once did, on a key inside "[removed]") fails the run instead of hanging it
    const std::string root = argv[1];
    util::make_dirs(root);
    setenv("HOME", root.c_str(), 1);
    setenv("XDG_CONFIG_HOME", (root + "/config").c_str(), 1);
    setenv("XDG_DATA_HOME", (root + "/data").c_str(), 1);
    setenv("XDG_CACHE_HOME", (root + "/cache").c_str(), 1);
    test_record_shape();
    test_unusual_messages();
    test_secrets();
    test_file_cap();
    test_compact();
    test_saving(root);
    printf("diagnostics: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
