// Internet radio: the user's own list of stations, the radio tool, and a check of a stream before
// it is played. The app ships no stations; the list is a file the user fills in
// (~/.config/zero-bot/radio.json, see the README). While it is empty or missing, the tool and the
// line in the system prompt do not exist, so the model never tries to use radio.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "core/util.h"

namespace radio {

struct Station {
    std::string name;                     // as the user wrote it, cleaned ("Harbour FM")
    std::vector<std::string> nicknames;   // other names the user calls it ("Harbour")
    std::string url;                      // the stream
    std::string url_low;                  // optional: a low-data stream, used on a phone's connection
};

constexpr size_t kMaxStations = 50;

std::string path();                        // ~/.config/zero-bot/radio.json
std::string shown_path();                  // the same, with ~ for the home folder, to show a person
// The stations in the file. The file is read again whenever it has changed, so a station added
// while the app runs is there at the next request. Cheap: one stat() when nothing has changed.
std::vector<Station> stations();
// What is wrong with the file (unreadable JSON, an entry without an address, ...), or "".
std::string problem();
// The file's text turned into stations; `problems` lists what was left out and why. Exposed for tests.
std::vector<Station> parse(const std::string &text, std::vector<std::string> &problems);

// The station a name refers to: its name or a nickname, ignoring case, punctuation and number
// words ("radio two" is "Radio 2"), or words that pick out exactly one station ("six music").
// Returns -1 when none does or several do.
int find(const std::vector<Station> &list, const std::string &said);

// Whether the internet connection is a phone's (its Bluetooth tether or a USB link of the kind
// phones and mobile modems use), where data may cost money. ZB_TEST_TETHER=1 pretends it is.
bool metered_link();
// The address to play: the low-data one on a phone's connection when the station has one.
std::string stream_for(const Station &s, bool metered);

// Opens the address for a moment, as the player will, and says what is wrong if anything is: no
// connection, refused (often: not available in this country), gone, a web page rather than a
// stream. A playlist file (.pls or a plain .m3u) is followed to the first stream it names, which
// is what `url` is then. `error` is empty when the stream can be played.
struct Check {
    std::string url, error;
};
Check check(const std::string &url, std::atomic<bool> *cancel = nullptr);
// The same for a station chosen with a key or in settings, without waiting on the UI thread: the
// station's stream (the low-data one on a phone's connection) is checked on a thread of its own,
// and `done` runs on the UI thread (uiq) with the result, where the caller plays `url` (a playlist
// file already followed: the player cannot open one itself) or says why not. Only the newest
// call's result is handed over: a station chosen meanwhile wins.
void check_async(const Station &s, std::function<void(const Check &)> done);

// ---- the radio tool (wired up in agent/tools.cpp)
// Its description and parameters for the model, made from the station list (names as allowed
// values, nicknames as data), or null when there are no stations: the tool is then left out.
json tool_spec(bool can_search);
// The system prompt's line about radio, or "" when there are no stations.
std::string prompt_line();
struct ToolEnv {
    bool dry_run = false;                  // ZB_DRY_RUN: nothing is checked or played
    bool in_conversation = false;          // the spoken reply is still to come: start after it
    std::atomic<bool> *cancel = nullptr;
};
std::string run_tool(const json &args, const ToolEnv &env);

}  // namespace radio
