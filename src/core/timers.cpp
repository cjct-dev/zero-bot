#include "core/timers.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "core/result.h"
#include "platform/audio.h"

namespace timers {

namespace {

using util::jget;

constexpr long kJustEnded = 180;   // seconds: a timer that ended this recently is what "give it more time" means

std::atomic<time_t> g_fixed_clock{0};

using result::fail;

// Labels are the user's words: never let an odd byte in one make the result throw.
std::string reply(const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

std::string str_arg(const json &a, const char *key) { return util::trim(jget(a, key, std::string())); }

// Models sometimes send numbers as strings; accept both, text only when it is a number as a whole
// (util::to_number, as the tool runner reads one: an id of "1oops" is not timer 1).
bool num_arg(const json &a, const char *key, double &out) {
    auto it = a.find(key);
    if (it == a.end() || it->is_null()) return false;
    if (it->is_number()) { out = it->get<double>(); return std::isfinite(out); }
    if (it->is_string()) return util::to_number(it->get<std::string>(), out);
    return false;
}

bool bool_arg(const json &a, const char *key) {
    auto it = a.find(key);
    if (it == a.end()) return false;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_string()) return util::lower(it->get<std::string>()) == "true";
    return false;
}

std::string plural(long n, const char *unit) { return std::to_string(n) + " " + unit + (n == 1 ? "" : "s"); }

// The length asked for, in whole seconds: minutes and/or seconds (and hours, should a model send
// them). -1 when no length was given at all.
long length_arg(const json &a) {
    double v = 0, total = 0;
    bool given = false;
    if (num_arg(a, "hours", v)) { total += v * 3600; given = true; }
    if (num_arg(a, "minutes", v)) { total += v * 60; given = true; }
    if (num_arg(a, "seconds", v)) { total += v; given = true; }
    if (!given) return -1;
    if (!std::isfinite(total) || total < 0) return 0;
    return static_cast<long>(std::llround(std::min(total, 1e9)));
}

// When a timer ends, said the short way: "14:05", "tomorrow 01:15" or a date further off.
std::string end_text(time_t end, time_t now) {
    const std::string day = util::format_time(end, "%Y-%m-%d");
    if (day == util::format_time(now, "%Y-%m-%d")) return util::format_time(end, "%H:%M");
    if (day == util::format_time(util::add_local_days(now, 1), "%Y-%m-%d")) return "tomorrow " + util::format_time(end, "%H:%M");
    return util::short_date(end);
}

// Lower-case words of letters and digits: "Dad's" is "dads". Bytes of other scripts are kept.
std::vector<std::string> words_of(const std::string &text) {
    std::vector<std::string> out;
    std::string w;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (text.compare(i, 3, "\xE2\x80\x99") == 0) { i += 2; continue; }   // a typographic apostrophe
        if (isalnum(c) || (c & 0x80)) w += static_cast<char>(tolower(c));
        else if (c != '\'') {
            if (!w.empty()) out.push_back(w);
            w.clear();
        }
    }
    if (!w.empty()) out.push_back(w);
    return out;
}

template <size_t N> bool one_of(const std::string &w, const char *const (&list)[N]) {
    return std::find_if(std::begin(list), std::end(list), [&](const char *f) { return w == f; }) != std::end(list);
}

// The words that tell timers apart: without "the", "timer" and the like, numbers as digits.
std::vector<std::string> significant(const std::string &text) {
    static const char *const kFiller[] = {"the", "my", "a", "an", "timer", "timers", "one", "ones", "that",
                                          "this", "please", "for", "with", "left", "on"};
    static const char *const kUnits[] = {"second", "seconds", "sec", "secs", "minute", "minutes", "min", "mins",
                                         "hour", "hours", "hr", "hrs"};
    static const char *const kNumbers[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
                                           "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen",
                                           "seventeen", "eighteen", "nineteen", "twenty"};
    static const char *const kTens[] = {"twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};
    auto index_in = [](const std::string &w, const char *const *list, int n) {
        for (int k = 0; k < n; ++k)
            if (w == list[k]) return k;
        return -1;
    };
    std::vector<std::string> out;
    const std::vector<std::string> words = words_of(text);
    for (size_t i = 0; i < words.size(); ++i) {
        std::string w = words[i];
        const int tens = index_in(w, kTens, 8);
        // A tens word and a ones word after it are one number: "twenty five" and "thirty-five" are
        // 25 and 35, not 20 and 5 (which would pick the 5 minute timer for "twenty five minutes").
        const int ones = tens >= 0 && i + 1 < words.size() ? index_in(words[i + 1], kNumbers, 10) : -1;
        if (ones >= 1) {
            out.push_back(std::to_string((tens + 2) * 10 + ones));
            ++i;
            continue;
        }
        // "One" is a number before a unit ("the one minute timer") and a filler word otherwise
        // ("the oven one").
        if (w == "one" && i + 1 < words.size() && one_of(words[i + 1], kUnits)) w = "1";
        else if (one_of(w, kFiller)) continue;
        const int n = index_in(w, kNumbers, 21);
        if (n >= 0) w = std::to_string(n);
        else if (tens >= 0) w = std::to_string((tens + 2) * 10);
        out.push_back(w);
    }
    return out;
}

// Two words name the same thing when they are equal or one begins the other ("stud", "studying").
bool same_word(const std::string &a, const std::string &b) {
    if (a == b) return true;
    const std::string &shorter = a.size() < b.size() ? a : b, &longer = a.size() < b.size() ? b : a;
    return shorter.size() >= 3 && longer.compare(0, shorter.size(), shorter) == 0;
}

// A label no running timer has yet: a second "Pasta" becomes "Pasta 2", so the two can be told apart.
// Labels that differ only in words that tell no timer apart ("Pasta", "pasta timer") count as the
// same, since they would be one name to whoever picks a timer by its words.
std::string unique_label(const std::string &label, const std::vector<Reminder> &running) {
    auto taken = [&](const std::string &name) {
        const std::vector<std::string> words = significant(name);
        return std::any_of(running.begin(), running.end(), [&](const Reminder &r) {
            return util::lower(r.text) == util::lower(name) || significant(r.text) == words;
        });
    };
    if (!taken(label)) return label;
    for (int n = 2;; ++n)
        if (!taken(label + " " + std::to_string(n))) return label + " " + std::to_string(n);
}

json describe(const Reminder &r, time_t now) {
    json j = {{"id", r.id}, {"label", r.text}};
    const long left = static_cast<long>(r.due - now);
    if (left > 0) {
        j["time_left"] = length_words(left);
        j["ends_at"] = end_text(r.due, now);
    } else {
        j["time_left"] = "none";
        j["ended"] = true;
    }
    return j;
}

// "Study (id 3, 40 minutes left), Pasta (id 5, 4 minutes 10 seconds left)"
std::string summary(const std::vector<Reminder> &list, time_t now) {
    std::string out;
    for (auto &r : list)
        out += (out.empty() ? "" : ", ") + r.text + " (id " + std::to_string(r.id) + ", " +
               (r.due > now ? length_words(static_cast<long>(r.due - now)) + " left" : std::string("ended")) + ")";
    return out;
}

// Why nothing is running, with the timer that has just ended if there is one, so the model can
// tell the user it is already over.
std::string none_running(time_t now) {
    const auto recent = Store::get().recent_timers(now);
    if (recent.empty()) return "no timer is running";
    return "no timer is running; the last one, " + recent.front().text + ", ended at " + end_text(recent.front().due, now);
}

// The one running timer the arguments mean: by id, by words from its label, or the only one.
bool pick(const json &a, const std::vector<Reminder> &running, time_t now, Reminder &out, std::string &err) {
    double idv = 0;
    if (num_arg(a, "id", idv)) {
        for (auto &r : running)   // compared as numbers: 1e300 made into an int is undefined
            if (r.id == idv) { out = r; return true; }
        err = running.empty() ? none_running(now) : "no running timer has that id. Running: " + summary(running, now);
        return false;
    }
    if (running.empty()) { err = none_running(now); return false; }
    const std::string words = str_arg(a, "label");
    const std::vector<Reminder> found = match(running, words);
    if (found.size() == 1) { out = found[0]; return true; }
    if (found.empty()) {
        const std::vector<Reminder> ended = match(Store::get().recent_timers(now), words);
        if (!ended.empty())
            err = "the " + ended.front().text + " timer already ended at " + end_text(ended.front().due, now) +
                  ". Still running: " + summary(running, now);
        else
            err = "no running timer is called '" + words + "'. Running: " + summary(running, now) +
                  ". Ask the user which one they mean";
        return false;
    }
    err = "more than one timer could be meant: " + summary(found, now) +
          ". Ask the user which one, then call again with its label";
    return false;
}

// Both clocks, in milliseconds: the wall clock, and the time since boot with sleep included, which
// is real time passing (unlike the wall clock, it is never set).
void read_clocks(long long &wall_ms, long long &steady_ms) {
    timespec wall{}, steady{};
    clock_gettime(CLOCK_REALTIME, &wall);
    clock_gettime(CLOCK_BOOTTIME, &steady);
    auto ms = [](const timespec &t) { return static_cast<long long>(t.tv_sec) * 1000 + t.tv_nsec / 1000000; };
    wall_ms = ms(wall);
    steady_ms = ms(steady);
}

// ---------------------------------------------------------------- actions
std::string start(const json &a, time_t now, long long offset_ms) {
    const long seconds = length_arg(a);
    if (seconds < 0)
        return fail("give the length in minutes and/or seconds. If the user did not say how long, ask them");
    if (seconds < 1) return fail("a timer needs a length of at least one second");
    if (seconds > kMaxSeconds)
        return fail("a timer runs for at most 24 hours. For something further off, use reminder_set with the day and time");
    Store &store = Store::get();
    const std::vector<Reminder> running = store.timers();
    const std::string label = unique_label(make_label(str_arg(a, "label"), seconds), running);
    const int id = store.add_timer(label, seconds, now, offset_ms);
    if (!id)
        return fail(std::to_string(Store::kMaxTimers) + " timers are already running, which is the most at once. Tell the "
                    "user and offer to cancel one");
    json out = {{"started", {{"id", id}, {"label", label}, {"length", length_words(seconds)},
                             {"ends_at", end_text(now + seconds, now)}}}};
    if (!running.empty()) out["other_timers_running"] = running.size();
    return reply(out);
}

std::string cancel(const json &a, time_t now) {
    Store &store = Store::get();
    const std::vector<Reminder> running = store.timers();
    if (bool_arg(a, "all")) {
        if (running.empty()) return fail(none_running(now) + ", so there was nothing to cancel");
        store.clear_timers();
        json names = json::array();
        for (auto &r : running) names.push_back(r.text);
        return reply(json{{"cancelled", names}});
    }
    if (running.size() > 1 && significant(str_arg(a, "label")).empty() && !a.contains("id"))
        return fail(std::to_string(running.size()) + " timers are running: " + summary(running, now) +
                    ". Ask the user which one to cancel, or whether to cancel all of them (all: true)");
    Reminder t;
    std::string err;
    if (!pick(a, running, now, t, err)) return fail(err);
    if (!store.delete_timer(t.id)) return fail("the " + t.text + " timer has just ended");
    json out = {{"cancelled", json::array({t.text})}};
    json rest = json::array();
    for (auto &r : running)
        if (r.id != t.id) rest.push_back(r.text);
    if (!rest.empty()) out["still_running"] = rest;
    return reply(out);
}

std::string add_time(const json &a, time_t now, long long offset_ms) {
    const long extra = length_arg(a);
    if (extra <= 0) return fail("say how much time to add, in minutes and/or seconds");
    Store &store = Store::get();
    const std::vector<Reminder> running = store.timers();
    const std::vector<Reminder> recent = store.recent_timers(now);
    const std::string words = str_arg(a, "label");
    const std::vector<std::string> wanted = significant(words);
    // "Give it five more minutes", said just after an alarm, means the timer that rang: it starts
    // again for that long. So does naming one that has ended, unless a running timer fits the
    // name at least as well ("Pasta" has ended, "Pasta 2" runs: "pasta" means the first).
    auto exact = [&](const std::vector<Reminder> &list) {
        std::vector<Reminder> out;
        for (auto &r : list)
            if (significant(r.text) == wanted) out.push_back(r);
        return out;
    };
    Reminder again;
    bool restart = false;
    if (!a.contains("id")) {
        if (wanted.empty()) {
            if (!recent.empty() && now - recent.front().due <= kJustEnded) {
                again = recent.front();
                restart = true;
            }
        } else {
            const std::vector<Reminder> ended = match(recent, words), ended_exactly = exact(recent);
            if (!ended.empty() && (match(running, words).empty() || (exact(running).empty() && !ended_exactly.empty()))) {
                again = ended_exactly.empty() ? ended.front() : ended_exactly.front();   // the newest
                restart = true;
            }
        }
    }
    if (restart) {
        if (extra > kMaxSeconds) return fail("a timer runs for at most 24 hours");
        if (!store.restart_timer(again.id, extra, now, offset_ms))
            return fail(std::to_string(Store::kMaxTimers) + " timers are already running, which is the most at once");
        again.due = now + extra;
        return reply(json{{"restarted", describe(again, now)},
                         {"note", "it had already ended, so it now runs again for " + length_words(extra) + " from now"}});
    }
    Reminder t;
    std::string err;
    if (!pick(a, running, now, t, err)) return fail(err);
    if (t.due - now + extra > kMaxSeconds) return fail("a timer runs for at most 24 hours");
    if (!store.extend_timer(t.id, extra)) return fail("the " + t.text + " timer has just ended");
    t.due += extra;
    json out = describe(t, now);
    out["added"] = length_words(extra);
    return reply(json{{"extended", out}});
}

std::string list(time_t now) {
    Store &store = Store::get();
    json arr = json::array();
    for (auto &r : store.timers()) arr.push_back(describe(r, now));
    json out = {{"timers", arr}};
    if (arr.empty()) out["note"] = "no timer is running";
    json ended = json::array();
    for (auto &r : store.recent_timers(now)) ended.push_back({{"label", r.text}, {"ended_at", end_text(r.due, now)}});
    if (!ended.empty()) out["ended_recently"] = ended;
    return reply(out);
}

}  // namespace

// ---------------------------------------------------------------- text
std::string clock_text(long seconds) {
    if (seconds < 0) seconds = 0;
    char buf[32];
    if (seconds >= 3600) snprintf(buf, sizeof(buf), "%ld:%02ld:%02ld", seconds / 3600, seconds % 3600 / 60, seconds % 60);
    else snprintf(buf, sizeof(buf), "%ld:%02ld", seconds / 60, seconds % 60);
    return buf;
}

std::string badge(const std::vector<time_t> &ends, time_t now) {
    if (ends.empty()) return "";
    std::string out = clock_text(static_cast<long>(*std::min_element(ends.begin(), ends.end()) - now));
    if (ends.size() > 1) out += " +" + std::to_string(ends.size() - 1);
    return out;
}

std::string length_words(long s) {
    if (s < 0) s = 0;
    const long h = s / 3600, m = s % 3600 / 60, sec = s % 60;
    if (h) return plural(h, "hour") + (m ? " " + plural(m, "minute") : "");
    if (m) return plural(m, "minute") + (sec ? " " + plural(sec, "second") : "");
    return plural(sec, "second");
}

std::string make_label(const std::string &raw, long seconds) {
    // Text from the model: control characters out, spaces collapsed, quotes and a full stop
    // around it dropped, kept short enough for the banner.
    std::string s;
    for (char c : raw) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) c = ' ';
        if (c == ' ' && (s.empty() || s.back() == ' ')) continue;
        s += c;
    }
    s = util::trim(s);
    while (!s.empty() && (s.back() == '.' || s.back() == '"' || s.back() == '\'' || s.back() == ' ')) s.pop_back();
    while (!s.empty() && (s[0] == '"' || s[0] == '\'' || s[0] == ' ')) s.erase(0, 1);
    s = util::truncate_utf8(s, 48);
    if (!s.empty()) {
        if (s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
        return s;
    }
    const long h = seconds / 3600, m = seconds % 3600 / 60, sec = seconds % 60;
    if (seconds < 120 && sec) return std::to_string(seconds) + " second timer";
    std::string out;
    if (h) out += std::to_string(h) + " hour";
    if (m) out += (out.empty() ? "" : " ") + std::to_string(m) + " minute";
    if (sec) out += (out.empty() ? "" : " ") + std::to_string(sec) + " second";
    return out + " timer";
}

std::vector<Reminder> match(const std::vector<Reminder> &running, const std::string &words) {
    // A timer's whole name, as it appears in a result ("Pasta timer"), means that timer alone.
    const std::string whole = util::lower(util::trim(words));
    std::vector<Reminder> named;
    for (auto &r : running)
        if (!whole.empty() && util::lower(r.text) == whole) named.push_back(r);
    if (!named.empty()) return named;
    const std::vector<std::string> wanted = significant(words);
    if (wanted.empty()) return running;
    std::vector<int> scores;
    int best = 0;
    for (auto &r : running) {
        const std::vector<std::string> have = significant(r.text);
        int score = 0;
        for (auto &w : wanted)
            if (std::any_of(have.begin(), have.end(), [&](const std::string &h) { return same_word(w, h); })) ++score;
        scores.push_back(score);
        best = std::max(best, score);
    }
    std::vector<Reminder> out;
    if (best == 0) return out;
    for (size_t i = 0; i < running.size(); ++i)
        if (scores[i] == best) out.push_back(running[i]);
    if (out.size() > 1) {
        // "Study" means the timer called just that, not the one called "Study break".
        std::vector<Reminder> exact;
        for (auto &r : out)
            if (significant(r.text) == wanted) exact.push_back(r);
        if (exact.size() == 1) return exact;
    }
    return out;
}

// ---------------------------------------------------------------- the tool
std::string run_tool(const json &args, time_t now, long long offset_ms) {
    // The time as the timers reckon it. Read just as the wall clock was set, it can be on the other
    // side of the step from them (the main loop sees a step on its next tick): what is left and
    // what has just ended then still compare like with like, and a timer started now is added by
    // the clock the others go by, whichever that is by then.
    now = Store::get().reckon(now, offset_ms);
    const std::string action = util::lower(str_arg(args, "action"));
    if (action == "start") return start(args, now, offset_ms);
    if (action == "cancel") return cancel(args, now);
    if (action == "list") return list(now);
    if (action == "add_time") return add_time(args, now, offset_ms);
    return fail("action must be start, cancel, list or add_time");
}

std::string run_tool(const json &args) {
    long long wall = 0, steady = 0;
    read_clocks(wall, steady);
    // Just after the wall clock is set, the timers are on the old clock until the main loop moves
    // them on its next tick. Waiting for that keeps the clock times in the result ("ends at 14:05")
    // the ones on the clock; should the loop not come by (it is held up, or there is none), the
    // time is reckoned across the step instead.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kClockWaitMs);
    while (Store::get().clock_stepped(wall - steady) && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        read_clocks(wall, steady);
    }
    return run_tool(args, static_cast<time_t>(wall / 1000), wall - steady);
}

time_t now() {
    const time_t fixed = g_fixed_clock;
    return fixed ? fixed : time(nullptr);
}

void fix_clock(time_t t) { g_fixed_clock = t; }

time_t follow_clock() {
    long long wall = 0, steady = 0;
    read_clocks(wall, steady);
    Store::get().follow_clock(wall, steady);
    return static_cast<time_t>(wall / 1000);
}

unsigned long ms_to_next_second() {
    long long wall = 0, steady = 0;
    read_clocks(wall, steady);
    return static_cast<unsigned long>(1000 - wall % 1000);
}

// ---------------------------------------------------------------- the alarm
Alarm::Alarm(Sound sound) : sound_(std::move(sound)) {}

Alarm::~Alarm() { stop(); }

std::vector<Ended> Alarm::poll(time_t now, unsigned long now_ms, bool recording, bool quiet) {
    std::vector<Ended> out;
    if (recording) return out;
    Store &store = Store::get();
    const time_t next = store.next_timer_end();
    if (!next || now < next) return out;   // the usual case: one atomic read and nothing else
    bool rings = false;
    for (const Reminder &r : store.take_ended_timers(now)) {
        Ended e;
        e.id = r.id;
        e.label = r.text;
        e.end = r.due;
        // Only a timer carried over from an earlier run can have ended unseen, while the app was
        // closed. One set in this run rings however late its end is noticed (the microphone was
        // open for a long time): the app has been here all along.
        e.missed = r.carried && now - r.due >= kMissedAfter;
        if (!e.missed) {
            labels_.push_back(e.label);
            rings = true;
        }
        out.push_back(e);
    }
    if (rings) {
        until_ = now_ms + kRingMs;   // a timer joining a ring that is under way gets its full minute
        if (!ringing_) {
            ringing_ = true;
            audio::Music::get().hold(audio::Music::kAlarm, true);
        }
        if (!quiet) {
            // Each timer that rings starts the sound afresh, from its first burst: one that joins a
            // ring under way gets a whole minute of sound too, not what is left of the first one's.
            sounding_ = true;
            if (sound_.start) sound_.start();
        }
    }
    store.save_timers();             // the file, once the alarm is under way
    return out;
}

bool Alarm::tick(unsigned long now_ms) {
    if (!ringing_ || now_ms < until_) return false;
    stop();
    return true;
}

Alarm::Key Alarm::key(bool tab) {
    if (!ringing_) return Key::None;
    stop();
    return tab ? Key::Listen : Key::Silence;
}

void Alarm::stop() {
    if (sounding_) {
        sounding_ = false;
        if (sound_.stop) sound_.stop();
    }
    if (ringing_) {
        ringing_ = false;
        audio::Music::get().hold(audio::Music::kAlarm, false);
    }
    labels_.clear();
}

}  // namespace timers
