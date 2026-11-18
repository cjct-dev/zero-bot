#include "core/radio.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

#include "core/tether.h"
#include "net/http.h"
#include "platform/audio.h"
#include "platform/proc.h"
#include "smarthome/home.h"

namespace radio {

namespace {

std::string fail(const std::string &msg) { return "ERROR: " + msg; }

// Names in sentences for the model are in double quotes, which home::clean_name() has removed from
// every name: a name cannot close its quotes and read as part of the sentence.
std::string in_quotes(const std::string &name) { return "\"" + name + "\""; }

bool starts_with(const std::string &s, const char *prefix) { return s.compare(0, strlen(prefix), prefix) == 0; }

// Only http and https: the address goes straight to the player, which would open a local file or
// any other protocol just as readily.
bool good_url(const std::string &u) {
    if (u.size() > 2048) return false;
    const size_t scheme = starts_with(u, "http://") ? 7 : starts_with(u, "https://") ? 8 : 0;
    if (!scheme || u.size() <= scheme) return false;
    for (const unsigned char c : u)
        if (c <= 0x20 || c == 0x7f) return false;
    return true;
}

// "City Radio Two!" and "city radio 2" both become "city radio 2": lower case, words of letters and
// digits, number words as digits, "&" as "and".
std::string plain(const std::string &text) {
    static const char *const kNumbers[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten"};
    std::vector<std::string> words;
    std::string word;
    auto flush = [&] {
        if (word.empty()) return;
        for (size_t n = 0; n < sizeof(kNumbers) / sizeof(*kNumbers); ++n)
            if (word == kNumbers[n]) word = std::to_string(n);
        words.push_back(word);
        word.clear();
    };
    for (const char c : util::lower(text)) {
        if (c == '&') { flush(); words.push_back("and"); continue; }
        if (isalnum(static_cast<unsigned char>(c)) || (c & 0x80)) word += c;
        else flush();
    }
    flush();
    std::string out;
    for (const auto &w : words) out += (out.empty() ? "" : " ") + w;
    return out;
}

std::vector<std::string> words_of(const std::string &plain_text) {
    std::vector<std::string> out;
    for (auto &w : util::split(plain_text, ' '))
        if (!w.empty()) out.push_back(w);
    return out;
}

bool all_in(const std::vector<std::string> &some, const std::vector<std::string> &of) {
    for (const auto &w : some)
        if (std::find(of.begin(), of.end(), w) == of.end()) return false;
    return true;
}

// The stations, kept until the file changes (or another file is meant: the XDG folders can move
// in tests).
struct Cache {
    std::mutex mtx;
    bool loaded = false, exists = false;
    std::string path;
    dev_t dev = 0;
    ino_t ino = 0;
    off_t size = 0;
    struct timespec mtime = {0, 0};
    std::vector<Station> list;
    std::string problem;
};

Cache &cache() {
    static Cache c;
    return c;
}

constexpr off_t kMaxFileBytes = 256 * 1024;

void refresh_locked(Cache &c) {
    const std::string p = path();
    struct stat st = {};
    const bool exists = ::stat(p.c_str(), &st) == 0;
    if (c.loaded && c.path == p && c.exists == exists &&
        (!exists || (st.st_dev == c.dev && st.st_ino == c.ino && st.st_size == c.size && st.st_mtim.tv_sec == c.mtime.tv_sec &&
                     st.st_mtim.tv_nsec == c.mtime.tv_nsec)))
        return;
    c.loaded = true;
    c.path = p;
    c.exists = exists;
    c.dev = exists ? st.st_dev : 0;
    c.ino = exists ? st.st_ino : 0;
    c.size = exists ? st.st_size : 0;
    c.mtime = exists ? st.st_mtim : timespec{0, 0};
    c.list.clear();
    c.problem.clear();
    if (!exists) return;   // no file: no stations, and nothing wrong
    std::string text;
    if (st.st_size > kMaxFileBytes) { c.problem = "the file is larger than 256 KB, so it was not read"; return; }
    if (!util::read_file(p, text)) { c.problem = "the file could not be read"; return; }
    std::vector<std::string> problems;
    c.list = parse(text, problems);
    for (const auto &q : problems) c.problem += (c.problem.empty() ? "" : "; ") + q;
}

std::string names_in_quotes(const std::vector<Station> &list) {
    std::string out;
    for (size_t i = 0; i < list.size(); ++i) out += (i ? ", " : "") + in_quotes(list[i].name);
    return out;
}

constexpr size_t kHead = 16 * 1024;   // what the check reads of a stream before it is satisfied

Check check_at(const std::string &url, std::atomic<bool> *cancel, int depth) {
    Check out;
    out.url = url;
    std::atomic<bool> enough{false};
    std::string head;
    size_t total = 0;
    http::Request req;
    req.url = url;
    req.timeout_ms = 8000;
    req.connect_timeout_ms = 5000;
    req.cancel = &enough;
    // Body bytes of a good answer arrive here; a stream never ends, so the transfer is ended once
    // enough has come (or the request has been cancelled).
    req.on_data = [&](const char *p, size_t n) {
        total += n;
        if (head.size() < kHead) head.append(p, std::min(n, kHead - head.size()));
        if (total >= kHead || (cancel && cancel->load())) enough = true;
    };
    const http::Response res = http::send(req);
    if (cancel && cancel->load()) { out.error = "the request was cancelled"; return out; }
    // Ended on purpose once enough had come, or a slow stream that was still trickling in.
    const bool flowing = total > 0 && (res.error == "cancelled" || res.error == "timed out");
    if (!res.error.empty() && !flowing) {
        if (res.error == "no connection")
            out.error = tether::online() ? "the station's server could not be reached: it may be off the air, or its address may have changed"
                                         : "there is no internet connection";
        else if (res.error == "timed out")
            out.error = "the station's server did not answer in time";
        else
            out.error = "the stream could not be opened (" + res.error + ")";
        return out;
    }
    const long status = res.status;
    if (status == 401 || status == 403 || status == 451)
        out.error = "the station refused to play it here, which usually means it is not available in this country";
    else if (status == 404 || status == 410)
        out.error = "the station's address no longer works: it has probably changed";
    else if (status == 429)
        out.error = "the station's server is busy: try again in a minute";
    else if (status >= 500)
        out.error = "the station's server reported a fault (HTTP " + std::to_string(status) + "): it may pass, or the address may be "
                    "missing a part it needs";
    else if (status >= 300)
        out.error = "the station's server refused the request (HTTP " + std::to_string(status) + ")";
    if (!out.error.empty()) return out;

    const std::string type = util::lower(res.content_type);
    const std::string start = util::lower(util::trim(head.substr(0, 512)));
    if (type.find("text/html") != std::string::npos || starts_with(start, "<!doctype html") || starts_with(start, "<html")) {
        out.error = "the address leads to a web page, not to the stream itself";
        return out;
    }
    if (starts_with(start, "#extm3u") && head.find("#EXT-X-") != std::string::npos) return out;   // HLS: the player reads it
    // A playlist file names the stream instead of being it: .pls ("File1=http://..."), or an .m3u
    // that is a plain list of addresses.
    const std::string bare = util::lower(url.substr(0, url.find_first_of("?#")));
    const bool pls = starts_with(start, "[playlist]") || type.find("scpls") != std::string::npos;
    const bool m3u = starts_with(start, "#extm3u") || type.find("mpegurl") != std::string::npos ||
                     (bare.size() > 4 && (bare.compare(bare.size() - 4, 4, ".m3u") == 0 || bare.compare(bare.size() - 4, 4, ".pls") == 0) &&
                      (type.empty() || starts_with(type, "text/")));
    if (pls || m3u) {
        std::string next;
        for (std::string line : util::split(head, '\n')) {
            line = util::trim(line);
            if (pls) {
                const size_t eq = line.find('=');
                if (eq == std::string::npos || !starts_with(util::lower(line), "file")) continue;
                line = util::trim(line.substr(eq + 1));
            } else if (line.empty() || line[0] == '#') {
                continue;
            }
            if (good_url(line)) { next = line; break; }
        }
        if (next.empty()) { out.error = "the playlist at that address names no stream"; return out; }
        if (depth >= 2) { out.error = "the address leads to a playlist of playlists, which cannot be played"; return out; }
        return check_at(next, cancel, depth + 1);
    }
    if (total == 0) out.error = "the station sent nothing";
    return out;
}

}  // namespace

std::string path() { return util::config_dir() + "/radio.json"; }

std::string shown_path() {
    const std::string p = path(), home = util::home_dir();
    return home.size() > 1 && p.compare(0, home.size() + 1, home + "/") == 0 ? "~" + p.substr(home.size()) : p;
}

std::vector<Station> parse(const std::string &text, std::vector<std::string> &problems) {
    std::vector<Station> out;
    if (util::trim(text).empty()) return out;                  // an empty file: no stations yet
    const json j = json::parse(text, nullptr, false, true);   // comments are allowed: people edit this file
    if (j.is_discarded()) { problems.push_back("the file is not valid JSON"); return out; }
    if (j.is_object() && j.empty()) return out;
    const json *list = j.is_array() ? &j : j.is_object() && j.contains("stations") ? &j["stations"] : nullptr;
    if (!list || !list->is_array()) { problems.push_back("the file should hold a list of stations under \"stations\""); return out; }
    std::vector<std::pair<std::string, size_t>> seen;   // plain names and nicknames taken, and by which station
    auto taken_by = [&seen](const std::string &p) {
        for (const auto &x : seen)
            if (x.first == p) return static_cast<long>(x.second);
        return -1L;
    };
    size_t n = 0;
    for (const json &e : *list) {
        ++n;
        const std::string which = "entry " + std::to_string(n);
        if (!e.is_object()) { problems.push_back(which + " is not a station (a name and a url)"); continue; }
        Station s;
        s.name = home::clean_name(util::jget<std::string>(e, "name", ""));
        if (s.name.empty() || plain(s.name).empty()) { problems.push_back(which + " has no name"); continue; }
        const std::string named = in_quotes(s.name);
        s.url = util::trim(util::jget<std::string>(e, "url", ""));
        if (s.url.empty()) { problems.push_back(named + " has no url"); continue; }
        if (!good_url(s.url)) { problems.push_back(named + ": the url must be a web address starting with http:// or https://"); continue; }
        if (taken_by(plain(s.name)) >= 0) {
            problems.push_back("two stations are called " + named + "; only the first is used");
            continue;
        }
        if (out.size() >= kMaxStations) { problems.push_back("only the first " + std::to_string(kMaxStations) + " stations are used"); break; }
        s.url_low = util::trim(util::jget<std::string>(e, "low_data_url", ""));
        if (!s.url_low.empty() && !good_url(s.url_low)) {
            problems.push_back(named + ": the low_data_url must start with http:// or https://, so it is not used");
            s.url_low.clear();
        }
        json nicks = e.contains("nicknames") ? e["nicknames"] : json::array();
        if (nicks.is_string()) nicks = json::array({nicks});
        if (!nicks.is_array()) { problems.push_back(named + ": nicknames should be a list of names"); nicks = json::array(); }
        seen.push_back({plain(s.name), out.size()});
        for (const json &nick : nicks) {
            if (!nick.is_string()) continue;
            const std::string name = home::clean_name(nick.get<std::string>());
            const std::string p = plain(name);
            const long owner = taken_by(p);
            if (p.empty() || owner == static_cast<long>(out.size())) continue;   // nothing, or a name it has already
            if (owner >= 0) {
                problems.push_back("the nickname " + in_quotes(name) + " belongs to two stations; only the first has it");
                continue;
            }
            if (s.nicknames.size() >= 8) { problems.push_back(named + ": only the first 8 nicknames are used"); break; }
            seen.push_back({p, out.size()});
            s.nicknames.push_back(name);
        }
        out.push_back(s);
    }
    return out;
}

std::vector<Station> stations() {
    Cache &c = cache();
    std::lock_guard<std::mutex> lk(c.mtx);
    refresh_locked(c);
    return c.list;
}

std::string problem() {
    Cache &c = cache();
    std::lock_guard<std::mutex> lk(c.mtx);
    refresh_locked(c);
    return c.problem;
}

int find(const std::vector<Station> &list, const std::string &said) {
    const std::string want = plain(said);
    if (want.empty()) return -1;
    // Its name, then one of its nicknames, said in full.
    for (size_t i = 0; i < list.size(); ++i)
        if (plain(list[i].name) == want) return static_cast<int>(i);
    for (size_t i = 0; i < list.size(); ++i)
        for (const auto &nick : list[i].nicknames)
            if (plain(nick) == want) return static_cast<int>(i);
    // Words that pick out one station: every word said is in its name or a nickname ("six jazz"
    // for "City Radio 6 Jazz"), or, failing that, every word of its name is in what was said
    // ("City Radio 2" for a station the user called "Radio 2"), the longest such name winning.
    const std::vector<std::string> said_words = words_of(want);
    int found = -1;
    for (size_t i = 0; i < list.size(); ++i) {
        std::vector<std::string> names = {list[i].name};
        names.insert(names.end(), list[i].nicknames.begin(), list[i].nicknames.end());
        for (const auto &n : names)
            if (all_in(said_words, words_of(plain(n)))) {
                if (found >= 0 && found != static_cast<int>(i)) return -1;
                found = static_cast<int>(i);
            }
    }
    if (found >= 0) return found;
    size_t best_words = 0;
    bool tie = false;
    for (size_t i = 0; i < list.size(); ++i) {
        std::vector<std::string> names = {list[i].name};
        names.insert(names.end(), list[i].nicknames.begin(), list[i].nicknames.end());
        for (const auto &n : names) {
            const std::vector<std::string> w = words_of(plain(n));
            if (w.empty() || !all_in(w, said_words)) continue;
            if (w.size() > best_words) { best_words = w.size(); found = static_cast<int>(i); tie = false; }
            else if (w.size() == best_words && found != static_cast<int>(i)) tie = true;
        }
    }
    return tie ? -1 : found;
}

bool metered_link() {
    if (const char *t = getenv("ZB_TEST_TETHER")) return *t && strcmp(t, "0") != 0;
    const std::string iface = tether::default_link();
    if (iface.empty()) return false;
    if (iface.compare(0, 4, "bnep") == 0) return true;   // a phone's Bluetooth tether
    // The drivers of the USB network links phones and mobile modems use (the same list as
    // discovery::link_kind); a USB Ethernet adapter has a driver of its own.
    char target[512];
    const ssize_t n = readlink(("/sys/class/net/" + iface + "/device/driver").c_str(), target, sizeof(target) - 1);
    if (n <= 0) return false;
    target[n] = 0;
    const char *slash = strrchr(target, '/');
    const std::string driver = slash ? slash + 1 : target;
    for (const char *phone : {"ipheth", "rndis_host", "cdc_ether", "cdc_ncm", "cdc_eem", "cdc_mbim", "cdc_subset", "huawei_cdc_ncm",
                              "qmi_wwan"})
        if (driver == phone) return true;
    return false;
}

std::string stream_for(const Station &s, bool metered) { return metered && !s.url_low.empty() ? s.url_low : s.url; }

Check check(const std::string &url, std::atomic<bool> *cancel) {
    if (!good_url(url)) return Check{url, "the address is not a web address"};
    return check_at(url, cancel, 0);
}

json tool_spec(bool can_search) {
    const std::vector<Station> list = stations();
    if (list.empty()) return nullptr;
    std::vector<std::string> names;
    std::string aka;
    for (const auto &s : list) {
        names.push_back(s.name);
        for (const auto &nick : s.nicknames) aka += (aka.empty() ? "" : "; ") + in_quotes(nick) + " is " + in_quotes(s.name);
    }
    const std::string description =
        "Live radio from the user's own list of stations: 'play " + list[0].name + "', 'turn the radio off'. "
        "action=play starts a station once you have finished speaking, in place of any music playing; action=stop ends "
        "it (music stop does too). Songs, artists and albums from the user's collection are music, not radio. " +
        std::string(can_search ? "The stream does not say which programme or song is on; to find out, use web_search."
                               : "The stream does not say which programme or song is on.");
    json props = json::object();
    props["action"] = {{"type", "string"}, {"description", "What to do"}, {"enum", json::array({"play", "stop"})}};
    props["station"] = {{"type", "string"},
                        {"description", "For play: the station" + (aka.empty() ? std::string() : ". Other names the user has for them: " + aka)},
                        {"enum", names}};
    props["say"] = {{"type", "string"},
                    {"description", "The one short sentence to speak to the user once this has worked, in their language, e.g. "
                                    "\"Here is " + list[0].name + ".\""}};
    return {{"description", description},
            {"parameters", {{"type", "object"}, {"properties", props}, {"required", json::array({"action"})}}}};
}

std::string prompt_line() {
    const std::vector<Station> list = stations();
    if (list.empty()) return "";
    std::string names;
    for (size_t i = 0; i < list.size(); ++i) names += (i ? ", " : "") + list[i].name;
    return "- Radio stations the user has set up, played with radio: " + names +
           ". A request for one of them by name means radio, not the music library. Only the user can add a station "
           "(in a file on the device); you cannot.\n";
}

std::string run_tool(const json &a, const ToolEnv &env) {
    const std::string action = util::lower(util::trim(util::jget<std::string>(a, "action", "")));
    const std::string said = util::trim(util::jget<std::string>(a, "station", ""));
    audio::Music &player = audio::Music::get();
    if (action == "stop") {
        if (!env.dry_run) player.stop();
        return "radio stopped";
    }
    const std::vector<Station> list = stations();
    if (list.empty())
        return fail("no radio stations are set up, so nothing was played. Tell the user that they can add their own in the file " +
                    shown_path() + "; settings, Your data, Radio stations says how");
    if (action != "play" && !(action.empty() && !said.empty())) return fail("action must be play or stop");
    if (said.empty()) return fail("give the station to play: one of " + names_in_quotes(list));
    const int i = find(list, said);
    if (i < 0)
        return fail("the user has no station called " + in_quotes(home::clean_name(said)) + ", so nothing was played. Their "
                    "stations are " + names_in_quotes(list));
    const Station &s = list[static_cast<size_t>(i)];
    const bool metered = metered_link();
    json out = {{"playing", s.name}};
    if (metered && !s.url_low.empty()) out["stream"] = "low data, because the internet connection is the phone's";
    else if (metered)
        out["note"] = "The internet connection is the phone's and this station has no low-data address: tell the user that it uses "
                      "about 30 to 70 MB of mobile data an hour.";
    if (env.dry_run) return out.dump();
    if (!proc::have("ffplay")) return fail("the player (ffplay) is not installed on this device, so radio cannot play");
    const Check c = check(stream_for(s, metered), env.cancel);
    if (!c.error.empty()) return fail(in_quotes(s.name) + " did not play: " + c.error + ". Tell the user");
    // Queued now, heard once the reply has been spoken (the agent or the speech queue lifts the
    // hold), as with music.
    if (env.in_conversation) player.hold(audio::Music::kExchange, true);
    player.play_live(c.url, s.name);
    return out.dump();
}

}  // namespace radio
