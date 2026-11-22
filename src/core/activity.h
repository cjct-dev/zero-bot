// DIAGNOSTICS, TEMPORARY. Everything in this module is there to diagnose and improve the assistant
// for now, and will be removed (ticket ZB-052) together with its call sites, each of which is
// marked "DIAGNOSTICS (temporary)":
//   src/agent/agent.cpp     Agent::Turn::run and Agent::Turn::run_tools (the log), trim_history and
//                           reset_conversation (what a save keeps), whole_conversation
//   src/agent/agent.h       Agent::whole_conversation, earlier_
//   src/ui/app_keys.cpp     save_conversation, settings_may_open, settle_keys; SPACE in handle_key
//                           (twice saves the conversation; one SPACE waits out the double tap)
//   src/ui/app_impl.h       space_tap, diagnostics_shots
//   src/ui/app_status.cpp   settle_keys in periodic
//   src/ui/key_guide.*      the DoubleTap helper and the guide's SPACE SPACE line
//   src/ui/menu.h           kPairKeyW, 96 to hold "SPACE SPACE" (84 before)
//   src/ui/settings_pages.cpp   Your data > Saved conversations (the row); the page is
//                           open_saved_conversations in settings_data.cpp, declared in settings_internal.h
//   src/ui/shots.cpp        App::diagnostics_shots (the last three --shots screens), and the SPACE
//                           checks in shots_keys
//   src/main.cpp            "/save" in --chat
// Also README "Diagnostics (temporary)" and the SPACE SPACE key, AGENTS.md sections 3, 6 and 7,
// tests/unit/activity_test.cpp (and its lines in tests/run_unit.sh), test_double_tap in
// tests/unit/key_guide_test.cpp, conversation_saved in tests/agent_loop_test.py, the last three
// golden screenshots (tests/golden/shots/*save*) and tests/suite/from_conversation.py. Removing it
// should also delete the files on the device.
//
// 1. The activity log: what the assistant did, each tool call with its arguments and result, so
// that a problem the owner reports ("it said it could not turn the TV on") can be diagnosed
// afterwards. It lives in ~/.cache/zero-bot/activity.log (mode 0600), at most about 256 kB plus one
// older file. What the user said is not written, nor anything secret: tool arguments and results
// never carry passwords or tokens (the smart-home results are names and states only).
//
// 2. Saved conversations: only when the user asks for it (SPACE twice on the chat screen), the
// conversation so far, from its start, as the model saw it: each user message with its time stamp,
// each reply, each tool call with its arguments and a shortened result. One JSON file per save in
// ~/.local/share/zero-bot/conversations/ (folder 0700, files 0600), named by the time it was saved;
// the oldest go first when there are more than kMaxSaved or they take more than kMaxSavedBytes.
// The keys in the settings are blanked out of anything saved; passwords and pairing codes never
// reach the conversation (they are typed in settings pages, and the smart-home tool has no
// pairing action).
#pragma once
#include <ctime>
#include <string>

#include "core/util.h"

namespace activity {

// ---------------------------------------------------------------- the activity log
// A line marking the start of a request.
void request();
// One tool call: its name, arguments (JSON), result and how long it took.
void tool(const std::string &name, const std::string &args, const std::string &result, long ms);
// Where the log is.
std::string path();

// ---------------------------------------------------------------- saved conversations
constexpr size_t kMaxSaved = 50;                      // files kept
constexpr size_t kMaxSavedBytes = 4 * 1024 * 1024;    // all of them together
constexpr size_t kMaxFileBytes = 256 * 1024;          // one save; the oldest messages go first beyond it
constexpr size_t kResultBytes = 800;                  // of each tool result

std::string conversations_dir();

// A message as it is kept for saving: a tool result shortened to kResultBytes. The agent keeps the
// messages trimmed from the model's context in this form, so that a save can start at the beginning.
json compact(const json &message);

// What a save holds, made from the agent's messages (`dropped` earlier ones were not kept), with
// `secrets` (the keys in the settings) blanked out wherever they appear. No file is touched.
json conversation_record(const json &messages, size_t dropped, time_t now, const std::vector<std::string> &secrets);

struct SaveResult {
    std::string path;    // the file written, or empty
    bool nothing = false;   // the conversation is empty: nothing was written
    std::string error;
};
// Writes one save (the secrets are the keys in the settings) and prunes the oldest beyond the
// limits. Called only when the user asks for it.
SaveResult save_conversation(const json &messages, size_t dropped);
// The words for the toast that follows.
std::string saved_toast(const SaveResult &r);

size_t saved_count();
// Deletes every saved conversation and the folder; returns how many files went.
size_t delete_saved();

}  // namespace activity
