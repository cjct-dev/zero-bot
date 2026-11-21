// See experiment.h. Nothing here is compiled unless the build sets ZB_EXPERIMENTS.
#include "agent/experiment.h"

#if ZB_EXPERIMENTS

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#include "agent/llm.h"
#include "agent/tools.h"

namespace experiment {

namespace {

using Clock = std::chrono::steady_clock;

std::mutex g_mtx;                          // the trace file, the current request
const Clock::time_point g_process_start = Clock::now();
int g_req = 0;                             // requests begun in this process
int g_calls = 0;                           // model calls in the current request
Clock::time_point g_req_start;
bool g_in_request = false;
bool g_started = false;                    // the "start" line has been written
std::atomic<int> g_probes{0};              // speech probes still running

long ms_since(Clock::time_point t) {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count());
}

bool env_on(const char *name) {
    const char *v = getenv(name);
    return v && *v && std::string(v) != "0";
}

void write_line(const json &event) {   // g_mtx held
    FILE *f = fopen(get().trace.c_str(), "a");
    if (!f) return;
    const std::string line = event.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
    fwrite(line.data(), 1, line.size(), f);
    fclose(f);
}

// ZB_EXP_WEB's value as the address requests are sent to, or "" when it is not one: "http://", an IPv4
// address in plain decimal (four parts, each 0 to 255 with no leading zero, the first 127), then nothing but
// an optional ":port" (1 to 65535) and a "/path". The address sent is rebuilt from the numbers read, so no
// other reading of the spelling ("0177.0.0.1" as octal, "127.1" as short form) can choose where it goes.
std::string stand_in_base(const std::string &v) {
    if (v.compare(0, 7, "http://") != 0 || v.find_first_of("@?#\\ \t\r\n") != std::string::npos) return "";
    size_t i = 7;
    // Up to max_len digits from i, as a number; -1 when there are none, too many, or a leading zero.
    auto number = [&](size_t max_len) {
        const size_t start = i;
        while (i < v.size() && isdigit(static_cast<unsigned char>(v[i])) && i - start <= max_len) ++i;
        const size_t len = i - start;
        if (len == 0 || len > max_len || (len > 1 && v[start] == '0')) return -1L;
        return std::stol(v.substr(start, len));
    };
    std::string out = "http://";
    for (int k = 0; k < 4; ++k) {
        if (k > 0) {
            if (i >= v.size() || v[i] != '.') return "";
            ++i;
        }
        const long octet = number(3);
        if (octet < 0 || octet > 255 || (k == 0 && octet != 127)) return "";
        out += (k > 0 ? "." : "") + std::to_string(octet);
    }
    if (i < v.size() && v[i] == ':') {
        ++i;
        const long port = number(5);
        if (port < 1 || port > 65535) return "";
        out += ":" + std::to_string(port);
    }
    if (i < v.size() && v[i] != '/') return "";   // after the address only a port and a path
    out += v.substr(i);
    while (out.size() > 7 && out.back() == '/') out.pop_back();
    return out;
}

}  // namespace

// A variant switch: its value, the default when unset, and a stop for a value that is not one (a
// typing slip would otherwise measure the default under another name).
std::string variant(const char *name, const char *dflt, std::initializer_list<const char *> known) {
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    for (const char *k : known)
        if (std::string(v) == k) return v;
    fprintf(stderr, "%s=%s is not a variant this build knows\n", name, v);
    exit(2);
}

const Switches &get() {
    static const Switches s = [] {
        Switches r;
        if (const char *t = getenv("ZB_EXP_TRACE")) r.trace = t;
        r.cache_bust = env_on("ZB_EXP_CACHE_BUST");
        r.tts_probe = env_on("ZB_EXP_TTS_PROBE");
        r.tools = variant("ZB_EXP_TOOLS", "full", {"full", "say1", "lean"});
        r.decide = variant("ZB_EXP_DECIDE", "second_look", {"second_look", "parallel", "required", "required_held"});
        if (const char *m = getenv("ZB_EXP_CHECK_MODEL")) r.check_model = m;
        if (const char *w = getenv("ZB_EXP_WEB"); w && *w) {
            // Only a stand-in on this computer: the switch must never send a search or a key elsewhere.
            r.web = stand_in_base(w);
            if (r.web.empty()) {
                fprintf(stderr, "ZB_EXP_WEB=%s is not an http://127.x address\n", w);
                exit(2);
            }
        }
        r.no_hardware = env_on("ZB_EXP_NO_HARDWARE");
        if (const char *n = getenv("ZB_EXP_NET"); n && *n) {
            r.net = n;
            const std::string v = n;
            if (v != "loopback" && !((v.compare(0, 8, "https://") == 0 || v.compare(0, 7, "http://") == 0) &&
                                     v.find_first_of("/?#@\\ ", v.find("://") + 3) == std::string::npos && v.size() > v.find("://") + 3)) {
                fprintf(stderr, "ZB_EXP_NET=%s is neither loopback nor one origin (scheme://host[:port])\n", n);
                exit(2);
            }
        }
        return r;
    }();
    return s;
}

std::string web(const std::string &url) {
    const std::string &base = get().web;
    const size_t scheme = url.find("://");
    if (base.empty() || scheme == std::string::npos) return url;
    return base + "/" + url.substr(scheme + 3);
}

bool stand_in(const std::string &url) {
    const std::string &base = get().web;
    return !base.empty() && url.size() > base.size() && url.compare(0, base.size(), base) == 0 && url[base.size()] == '/';
}

namespace {
// scheme, lower-case host and port of a URL ("" host when it cannot be read).
struct Origin { std::string scheme, host; long port = 0; };
Origin origin_of(const std::string &url) {
    Origin o;
    const size_t s = url.find("://");
    if (s == std::string::npos) return o;
    o.scheme = util::lower(url.substr(0, s));
    const size_t start = s + 3, end = url.find_first_of("/?#", start);
    std::string auth = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (auth.find('@') != std::string::npos) return Origin{};   // credentials in the address: never allowed
    const size_t colon = auth.rfind(':');
    if (colon != std::string::npos && auth.find(']') == std::string::npos) {
        const std::string p = auth.substr(colon + 1);
        if (p.empty() || p.size() > 5 || !std::all_of(p.begin(), p.end(), ::isdigit)) return Origin{};
        o.port = std::stol(p);
        auth = auth.substr(0, colon);
    }
    o.host = util::lower(auth);
    if (o.port == 0) o.port = o.scheme == "https" ? 443 : 80;
    return o;
}
bool loopback(const std::string &host) {
    if (host == "localhost" || host == "[::1]") return true;
    if (host.compare(0, 4, "127.") != 0) return false;
    return std::all_of(host.begin(), host.end(), [](char c) { return isdigit(static_cast<unsigned char>(c)) || c == '.'; });
}
}  // namespace

std::string net_refusal(const std::string &url) {
    const std::string &net = get().net;
    if (net.empty()) return "";
    const Origin o = origin_of(url);
    if (!o.host.empty() && (o.scheme == "http" || o.scheme == "https") && loopback(o.host)) return "";
    if (net != "loopback") {
        const Origin allowed = origin_of(net);
        if (!o.host.empty() && o.scheme == allowed.scheme && o.host == allowed.host && o.port == allowed.port) return "";
    }
    return "refused: a test run sends nothing but to 127.x" + std::string(net == "loopback" ? "" : " and the AI service") +
           " (ZB_EXP_NET), not to " + (o.host.empty() ? std::string("that address") : o.scheme + "://" + o.host + ":" + std::to_string(o.port));
}

std::string no_lan() {
    return get().net.empty() ? "" : "The real network is not looked at in a test run (ZB_EXP_NET).";
}

std::string no_camera() {
    return get().no_hardware ? "the camera is switched off for this test run (ZB_EXP_NO_HARDWARE): set ZB_FAKE_CAMERA to a picture"
                             : "";
}

std::string no_infrared() {
    return get().no_hardware ? "the infrared transmitter is switched off for this test run (ZB_EXP_NO_HARDWARE)" : "";
}

void trace(json event) {
    if (!tracing()) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_started) {
        g_started = true;
        write_line({{"ev", "start"}, {"version", ZB_VERSION}, {"pid", static_cast<long>(getpid())},
                    {"cache_bust", get().cache_bust}, {"tts_probe", get().tts_probe}, {"tools", get().tools},
                    {"decide", get().decide}, {"check_model", get().check_model.empty() ? "main" : "other"}});
    }
    if (!event.contains("ev")) event["ev"] = "note";
    event["at_ms"] = ms_since(g_process_start);
    write_line(event);
}

std::string cache_bust_line() {
    if (!get().cache_bust) return "";
    static const std::string line = [] {
        std::random_device rd;
        char buf[64];
        snprintf(buf, sizeof buf, "(Session %08x%08x.)\n\n", rd(), rd());
        return std::string(buf);
    }();
    return line;
}

long request_ms() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_in_request ? ms_since(g_req_start) : -1;
}

void trace_call(const json &call) {
    if (!tracing()) return;
    json ev = call;
    ev["ev"] = "call";
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        ev["req"] = g_req;
        ev["call"] = ++g_calls;
    }
    trace(ev);
}

json numbers_only(const json &v) {
    if (v.is_number() || v.is_boolean()) return v;
    if (v.is_object()) {
        json out = json::object();
        for (auto it = v.begin(); it != v.end(); ++it) {
            json kept = numbers_only(it.value());
            if (!kept.is_null()) out[it.key()] = kept;
        }
        return out;
    }
    return nullptr;
}

void wait_probes(int max_ms) {
    for (int waited = 0; g_probes > 0 && waited < max_ms; waited += 20) usleep(20 * 1000);
}

// ---------------------------------------------------------------- E2: the tool text sent

namespace {

// say1: what every confirmable tool's "say" says, once the system prompt explains it in full.
const char *const kSayShort = "Spoken once this has worked; see Using tools.";

// The limits the full text adds for some tools, kept.
std::string say_limit(const std::string &tool) {
    if (tool == "music") return " Only for stop and next.";
    if (tool == "timer") return " Only for start, cancel and add_time.";
    if (tool == "device" || tool == "smart_home") return " Not for status.";
    return "";
}

// lean: shorter descriptions for the tools whose text is fixed (smart_home and radio make theirs
// from the user's own devices and stations, and keep it). Every boundary rule of the full text is
// kept: timer, reminder or calendar; smart_home, tv_remote or device; weather or web_search;
// birthdays in the calendar, not memory; "the only way" for every tool that changes something.
struct Lean {
    const char *tool, *description;
    std::vector<std::pair<const char *, const char *>> params;
};

const std::vector<Lean> &lean_table() {
    static const std::vector<Lean> t = {
        {"calculator",
         "Exact arithmetic, for every sum beyond single digits: tips, splits, percentages, unit conversions. Not for "
         "dates (date_info). + - * / ^ %, brackets, sqrt, sin, cos, tan, ln, log, exp, abs, round, min, max, 5!, pi, e; "
         "'15% of 80' is 0.15*80.",
         {{"expression", "The expression, e.g. \"(84.50 + 12) * 1.125 / 4\""}}},
        {"run_python",
         "Run Python 3 in a sandbox and return what it prints, for multi-step calculations, text processing or "
         "anything calculator and date_info cannot do. Standard library only, no internet, no files, fresh each call; "
         "print() the result. 20 s limit.",
         {{"code", "Complete Python source"}}},
        {"date_info",
         "Exact facts about a date: its weekday and how many days it is from today. Use it for every 'how many days "
         "until', 'how long ago', 'what weekday is', 'the date N days from now'; never count days in your head.",
         {{"date", "YYYY-MM-DD, e.g. the coming 25 December for Christmas; omit for today"},
          {"add_days", "Optional: days to move on first (negative for back)"}}},
        {"look",
         "Take a photo with the camera (on the back, facing away from the user) and answer a question about it. Use it "
         "whenever the user refers to something in front of them: 'what is this', 'read this', 'translate this sign', "
         "'how much is this'. Each call takes a new photo.",
         {{"question", "What to find out, with any detail the answer needs, e.g. \"Transcribe the menu and translate it into English\""}}},
        {"calendar_add",
         "Add an event to the calendar; calling this is the only way one is saved. A timed event needs a day and a time "
         "from the user: ask for a missing one, never guess. A birthday, anniversary or holiday needs only its date: "
         "all_day, and repeat yearly for a birthday or anniversary. Birthdays and anniversaries go here, not in memory_save.",
         {{"title", "Short title, e.g. \"Dinner with Marta\" or \"Mum's birthday\""},
          {"start", "YYYY-MM-DD HH:MM (24-hour), or YYYY-MM-DD for all day; a repeating event's next time"},
          {"since_year", "Birthday or anniversary: the year it began, only if the user said it (born 1990 -> 1990); never ask"},
          {"end", "Optional end, YYYY-MM-DD HH:MM"},
          {"all_day", "True for a whole day with no clock time (birthday, holiday, day off)"},
          {"repeat", "Leave out for a one-off"},
          {"notes", "Optional detail such as place or booking reference"},
          {"remind_minutes_before", "Optional alert this many minutes before (0 = at the start; all-day events from 09:00 on the day)"}}},
        {"calendar_list",
         "Look at the calendar: a date range (the next 14 days by default), or with search the next time an event of "
         "that name comes up within a year ('when is dad's birthday'). Results carry ids; a repeating event appears "
         "once for each time it falls in a range.",
         {{"search", "Optional words from the event's name, e.g. \"dentist\""},
          {"from", "Optional first day, YYYY-MM-DD"},
          {"to", "Optional last day (inclusive), YYYY-MM-DD"}}},
        {"calendar_update",
         "Change an existing event (move, rename, make it repeat). Only the fields given change; a repeating event "
         "changes as a whole. The id comes from calendar_add or calendar_list; never add a second event to move one.",
         {{"id", "Event id"}, {"title", "New title"}, {"start", "New start, YYYY-MM-DD HH:MM"}, {"end", "New end, YYYY-MM-DD HH:MM"},
          {"notes", "New notes"}, {"all_day", "Whole-day event or not"}, {"repeat", "none stops it repeating"},
          {"remind_minutes_before", "New alert lead time in minutes; -1 removes it"}}},
        {"calendar_delete", "Delete an event by id (from calendar_list); a repeating one goes as a whole.", {{"id", "Event id"}}},
        {"timer",
         "Countdown timers, for anything counted down from now ('a timer for 10 minutes', 'the eggs need six minutes'): "
         "the time left shows on screen and an alarm rings at the end. Several can run, each with a short label. start "
         "needs the length; ask if none was given. cancel and add_time pick one by words of its label (none needed with "
         "one running). list gives each one's time left. The only way a timer is started, changed or cancelled.",
         {{"action", "What to do"},
          {"minutes", "start, add_time: minutes (fractions allowed)"},
          {"seconds", "start, add_time: seconds, alone or on top of minutes"},
          {"label", "start: a short name from the user's words, or leave out; cancel, add_time: words from its label"},
          {"all", "cancel: true for every timer"}}},
        {"reminder_set",
         "Set a reminder or alarm: a message chimed and spoken at a clock time or on a date ('remind me at 6 on Friday "
         "to call the bank', 'wake me at half six', 'an alarm for 7'). The only way one is set; use at for the clock "
         "time. A countdown from now ('in 5 minutes', 'for an hour') is a timer instead. No time given: ask when. Fires "
         "only while the app runs. An appointment belongs in calendar_add with remind_minutes_before.",
         {{"text", "What to say when it fires, e.g. \"Check the laundry\""},
          {"in_minutes", "Minutes from now (fractions allowed)"},
          {"at", "Or a local time, YYYY-MM-DD HH:MM"}}},
        {"reminder_list", "List pending reminders with their ids (timers: timer with action list).", {}},
        {"reminder_cancel", "Cancel a reminder by id (from reminder_list). Timers are cancelled with timer.", {{"id", "Reminder id"}}},
        {"memory_save",
         "Save a lasting fact about the user for every future conversation: preferences, diet, allergies, names, travel "
         "plans, anything they ask you to remember. The only way anything is remembered. One fact per call, making sense "
         "on its own; replace_id corrects an existing memory instead of adding a duplicate.",
         {{"text", "The fact, e.g. \"Allergic to shellfish\""}, {"replace_id", "Optional id of a memory to overwrite"}}},
        {"memory_delete", "Delete a saved memory by id when the user asks you to forget it or it is wrong.",
         {{"id", "Memory id as listed in your instructions"}}},
        {"web_search",
         "Search the web. Always for details that change or are easy to misremember: opening hours and days, prices, "
         "timetables, travel rules, news, results, who holds a post now, anything recent. Not for settled general "
         "knowledge. Returns a summary and snippets.",
         {{"query", "A specific query, with places and dates where relevant"}}},
        {"weather", "Current weather and a daily forecast for a place.",
         {{"location", "City, optionally with country; omit for the home city"}, {"days", "1 to 7 (default 3)"}}},
        {"convert_currency", "Convert an amount between currencies at a current rate.",
         {{"amount", "Amount"}, {"from", "Source currency code, e.g. EUR"}, {"to", "Target currency code, e.g. GBP"}}},
        {"stock_price", "Latest price of a share, index, fund or cryptocurrency, with the day's change and range.",
         {{"symbol", "Ticker (AAPL, RR.L, ^FTSE, BTC-USD) or company name"}}},
        {"world_time",
         "The current time in another place, or a given local time converted there. Not for the user's own time: it is "
         "in your instructions.",
         {{"place", "City or IANA zone, e.g. \"Tokyo\""}, {"local_time", "Optional local time to convert, YYYY-MM-DD HH:MM"}}},
        {"set_timezone", "Change this device's time zone, when the user has travelled or asks to.",
         {{"place", "City or IANA zone the user is now in"}}},
        {"music",
         "The user's own music library on this device. play starts everything matching the filters (all optional; none "
         "shuffles the whole library) and returns the track that started: tell the user that track. search lists "
         "matches without playing. Also stop, next, now_playing, library (counts, genres, artists).",
         {{"action", "What to do"}, {"artist", "Artist, or part of the name"}, {"genre", "Genre, or part of it"},
          {"title", "Song title, or part of it"}, {"album", "Album, or part of it"},
          {"query", "Loose words matched against title, artist, album and genre"},
          {"shuffle", "Default true unless one song was asked for"}}},
        {"tv_remote",
         "Press a TV button by infrared (the user points the device's top edge at the TV). One key per call; repeat for "
         "several presses (volume_up, repeat 5). Pass brand if the user says it. A TV that smart_home lists is controlled "
         "with smart_home instead; this is for other TVs, and no fallback when a listed TV does not answer: say so.",
         {{"key", "Button to press"}, {"repeat", "Presses (default 1)"},
          {"brand", "Optional: samsung, lg, sony, panasonic, philips, toshiba, vizio"}}},
        {"breathe",
         "A guided breathing exercise on the screen. Use it when the user is stressed, anxious, overwhelmed, cannot "
         "settle or sleep, or asks to meditate, breathe or calm down. Start it at once: no questions about kind or "
         "length unless they raise it, and no advice instead.",
         {{"pattern", "box: in, hold, out, hold, four seconds each (default); calm: in 4, out 6; sigh: two in, one long out, for winding down"},
          {"minutes", "1 to 20; leave out for 5"}}},
        {"device", "Read or change this device's own settings; status reports battery, Wi-Fi, volume and brightness.",
         {{"setting", "Which setting"}, {"value", "volume or brightness: 0 to 100"},
          {"on", "spoken_replies: true to speak answers, false for text only"}}},
    };
    return t;
}

}  // namespace

void shape_tools(json &schema) {
    if (decide_required() && schema.is_array())   // E3b: last, so the other tools' text is as it always is
        schema.push_back(
            {{"type", "function"},
             {"function",
              {{"name", "reply"},
               {"description",
                "Say something to the user in words: conversation, settled general knowledge, translations, the current "
                "local time, facts you remember about the user, a question asking for a missing detail, or saying plainly "
                "that you cannot do something none of your tools does. Never to say that something was done, saved, set, "
                "played or changed (call the tool that does it), nor for live or changeable information, date or number "
                "work, or what is in the calendar, reminders, timers or music library: those have their own tools."},
               {"parameters",
                {{"type", "object"},
                 {"properties", {{"text", {{"type", "string"}, {"description", "What to say, as it will be read aloud"}}}}},
                 {"required", json::array({"text"})}}}}}});
    const std::string &v = get().tools;
    if ((v != "say1" && v != "lean") || !schema.is_array()) return;
    for (json &t : schema) {
        if (!t.is_object() || !t.contains("function") || !t["function"].is_object()) continue;
        json &fn = t["function"];
        const std::string name = util::jget(fn, "name", std::string());
        json *props = nullptr;
        if (fn.contains("parameters") && fn["parameters"].is_object() && fn["parameters"].contains("properties") &&
            fn["parameters"]["properties"].is_object())
            props = &fn["parameters"]["properties"];
        if (props && props->contains("say") && (*props)["say"].is_object())
            (*props)["say"]["description"] = kSayShort + say_limit(name);
        if (v != "lean") continue;
        for (const Lean &l : lean_table()) {
            if (name != l.tool) continue;
            fn["description"] = l.description;
            for (const auto &p : l.params)   // only parameters the tool has: the variant never adds one
                if (props && props->contains(p.first) && (*props)[p.first].is_object()) (*props)[p.first]["description"] = p.second;
        }
    }
}

std::string say_rule(const std::string &standard) {
    if (get().tools != "say1" && get().tools != "lean") return standard;
    return "- Some tools take 'say': the one short sentence to speak to the user once the call has worked, with the detail "
           "that matters (\"Your timer is set for 12 minutes.\"). ";
}

// ---------------------------------------------------------------- E3b: answers through the reply tool

namespace {

int hex4(const std::string &s, size_t at) {
    int v = 0;
    for (size_t i = at; i < at + 4; ++i) {
        const char c = s[i];
        v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -100000);
    }
    return v < 0 ? -1 : v;
}

void append_utf8(std::string &out, unsigned cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

}  // namespace

std::string ReplyCalls::feed(size_t idx, const std::string &name, const std::string &piece) {
    if (!decide_required() || name != "reply" || idx > 64) return "";
    if (calls_.size() <= idx) calls_.resize(idx + 1);
    One &c = calls_[idx];
    c.raw += piece;
    if (c.done) return "";
    const std::string &s = c.raw;
    if (c.at == std::string::npos) {   // the start of the "text" value: "text" : "
        const size_t k = s.find("\"text\"");
        if (k == std::string::npos) return "";
        size_t i = k + 6;
        while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i >= s.size()) return "";
        if (s[i] != ':') { c.done = true; return ""; }
        ++i;
        while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i >= s.size()) return "";
        if (s[i] != '"') { c.done = true; return ""; }
        c.at = c.cursor = i + 1;
    }
    std::string out;
    while (c.cursor < s.size()) {
        const char ch = s[c.cursor];
        if (ch == '"') { c.done = true; break; }
        if (ch != '\\') { out += ch; ++c.cursor; continue; }
        if (c.cursor + 1 >= s.size()) break;   // the escape's other half is still to come
        const char e = s[c.cursor + 1];
        if (e != 'u') {
            out += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' || e == 'f' ? ' ' : e;
            c.cursor += 2;
            continue;
        }
        if (c.cursor + 6 > s.size()) break;
        int cp = hex4(s, c.cursor + 2);
        size_t used = 6;
        if (cp >= 0xD800 && cp <= 0xDBFF) {   // a pair of escapes for one character beyond the first plane
            if (c.cursor + 12 > s.size()) break;
            const int lo = s[c.cursor + 6] == '\\' && s[c.cursor + 7] == 'u' ? hex4(s, c.cursor + 8) : -1;
            if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); used = 12; }
        }
        if (cp >= 0) append_utf8(out, static_cast<unsigned>(cp));
        c.cursor += used;
    }
    c.text += out;
    return out;
}

int ReplyCalls::finish(std::vector<llm::ToolCall> &calls, std::string &unstreamed) {
    if (!decide_required()) return 0;
    int n = 0;
    std::vector<llm::ToolCall> kept;
    for (size_t i = 0; i < calls.size(); ++i) {
        if (calls[i].name != "reply") { kept.push_back(calls[i]); continue; }
        ++n;
        const std::string streamed = i < calls_.size() ? calls_[i].text : "";
        // The whole value from the complete arguments, for whatever the streaming decoder could not read.
        const json a = json::parse(util::trim(calls[i].args).empty() ? "{}" : calls[i].args, nullptr, false);
        const std::string full = a.is_object() && a.contains("text") && a["text"].is_string() ? a["text"].get<std::string>() : streamed;
        if (full.size() > streamed.size() && full.compare(0, streamed.size(), streamed) == 0) unstreamed += full.substr(streamed.size());
        else if (streamed.empty()) unstreamed += full;
    }
    calls.swap(kept);
    return n;
}

// ---------------------------------------------------------------- E3: the parallel check

namespace {

// Added to the newest user message of a copy of the deciding call's messages: the same system prompt,
// tools and conversation, so the service answers it from the same cached prefix.
const char *const kCheckNote =
    "(Automatic check, not from the user.) Do not answer the message above. Say only whether answering it needs one "
    "of your tools. It does if the answer would do, save, set, add, move, cancel, play, stop, switch or change "
    "anything; needs live or changeable information (weather, a share price, an exchange rate or converted amount, "
    "the time in another place, opening hours, prices, news, results, anything recent); counts days, finds the weekday "
    "of a date or does arithmetic beyond one easy step; is about something the user is looking at or holding; or says "
    "what is or is not in their calendar, reminders, timers or music library. Reply with the single word TOOL if it "
    "does, or if you are unsure. Reply with the single word NONE only when no tool applies: conversation, settled "
    "general knowledge, translations, the current local time, facts you remember about the user, a question asking "
    "for a missing detail, or saying you cannot do something none of your tools does.";

}  // namespace

void ParallelCheck::start(json messages, const json &tools, const std::string &main_model) {
    if (started_ || !messages.is_array() || messages.empty()) return;
    started_ = true;
    start_ = Clock::now();
    json &last = messages.back();
    if (util::jget(last, "role", std::string()) == "user" && last["content"].is_string())
        last["content"] = last["content"].get<std::string>() + "\n\n" + kCheckNote;
    else
        messages.push_back({{"role", "user"}, {"content", kCheckNote}});
    llm::ChatOptions opt;
    opt.model = get().check_model.empty() ? main_model : get().check_model;
    opt.fast = true;
    opt.max_tokens = 16;      // one word; a tool call cut short still names its tool
    opt.temperature = 0;
    opt.cancel = &cancel_;
    opt.role = get().check_model.empty() ? "main" : "check";
    opt.purpose = "parallel_check";
    thread_ = std::thread([this, messages = std::move(messages), tools, opt] {
        const llm::ChatResult r = llm::chat(messages, tools, opt);
        int v = kUnclear;
        if (r.cancelled) v = kCancelled;
        else if (!r.error.empty()) v = kFailed;
        else if (!r.tool_calls.empty()) v = kTool;
        else {
            std::string word;
            for (char c : r.content)
                if (isalpha(static_cast<unsigned char>(c))) word += static_cast<char>(toupper(static_cast<unsigned char>(c)));
                else if (!word.empty()) break;
            v = word == "NONE" ? kNone : word == "TOOL" ? kTool : kUnclear;
        }
        ms_ = ms_since(start_);
        verdict_ = v;
    });
}

bool ParallelCheck::wait_none(int max_ms) {
    if (!started_) return false;
    for (int waited = 0; verdict_ == kPending && waited < max_ms; waited += 5) usleep(5 * 1000);
    return verdict_ == kNone;
}

void ParallelCheck::stop() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
}

ParallelCheck::~ParallelCheck() { stop(); }

json ParallelCheck::summary() const {
    if (!started_) return nullptr;
    static const char *const names[] = {"pending", "none", "tool", "unclear", "failed", "cancelled"};
    return {{"verdict", names[verdict_.load()]}, {"ms", ms_.load()}, {"used", used_}};
}

// ---------------------------------------------------------------- one request

void RequestTrace::absorbing() {
    if (!tracing()) return;
    absorb_start_ = Clock::now();
    absorb_marked_ = true;
}

void RequestTrace::begin() {
    end();
    if (!tracing()) return;
    const long absorb = absorb_marked_ ? ms_since(absorb_start_) : -1;   // -1: absorbed while answering
    absorb_marked_ = false;
    int n;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        n = ++g_req;
        g_calls = 0;
        g_req_start = Clock::now();
        g_in_request = true;
    }
    ev_ = {{"ev", "request"}, {"req", n}, {"absorb_ms", absorb}, {"first_sentence_ms", -1}, {"sentences", 0},
           {"tools", json::array()}, {"guards", json::array()}, {"second_look", nullptr}, {"steered", 0}};
    open_ = true;
}

void RequestTrace::sentence(const std::string &text) {
    if (!open_) return;
    ev_["sentences"] = ev_["sentences"].get<int>() + 1;
    if (ev_["first_sentence_ms"].get<long>() >= 0) return;
    ev_["first_sentence_ms"] = request_ms();
    if (!get().tts_probe) return;
    // How long the first sentence takes to synthesise, measured beside the request rather than in
    // it: the result is discarded, nothing is played.
    const int req = ev_["req"].get<int>();
    const long at = ev_["first_sentence_ms"].get<long>();
    ++g_probes;
    std::thread([text, req, at] {
        const auto t = Clock::now();
        std::string pcm, err;
        const bool ok = llm::synthesize(text, pcm, err);
        trace({{"ev", "tts"}, {"req", req}, {"after_ms", at}, {"tts_first_ms", ms_since(t)}, {"ok", ok},
               {"pcm_bytes", static_cast<long>(pcm.size())}});
        --g_probes;
    }).detach();
}

void RequestTrace::tool(const std::string &name, long ms, const std::string &result) {
    if (!open_) return;
    const char *outcome = tools::failed(result) ? "error" : result.compare(0, 7, "CONFIRM") == 0 ? "confirm" : "ok";
    ev_["tools"].push_back({{"name", name}, {"ms", ms}, {"outcome", outcome}, {"result_bytes", static_cast<long>(result.size())},
                            {"at_ms", request_ms()}});
}

void RequestTrace::guard(const char *which) {
    if (open_) ev_["guards"].push_back({{"guard", which}, {"at_ms", request_ms()}});
}

void RequestTrace::second_look(const std::string &outcome) {
    if (open_) ev_["second_look"] = outcome;
}

void RequestTrace::parallel_check(const json &summary) {
    if (open_ && !summary.is_null()) ev_["parallel_check"] = summary;
}

void RequestTrace::steered() {
    if (open_) ev_["steered"] = ev_["steered"].get<int>() + 1;
}

void RequestTrace::end() {
    if (!open_) return;
    open_ = false;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        ev_["done_ms"] = ms_since(g_req_start);
        ev_["calls"] = g_calls;
        g_in_request = false;
    }
    trace(ev_);
}

}  // namespace experiment

#endif  // ZB_EXPERIMENTS
