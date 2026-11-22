// The music tool: music from the user's own library.
#include "agent/tools/common.h"

#include <algorithm>
#include <mutex>
#include <random>

#include "core/music_lib.h"
#include "core/radio.h"
#include "core/settings.h"
#include "platform/audio.h"

namespace tools {

namespace {

// A request for music that names a radio station ("play Radio 2") when the music library is what
// the model reached for: say how radio stands, so that the reply can say so too.
void radio_hint(const json &a, json &out) {
    std::string asked = util::lower(str_arg(a, "query") + " " + str_arg(a, "title") + " " + str_arg(a, "artist") + " " + str_arg(a, "album"));
    for (char &c : asked)
        if (!isalnum(static_cast<unsigned char>(c))) c = ' ';
    const auto words = util::split(asked, ' ');
    if (std::find(words.begin(), words.end(), "radio") == words.end() && std::find(words.begin(), words.end(), "fm") == words.end()) return;
    out["radio"] = radio::stations().empty()
                       ? "No radio stations are set up, so radio cannot play. The user can add their own: settings, Your data, Radio "
                         "stations says how."
                       : "A radio station is played with the radio tool, not from the music library.";
}

// With ZB_DRY_RUN nothing plays, but the music tool keeps a pretend queue of what it would have played,
// so that "next" and "now_playing" answer as they would with a player (the test sets ask for a song
// and then skip it). Without ZB_DRY_RUN it is never touched.
struct DryQueue {
    std::mutex mtx;
    std::vector<std::string> paths;
    size_t at = 0;
    std::string current() { return at < paths.size() ? paths[at] : std::string(); }
};
DryQueue &dry_queue() { static DryQueue q; return q; }

std::string t_music(const json &a, Context &ctx) {
    const std::string action = util::lower(str_arg(a, "action"));
    audio::Music &player = audio::Music::get();
    MusicLibrary &lib = MusicLibrary::get();
    DryQueue &dry = dry_queue();
    std::unique_lock<std::mutex> dry_lock(dry.mtx, std::defer_lock);
    if (dry_run()) dry_lock.lock();
    if (action == "stop") {
        if (!dry_run()) player.stop();
        else dry.paths.clear();
        return "music stopped";
    }
    if (action == "next") {
        if (dry_run()) {
            if (dry.current().empty()) return fail("nothing is playing");
            if (++dry.at >= dry.paths.size()) dry.paths.clear();   // past the last track, as the player stops
            return "skipped to the next track";
        }
        if (!player.playing()) return fail("nothing is playing");
        if (player.live()) return fail("a radio station is playing, not a list of songs; another station is started with radio");
        player.next();
        return "skipped to the next track";
    }
    if (action == "now_playing") {
        if (!dry_run() && player.live())
            return json{{"radio_station", player.title()},
                        {"note", "a live station: the stream does not say which programme or song is on"}}.dump();
        const std::string cur = dry_run() ? dry.current() : player.current();
        if (cur.empty()) return "nothing is playing";
        const Track t = lib.lookup(cur);
        return json{{"title", t.title}, {"artist", t.artist}, {"album", t.album}}.dump();
    }
    if (action == "library") {
        json out = {{"tracks", lib.size()}};
        json g = json::array(), ar = json::array();
        for (auto &x : lib.genres()) { g.push_back(x); if (g.size() >= 25) break; }
        for (auto &x : lib.artists()) { ar.push_back(x); if (ar.size() >= 40) break; }
        out["genres"] = g;
        out["top_artists"] = ar;
        return out.dump(-1, ' ', false, json::error_handler_t::replace);
    }
    if (action != "play" && action != "search") return fail("action must be play, search, stop, next, now_playing or library");

    if (lib.size() == 0) {
        json hint = json::object();
        radio_hint(a, hint);
        return fail("the music library is empty; songs go in " + Settings::get().eff_music_dir() +
                    (hint.contains("radio") ? ". " + hint["radio"].get<std::string>() : std::string()));
    }
    std::vector<Track> found = lib.find(str_arg(a, "query"), str_arg(a, "artist"), str_arg(a, "genre"),
                                        str_arg(a, "title"), str_arg(a, "album"));
    if (found.empty()) {
        json g = json::array();
        for (auto &x : lib.genres()) { g.push_back(x); if (g.size() >= 20) break; }
        json out = {{"error", "nothing in the library matches, so nothing was started"},
                    {"hint", "tell the user it is not in their library; these genres are"},
                    {"genres", g}};
        radio_hint(a, out);
        return out.dump(-1, ' ', false, json::error_handler_t::replace);
    }
    if (action == "search") {
        json arr = json::array();
        for (auto &t : found) {
            arr.push_back(t.title + (t.artist.empty() ? "" : " - " + t.artist) + (t.genre.empty() ? "" : " [" + t.genre + "]"));
            if (arr.size() >= 15) break;
        }
        return json{{"matches", found.size()}, {"first", arr}}.dump(-1, ' ', false, json::error_handler_t::replace);
    }
    const bool one = str_arg(a, "title").size() > 0 && found.size() <= 3;
    if (bool_arg(a, "shuffle", !one)) {
        static std::mt19937 rng(std::random_device{}());
        std::shuffle(found.begin(), found.end(), rng);
    }
    if (found.size() > 60) found.resize(60);
    std::vector<std::string> paths;
    for (auto &t : found) paths.push_back(t.path);
    if (!dry_run()) {
        // Queue it now, start it once the reply has been spoken (the agent or the speech queue
        // lifts the hold), so the song does not begin, stop for the reply and begin again.
        if (ctx.in_conversation) player.hold(audio::Music::kExchange, true);
        player.play(paths);
    } else {
        dry.paths = paths;
        dry.at = 0;
    }
    const Track &first = found[0];
    return json{{"now_playing", first.title + (first.artist.empty() ? "" : " by " + first.artist)},
                {"queued_tracks", found.size()}}.dump(-1, ' ', false, json::error_handler_t::replace);
}

// The other actions return something the model has not seen yet (the track that started), so the answer
// has to be written after the result.
bool say_confirms(const json &args, const std::string &) {
    const std::string action = util::lower(str_arg(args, "action"));
    return action == "stop" || action == "next";
}

}  // namespace

Tool make_music() {
    return {"music", "Music",
            "Control music from the user's own library on this device. action=play starts playback of everything "
            "matching the filters (all filters optional; none = shuffle the whole library) and returns the track that "
            "actually started; tell the user that track. action=search lists matches without playing. Other actions: "
            "stop, next, now_playing, library (counts, genres and artists).",
            obj({{"action", E("What to do", {"play", "search", "stop", "next", "now_playing", "library"})},
                 {"artist", S("Artist name or part of it, e.g. \"ABBA\"")},
                 {"genre", S("Genre or part of it, e.g. \"Rock\"")},
                 {"title", S("Song title or part of it")},
                 {"album", S("Album name or part of it")},
                 {"query", S("Loose words matched against title, artist, album and genre together")},
                 {"shuffle", B("Shuffle the matches (default true unless one specific song was asked for)")}},
                {"action"}), t_music,
            takes_say("Only for stop and next; omit for the other actions.", say_confirms)};
}

}  // namespace tools
