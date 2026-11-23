// Device-free tests of the core that every thread shares (ZB-037, from the external review):
// files saved by two threads at once, the settings read and changed on several threads (and their list of
// web search services, read from an older file too), and the calendar's all-day alerts on the days the
// clocks change. Also the small shared helpers (ZB-035): the
// failure shape, the battery, the Wi-Fi list, the default route, the monotonic count and the start of
// a day, on known inputs; numeric dates spelt out for the model and dates typed in the calendar editor,
// in both date orders; when a calendar event comes round again; and the breathing exercise's timing.
//
// Built and run by tests/unit/run_unit.sh with a state folder; everything it writes goes there (main()
// points HOME and the XDG folders at it).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include <unistd.h>

#include "core/breath.h"
#include "core/result.h"
#include "core/settings.h"
#include "core/store.h"
#include "core/ui_queue.h"
#include "core/upgrade.h"
#include "core/util.h"
#include "platform/net_status.h"
#include "platform/power.h"
#include "platform/sys.h"

namespace {

int g_checks = 0, g_failures = 0;
const char *g_test = "";

void check(bool ok, const char *what, int line) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    fprintf(stderr, "FAIL %s, line %d: %s\n", g_test, line, what);
}

void check_eq(const std::string &got, const std::string &want, const char *what, int line) {
    ++g_checks;
    if (got == want) return;
    ++g_failures;
    fprintf(stderr, "FAIL %s, line %d: %s is \"%s\", expected \"%s\"\n", g_test, line, what, got.c_str(), want.c_str());
}

void check_eq(long long got, long long want, const char *what, int line) {
    ++g_checks;
    if (got == want) return;
    ++g_failures;
    fprintf(stderr, "FAIL %s, line %d: %s is %lld, expected %lld\n", g_test, line, what, got, want);
}

#define CHECK(cond) check((cond), #cond, __LINE__)
#define CHECK_EQ(got, want) check_eq((got), (want), #got, __LINE__)

std::vector<std::string> names_in(const std::string &dir) {
    std::vector<std::string> out;
    if (DIR *d = opendir(dir.c_str())) {
        while (const dirent *e = readdir(d)) {
            const std::string name = e->d_name;
            if (name != "." && name != "..") out.push_back(name);
        }
        closedir(d);
    }
    return out;
}

// fsync, held up on request (-Wl,--wrap=fsync in run_unit.sh): a file then takes as long to write
// as on a slow card.
std::atomic<int> g_fsync_delay_ms{0};
std::atomic<int> g_fsyncs_under_way{0};

}  // namespace

extern "C" int __real_fsync(int fd);
extern "C" int __wrap_fsync(int fd) {
    ++g_fsyncs_under_way;
    if (const int ms = g_fsync_delay_ms) usleep(static_cast<useconds_t>(ms) * 1000);
    const int r = __real_fsync(fd);
    --g_fsyncs_under_way;
    return r;
}

namespace {

// ---------------------------------------------------------------- one file saved by two threads
// Both threads once wrote the same NAME.tmp: one could rename the other's half-written copy into
// place, and the other's rename then failed. Each save now has a temporary file of its own.
void test_atomic_writes(const std::string &root) {
    g_test = "two threads saving one file";
    const std::string dir = root + "/race", path = dir + "/file.json";
    const std::string a(300000, 'a'), b(200000, 'b');   // large enough for the writes to overlap
    std::atomic<int> failed{0}, torn{0};
    std::atomic<bool> done{false};
    auto writer = [&](const std::string *data) {
        for (int i = 0; i < 120; ++i)
            if (!util::write_file_atomic(path, *data)) ++failed;
    };
    std::thread reader([&] {
        while (!done) {
            std::string got;
            if (util::read_file(path, got) && got != a && got != b) ++torn;
        }
    });
    std::thread t1(writer, &a), t2(writer, &b);
    t1.join();
    t2.join();
    done = true;
    reader.join();
    CHECK_EQ(failed.load(), 0);
    CHECK_EQ(torn.load(), 0);
    std::string last;
    CHECK(util::read_file(path, last) && (last == a || last == b));
    const std::vector<std::string> left = names_in(dir);
    CHECK_EQ(static_cast<long long>(left.size()), 1);   // no temporary file is left behind
    CHECK(left.size() == 1 && left[0] == "file.json");

    // A save that cannot be put in place leaves no temporary file behind.
    const std::string blocked = dir + "/blocked";
    util::make_dirs(blocked + "/file.json");   // a folder where the file should go: rename fails
    CHECK(!util::write_file_atomic(blocked + "/file.json", "new"));
    const std::vector<std::string> after = names_in(blocked);
    CHECK(after.size() == 1 && after[0] == "file.json");
}

// ---------------------------------------------------------------- settings on several threads
// The UI thread and the agent's tools both change the settings, and every thread reads them. A
// reader gets a whole snapshot, no change is lost, and the file ends with the newest settings.
void test_settings_threads(const std::string &root) {
    g_test = "settings shared between threads";
    // What get() hands out is a copy that cannot be written to: a change goes through update().
    static_assert(std::is_const<decltype(Settings::get())>::value, "Settings::get() returns a read-only snapshot");
    Settings::load();
    const std::string file = root + "/config/zero-bot/settings.json";

    // A snapshot stays as it was while it is used, whatever changes meanwhile.
    const Settings before = Settings::get();
    const Settings after = Settings::update([](Settings &s) { s.volume = s.volume == 33 ? 34 : 33; });
    CHECK(Settings::get().volume == after.volume && before.volume != after.volume);

    const int gain0 = Settings::get().mic_gain;
    std::atomic<bool> done{false};
    std::atomic<long> torn{0}, reads{0};
    auto reader = [&] {
        while (!done) {
            const Settings s = Settings::get();
            if (s.api_base != s.api_key || s.user_name != s.home_city) ++torn;
            ++reads;
        }
    };
    // Strings of changing length, so that a change reallocates while another thread copies.
    auto value = [](char c, int i) { return std::string(static_cast<size_t>(i % 9) * 9, c) + std::to_string(i); };
    std::thread r1(reader), r2(reader);
    std::thread agent([&] {
        for (int i = 0; i < 3000; ++i)
            Settings::update([&, i](Settings &s) { s.api_base = s.api_key = value('k', i); s.mic_gain += 1; }, i % 100 == 0);
    });
    std::thread ui([&] {
        for (int i = 0; i < 3000; ++i)
            Settings::update([&, i](Settings &s) { s.user_name = s.home_city = value('c', i); s.mic_gain += 1; }, i % 100 == 0);
    });
    agent.join();
    ui.join();
    done = true;
    r1.join();
    r2.join();
    CHECK(reads.load() > 0);
    CHECK_EQ(torn.load(), 0);
    CHECK_EQ(Settings::get().mic_gain, gain0 + 6000);   // every change counted

    // Saves made at the same time end with the newest settings on disk.
    std::thread s1([] { for (int i = 0; i < 150; ++i) Settings::update([](Settings &s) { s.brightness = s.brightness % 90 + 10; }); });
    std::thread s2([] { for (int i = 0; i < 150; ++i) Settings::update([](Settings &s) { s.screen_off_min = s.screen_off_min % 30 + 1; }); });
    s1.join();
    s2.join();
    const json disk = util::load_json(file, json());
    const json now = Settings::get().to_json();
    CHECK(disk == now);
    CHECK_EQ(static_cast<long long>(names_in(root + "/config/zero-bot").size()), 1);   // settings.json alone

    // A change kept back from the disk (the volume keys save after a pause) is written by save().
    const int gain = Settings::update([](Settings &s) { s.mic_gain = 120; }, false).mic_gain;
    CHECK(util::load_json(file, json()).value("mic_gain", 0) != gain);
    Settings::save();
    CHECK_EQ(util::load_json(file, json()).value("mic_gain", 0), 120);

    // Loading again reads what was saved.
    Settings::update([](Settings &s) { s.user_name = "Saved"; });
    Settings::update([](Settings &s) { s.user_name = "Not saved"; }, false);
    Settings::load();
    CHECK_EQ(Settings::get().user_name, "Saved");
}

// ---------------------------------------------------------------- web search services in the settings
// The services are a list (agent/search/). An older file's search_key, always a Tavily key, is read as one
// Tavily entry and saved as the list; an entry of a kind a newer version wrote is kept whole, its fields
// included; ZB_SEARCH_KEY is the first entry's key; every key is a secret.
void test_search_settings(const std::string &root) {
    g_test = "web search services in the settings";
    const std::string file = root + "/config/zero-bot/settings.json";
    auto load = [&](const json &j) {
        util::save_json(file, j);
        Settings::load();
        return Settings::get();
    };
    const json before = util::load_json(file, json::object());
    Settings s = load({{"search_key", "tvly-old"}, {"api_key", "sk-1"}});
    CHECK(s.search.size() == 1 && s.search[0].kind == "tavily" && s.search[0].key == "tvly-old" && s.search[0].enabled);
    CHECK_EQ(s.search_key("tavily"), "tvly-old");
    // What load() read counts as on disk already, so save() alone writes nothing: a change does.
    const auto resave = [] { Settings::update([](Settings &) {}); };
    resave();
    json disk = util::load_json(file, json());
    CHECK(!disk.contains("search_key"));
    CHECK(disk.value("search", json()) == json::parse(R"([{"kind": "tavily", "key": "tvly-old", "enabled": true}])"));
    CHECK(load({{"search_key", ""}}).search.empty());
    CHECK(load(json::object()).search.empty());

    // The list wins over a search_key beside it; entries keep their order, a newer version's whole.
    const json list = json::parse(R"([{"kind": "later", "key": "lt-1", "monthly": 5, "region": {"x": 1}},
                                      {"kind": "tavily", "key": "tvly-2", "enabled": false}, 7, {"kind": "tavily", "key": "tvly-3"}])");
    s = load({{"search", list}, {"search_key", "tvly-old"}});
    CHECK(s.search.size() == 3 && s.search[0].kind == "later" && s.search[1].key == "tvly-2" && !s.search[1].enabled &&
          s.search[2].key == "tvly-3" && s.search[2].enabled);
    CHECK_EQ(s.search_key("tavily"), "tvly-2");   // the first entry of the kind
    resave();
    disk = util::load_json(file, json());
    CHECK(disk["search"][0] == json::parse(R"({"kind": "later", "key": "lt-1", "monthly": 5, "region": {"x": 1}, "enabled": true})"));
    CHECK(disk["search"].size() == 3 && !disk.contains("search_key"));

    // Setting a key changes the first entry of its kind, or adds one at the end; an empty key adds nothing.
    s.set_search_key("tavily", "tvly-new");
    CHECK(s.search[1].key == "tvly-new" && s.search[2].key == "tvly-3");
    s.set_search_key("other", "");
    CHECK_EQ(static_cast<long long>(s.search.size()), 3);
    s.set_search_key("other", "o-1");
    CHECK(s.search.size() == 4 && s.search[3].kind == "other" && s.search[3].key == "o-1" && s.search[3].enabled);
    s.set_search_key("tavily", "");
    CHECK_EQ(s.search_key("tavily"), "");

    // Every key set is a secret: the AI service's and each service's, whatever its kind.
    s.api_key = "sk-1";
    CHECK((s.secret_values() == std::vector<std::string>{"sk-1", "lt-1", "tvly-3", "o-1"}));

    // ZB_SEARCH_KEY is the first entry's key, or makes a Tavily entry when there is none.
    setenv("ZB_SEARCH_KEY", "env-key", 1);
    s = load({{"search", list}});
    CHECK(s.search[0].kind == "later" && s.search[0].key == "env-key" && s.search[1].key == "tvly-2");
    s = load(json::object());
    CHECK(s.search.size() == 1 && s.search[0].kind == "tavily" && s.search[0].key == "env-key");
    unsetenv("ZB_SEARCH_KEY");
    load(before);
}

// ---------------------------------------------------------------- all-day alerts and clock changes
// All-day alerts are measured from 09:00 on the day. Built by adding nine hours to midnight they
// came at 10:00 on the day the clocks go forward and 08:00 on the day they go back; "tomorrow" was
// "now plus 24 hours", which is the day after tomorrow late on the evening before a 23-hour day.
void test_all_day_alerts(const std::string &root) {
    g_test = "all-day alerts when the clocks change";
    util::apply_timezone("Europe/London");
    if (util::format_time(util::make_local(2027, 7, 1, 12, 0), "%Z") != "BST") {
        fprintf(stderr, "(no zone database here: the clock-change checks were skipped)\n");
        return;
    }
    auto hhmm = [](time_t t) { return util::format_time(t, "%Y-%m-%d %H:%M"); };
    CalEvent day;
    day.all_day = true;
    for (int d : {28, 31}) {   // 28 March 2027: clocks forward; 31 October 2027: clocks back
        day.start = util::make_local(2027, d == 28 ? 3 : 10, d, 0, 0);
        const std::string want = d == 28 ? "2027-03-28 09:00" : "2027-10-31 09:00";
        CHECK_EQ(hhmm(alert_base(day, day.start)), want);
    }
    day.start = util::make_local(2027, 6, 10, 0, 0);
    CHECK_EQ(hhmm(alert_base(day, day.start)), "2027-06-10 09:00");

    Store &store = Store::get();
    store.load();
    store.clear_events();
    CalEvent clocks;
    clocks.title = "Clocks change";
    clocks.all_day = true;
    clocks.start = util::make_local(2027, 3, 28, 0, 0);
    clocks.remind_min = 0;   // on the day, at 09:00
    store.add_event(clocks);
    CHECK(store.take_due_alerts(util::make_local(2027, 3, 28, 8, 59)).empty());
    const auto on_the_day = store.take_due_alerts(util::make_local(2027, 3, 28, 9, 0));
    CHECK(on_the_day.size() == 1 && on_the_day[0] == "Clocks change today");

    store.clear_events();
    CalEvent dentist;
    dentist.title = "Dentist";
    dentist.all_day = true;
    dentist.start = util::make_local(2027, 3, 28, 0, 0);
    dentist.remind_min = 24 * 60;   // the day before, at 09:00
    store.add_event(dentist);
    const auto late = store.take_due_alerts(util::make_local(2027, 3, 27, 23, 30));
    CHECK(late.size() == 1 && late[0] == "Dentist tomorrow");
    store.clear_events();
    util::apply_timezone("");
    (void)root;
}

// ---------------------------------------------------------------- the store's lock, while a file is written
// The UI thread reads the store every second (take_due_alerts, timers) and waited for every file a tool
// saved, fsync and all, since the store wrote with its lock held (ZB-036). Now a change's file is written
// after the lock is let go, yet before the change returns; changes saved at once all reach the file.
void test_store_writes_unlocked() {
    g_test = "the store's files written outside its lock";
    Store &store = Store::get();
    store.load();
    store.clear_memories();
    const std::string file = util::data_dir() + "/memory.json";
    g_fsync_delay_ms = 300;
    std::thread saving([&store] { store.add_memory("Written slowly"); });
    while (g_fsyncs_under_way == 0) usleep(1000);
    const unsigned long t0 = sys::now_ms();
    const std::vector<Memory> seen = store.memories();
    store.take_due_alerts(time(nullptr));
    store.timers();
    const unsigned long waited = sys::now_ms() - t0;
    CHECK(waited < 100);                                    // 300 ms and more when the write held the lock
    CHECK(seen.size() == 1 && seen[0].text == "Written slowly");
    saving.join();
    CHECK_EQ(static_cast<long long>(util::load_json(file, json::array()).size()), 1);   // in the file once it returned

    g_fsync_delay_ms = 50;
    std::vector<std::thread> savers;
    for (int i = 0; i < 4; ++i) savers.emplace_back([&store, i] { store.add_memory("At once " + std::to_string(i)); });
    for (std::thread &t : savers) t.join();
    g_fsync_delay_ms = 0;
    CHECK_EQ(static_cast<long long>(util::load_json(file, json::array()).size()), 5);
    store.clear_memories();
    CHECK_EQ(static_cast<long long>(util::load_json(file, json::array()).size()), 0);
}

// ---------------------------------------------------------------- the main loop's sleep
// The main loop sleeps in uiq::wait until its next work (ZB-036): something posted, or a key (wake),
// ends the sleep at once; one that came just before the sleep is not lost; with neither, it lasts its time.
void test_ui_wake() {
    g_test = "the main loop's sleep and what wakes it";
    auto timed = [](void (*f)()) {
        const unsigned long t0 = sys::now_ms();
        f();
        return sys::now_ms() - t0;
    };
    CHECK(timed([] { uiq::wait(120); }) >= 110);
    std::thread poster([] { usleep(50 * 1000); uiq::post([] {}); });
    const unsigned long posted = timed([] { uiq::wait(2000); });
    poster.join();
    CHECK(posted >= 40 && posted < 500);
    CHECK_EQ(static_cast<long long>(uiq::drain()), 1);
    std::thread key([] { usleep(50 * 1000); uiq::wake(); });
    const unsigned long woke = timed([] { uiq::wait(2000); });
    key.join();
    CHECK(woke >= 40 && woke < 500);
    uiq::wake();                                         // before the sleep: it ends at once...
    CHECK(timed([] { uiq::wait(2000); }) < 100);
    CHECK(timed([] { uiq::wait(120); }) >= 110);         // ...and only that one
}

// ZB-085: "Start at power-on" is gone; the drop-in it wrote for the launcher is removed at start,
// but only when Zero Bot wrote it, and its folder only when nothing else is left in it.
void test_power_on_leftover(const std::string &root) {
    const std::string home = root + "/upgrade-home";
    const std::string dir = home + "/.config/systemd/user/APPLaunch.service.d";
    const std::string ours = dir + "/zero-bot.conf";
    const std::string old_text = "# Written by Zero Bot (Settings > Start at power-on).\n[Service]\n"
                                 "ExecStartPre=-/usr/bin/zero_bot --boot\nTimeoutStartSec=infinity\n";
    auto exists = [](const std::string &p) { return access(p.c_str(), F_OK) == 0; };
    CHECK(!upgrade::remove_power_on_dropin(home));                   // nothing there: nothing to do
    CHECK(!upgrade::remove_power_on_dropin(""));
    util::make_dirs(dir);
    CHECK(util::write_file_atomic(ours, old_text, 0644));
    CHECK(upgrade::remove_power_on_dropin(home));                    // ours, alone: file and folder go
    CHECK(!exists(ours) && !exists(dir));
    CHECK(exists(home + "/.config/systemd/user"));
    CHECK(!upgrade::remove_power_on_dropin(home));                   // once
    util::make_dirs(dir);
    CHECK(util::write_file_atomic(ours, old_text, 0644));
    CHECK(util::write_file_atomic(dir + "/other.conf", "[Service]\nNice=5\n", 0644));
    CHECK(upgrade::remove_power_on_dropin(home));                    // ours goes; the folder stays for the other
    CHECK(!exists(ours) && exists(dir + "/other.conf"));
    const std::string foreign = "[Service]\n# Written by Zero Bot, said the second line\nNice=5\n";
    CHECK(util::write_file_atomic(ours, foreign, 0644));
    CHECK(!upgrade::remove_power_on_dropin(home));                   // a file of that name someone else wrote
    std::string kept;
    CHECK(util::read_file(ours, kept) && kept == foreign);
    CHECK(util::write_file_atomic(ours, "", 0644));
    CHECK(!upgrade::remove_power_on_dropin(home) && exists(ours));   // an empty one too
}

// ---------------------------------------------------------------- the shared helpers
void write_text(const std::string &path, const std::string &text) { std::ofstream(path) << text; }

void test_result() {
    g_test = "the failure shape";
    CHECK_EQ(result::fail("give the length"), "ERROR: give the length");
    CHECK_EQ(result::fail(""), "ERROR: ");
    for (const char *yes : {"ERROR: no", "ERROR:no", "ERROR", "ERRORS", "{\"error\""}) CHECK(result::failed(yes));
    for (const char *no : {"", "error: x", "ERR", " ERROR: x", "{\"ok\":1}"}) CHECK(!result::failed(no));
    // ZB-090: a JSON object holding "error" is a failure, wherever the key is and whatever its value.
    // Before, only text beginning {"error" counted (9 characters were compared with an 8-character
    // prefix), so {"error": 5} and {"error": ""}, which tools::run's one_shape() leaves as they are,
    // passed as successes.
    for (const char *yes : {"{\"error\": \"x\"}", "{\"error\":5}", "{\"error\": \"\"}", "{ \"error\": 1}",
                            "{\"done\": [1], \"error\": null}", "{\n  \"error\": {\"code\": 3}\n}"})
        CHECK(result::failed(yes));
    // Not an object holding "error": the word as a value, a longer key, a nested key, a list, text that
    // only looks like JSON.
    for (const char *no : {"{\"errors\": [1]}", "{\"note\": \"error\"}", "{\"x\": {\"error\": 1}}", "[\"error\"]",
                           "{\"error", "{x \"error\": 1}", " {\"error\": 1}"})
        CHECK(!result::failed(no));
    // The key as JSON decodes it, however it is written: an escaped letter once passed as a success, because only
    // text holding the bytes "error" was parsed. Not the word as a value, or a key that decodes to another word.
    for (const char *yes : {"{\"err\\u006fr\": \"x\"}", "{\"\\u0065\\u0072\\u0072\\u006F\\u0072\": 5}", "{\"ok\": 1, \"erro\\u0072\": null}"})
        CHECK(result::failed(yes));
    for (const char *no : {"{\"note\": \"err\\u006fr\"}", "{\"err\\u006frs\": 1}", "{\"err\\u006fr\": 1"})
        CHECK(!result::failed(no));
    nlohmann::json decoded;   // decoded once: what the caller gets to shape it from
    CHECK(result::failed("{\"err\\u006fr\": \"disk full\", \"n\": 2}", &decoded) && decoded.is_object() &&
          decoded.value("error", "") == "disk full" && decoded.value("n", 0) == 2);
    CHECK(result::failed("{\"error\": 5", &decoded) && decoded.is_discarded());   // begins like one, is no JSON
    CHECK_EQ(result::words("ERROR: no such timer"), "no such timer");
    CHECK_EQ(result::words("ERROR:  two"), " two");
    CHECK_EQ(result::words("ERROR:no"), "ERROR:no");          // only the exact prefix goes
    CHECK_EQ(result::words("fine"), "fine");
}

void test_battery(const std::string &root) {
    g_test = "the battery";
    struct Gauge { const char *name, *capacity, *status; int percent; bool charging; };
    const Gauge gauges[] = {{"charging", "85\n", "Charging\n", 85, true},    {"discharging", "40\n", "Discharging\n", 40, false},
                            {"full", "100\n", "Full\n", 100, false},         {"garbage", "abc\n", "Charging\n", -1, true},
                            {"empty", "", "", -1, false},                    {"no-status", "12\n", nullptr, 12, false},
                            {"none", nullptr, nullptr, -1, false},           {"lower-case", "50", "charging", 50, false}};
    for (const Gauge &g : gauges) {
        const std::string dir = root + "/gauge-" + g.name;
        util::make_dirs(dir);
        if (g.capacity) write_text(dir + "/capacity", g.capacity);
        if (g.status) write_text(dir + "/status", g.status);
        const power::Battery b = power::battery(dir);
        CHECK_EQ(b.percent, g.percent);
        CHECK(b.charging == g.charging);
    }
}

void test_wifi_list() {
    g_test = "the Wi-Fi list";
    net_status::WiFi w = net_status::parse_wifi("no:Neighbours:40\nyes:Home net:72\nno:Cafe:30\n");
    CHECK(w.connected && w.has_signal);
    CHECK_EQ(w.entry, "Home net:72");
    CHECK_EQ(w.ssid, "Home net");
    CHECK_EQ(w.signal, "72");
    w = net_status::parse_wifi("yes:A:1\nyes:B:2\n");                // the last active row counts
    CHECK_EQ(w.ssid, "B");
    w = net_status::parse_wifi("yes:Only name\n");
    CHECK(w.connected && !w.has_signal);
    CHECK_EQ(w.ssid, "Only name");
    CHECK_EQ(w.signal, "");
    w = net_status::parse_wifi("yes:Home\\:net:72\n");              // nmcli escapes a ':' in the name; the entry keeps it
    CHECK_EQ(w.entry, "Home\\:net:72");
    for (const char *none : {"", "no:X:1\n", "yes\n", "YES:X:1\n"}) CHECK(!net_status::parse_wifi(none).connected);
}

void test_default_route(const std::string &root) {
    g_test = "the default route";
    const std::string H = "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n";
    struct Table { const char *name; std::string text; const char *link, *gw_iface, *gateway; };
    const Table tables[] = {
        {"wifi", H + "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n"
                     "wlan0\t0001A8C0\t00000000\t0001\t0\t0\t600\t00FFFFFF\t0\t0\t0\n", "wlan0", "wlan0", "192.168.1.1"},
        {"phone", H + "bnep0\t00000000\t012CA8C0\t0003\t0\t0\t750\t00000000\t0\t0\t0\n", "bnep0", "bnep0", "192.168.44.1"},
        {"two", H + "eth0\t00000000\t0100000A\t0003\t0\t0\t100\t00000000\t0\t0\t0\n"
                    "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n", "eth0", "eth0", "10.0.0.1"},
        // A VPN with no gateway first: the app is online through it (tether), but there is no router
        // behind it to identify the network by (discovery).
        {"vpn", H + "wg0\t00000000\t00000000\t0001\t0\t0\t50\t00000000\t0\t0\t0\n"
                    "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n", "wg0", "wlan0", "192.168.1.1"},
        {"ppp", H + "ppp0\t00000000\t00000000\t0001\t0\t0\t0\t00000000\t0\t0\t0\n", "ppp0", "", ""},
        {"none", H + "wlan0\t0001A8C0\t00000000\t0001\t0\t0\t600\t00FFFFFF\t0\t0\t0\n", "", "", ""},
        {"header", H, "", "", ""},
        {"empty", "", "", "", ""},
    };
    for (const Table &t : tables) {
        const std::string path = root + "/route-" + t.name;
        write_text(path, t.text);
        const net_status::Route link = net_status::default_route(false, path), gw = net_status::default_route(true, path);
        CHECK_EQ(link.iface, t.link);
        CHECK_EQ(link.gateway, "");
        CHECK_EQ(gw.iface, t.gw_iface);
        CHECK_EQ(gw.gateway, t.gateway);
    }
    CHECK_EQ(net_status::default_route(false, root + "/no-such-table").iface, "");
    CHECK_EQ(net_status::default_route(true, root + "/no-such-table").gateway, "");
}

void test_now_ms() {
    g_test = "the monotonic count";
    const unsigned long a = sys::now_ms();
    usleep(20 * 1000);
    const unsigned long b = sys::now_ms();
    CHECK(b >= a + 15 && b < a + 2000);
}

void test_start_of_day() {
    g_test = "the start of a day";
    struct Zone { const char *tz; std::vector<std::string> days; };
    const Zone zones[] = {{"Europe/London", {"2026-03-29", "2026-10-25", "2026-06-01"}},
                          {"America/New_York", {"2026-03-08", "2026-11-01"}},
                          {"Australia/Sydney", {"2026-04-05", "2026-10-04"}},
                          {"America/Santiago", {"2026-04-05", "2026-09-06"}}};   // its clocks change at midnight
    int times = 0;
    for (const Zone &z : zones) {
        util::apply_timezone(z.tz);
        for (const std::string &d : z.days)
            for (int minutes : {0, 59, 90, 150, 181, 720, 1439}) {
                time_t t = 0;
                char text[32];
                snprintf(text, sizeof(text), "%s %02d:%02d", d.c_str(), minutes / 60, minutes % 60);
                if (!util::parse_local(text, t)) continue;
                const time_t s = util::start_of_day(t);
                CHECK(s <= t && t - s < 25 * 3600);
                CHECK_EQ(util::format_time(s, "%Y-%m-%d"), util::format_time(t, "%Y-%m-%d"));
                CHECK(util::format_time(s - 1, "%Y-%m-%d") != util::format_time(s, "%Y-%m-%d"));   // the first second of the day
                ++times;
            }
    }
    CHECK(times >= 60);
    util::apply_timezone("");
}

// ---------------------------------------------------------------- dates said, typed and repeated
// A day and month given without a year mean their next time round (util.cpp year_meant): noon of that day.
time_t next_noon(int month, int day) {
    const util::LocalTime now = util::local_fields(time(nullptr));
    const bool gone = now.month > month || (now.month == month && now.day > day);
    return util::make_local(now.year + (gone ? 1 : 0), month, day, 12, 0);
}

// The text with each "{M/D}" replaced by that day's date as spell_numeric_dates writes it in the order set.
std::string with_dates(std::string text) {
    for (size_t at; (at = text.find('{')) != std::string::npos;) {
        int month = 0, day = 0;
        sscanf(text.c_str() + at + 1, "%d/%d", &month, &day);
        const char *form = util::month_first() ? "%A, %B %-d, %Y" : "%A %-d %B %Y";
        text.replace(at, text.find('}', at) - at + 1, util::format_time(next_noon(month, day), form));
    }
    return text;
}

// Numeric dates are spelt out for the model (AGENTS.md section 4), in the user's date order; a number that
// only looks like a date is left alone.
void test_spelt_dates() {
    g_test = "numeric dates spelt out";
    util::apply_timezone("Europe/London");
    const struct { const char *said, *day_first, *month_first; } cases[] = {
        {"put the dentist in my calendar on 3/4 at 10am", "put the dentist in my calendar on 3/4 ({4/3}) at 10am",
         "put the dentist in my calendar on 3/4 ({3/4}) at 10am"},
        {"dentist 3/4 at 10", "dentist 3/4 ({4/3}) at 10", "dentist 3/4 ({3/4}) at 10"},
        {"on 3/4.", "on 3/4 ({4/3}).", "on 3/4 ({3/4})."},
        {"flight on 25/12", "flight on 25/12 ({12/25})", "flight on 25/12 ({12/25})"},   // only one reading is a date
        {"from 1/12 to 5/12", "from 1/12 ({12/1}) to 5/12 ({12/5})", "from 1/12 ({1/12}) to 5/12 ({5/12})"},
        {"call on 17/11 and 16/11", "call on 17/11 ({11/17}) and 16/11 ({11/16})", "call on 17/11 ({11/17}) and 16/11 ({11/16})"},
        {"meeting 14.03.2027 with tom", "meeting 14.03.2027 (Sunday 14 March 2027) with tom",
         "meeting 14.03.2027 (Sunday, March 14, 2027) with tom"},
        {"pay by 5-6-27", "pay by 5-6-27 (Saturday 5 June 2027)", "pay by 5-6-27 (Thursday, May 6, 2027)"},
    };
    for (const bool month_first : {false, true}) {
        util::set_month_first(month_first);
        for (const auto &c : cases) CHECK_EQ(util::spell_numeric_dates(c.said), with_dates(month_first ? c.month_first : c.day_first));
        // A fraction, "24/7", a decimal, a version, a date already plain, a day that does not exist, an address.
        for (const char *same : {"add 3/4 cup of flour", "is the shop open 24/7", "what is 3.4 times 2", "version 1.2.3 is out",
                                 "remind me on 2027-03-04", "book for 31/2", "ip 10.0.0.1/24 and 1/2/3/4"})
            CHECK_EQ(util::spell_numeric_dates(same), same);
    }
    util::set_month_first(false);
    util::apply_timezone("");
}

// A date typed in the calendar editor (util::parse_typed), in each order, and how it is shown and edited.
void test_typed_dates() {
    g_test = "typed dates";
    util::apply_timezone("Europe/London");
    auto read = [](const char *typed) {
        time_t t = 0;
        return util::parse_typed(typed, t) ? util::format_time(t, "%Y-%m-%d %H:%M") : std::string("rejected");
    };
    const std::string april_3 = util::format_time(next_noon(4, 3), "%Y-%m-%d"), march_4 = util::format_time(next_noon(3, 4), "%Y-%m-%d");
    util::set_month_first(false);
    CHECK_EQ(util::typed_date_hint(), "Day/month/year hour:minute, 24-hour clock");
    CHECK_EQ(read("14/03/2027 18:30"), "2027-03-14 18:30");
    CHECK_EQ(read("03.04.27 9"), "2027-04-03 09:00");            // a two-digit year, an hour alone
    CHECK_EQ(read("4-3-2027 07:05"), "2027-03-04 07:05");
    CHECK_EQ(read("3/4 10:00"), april_3 + " 10:00");               // no year: its next time round
    CHECK_EQ(read("3/4"), april_3 + " 00:00");
    CHECK_EQ(read("25/12/2026"), "2026-12-25 00:00");
    time_t t = 0;
    CHECK(util::parse_typed("14/03/2027 18:30", t));
    CHECK_EQ(util::typed_date(t), "14/03/2027 18:30");
    CHECK_EQ(util::short_date(t), "Sun 14 Mar 18:30");
    util::set_month_first(true);
    CHECK_EQ(util::typed_date_hint(), "Month/day/year hour:minute, 24-hour clock");
    CHECK_EQ(read("03.04.27 9"), "2027-03-04 09:00");
    CHECK_EQ(read("4-3-2027 07:05"), "2027-04-03 07:05");
    CHECK_EQ(read("3/4 10:00"), march_4 + " 10:00");
    CHECK_EQ(util::typed_date(t), "03/14/2027 18:30");
    CHECK_EQ(util::short_date(t), "Sun Mar 14 18:30");
    for (const char *wrong : {"14/03/2027 18:30", "17/11", "25/12/2026"}) CHECK_EQ(read(wrong), "rejected");   // no 14th month
    for (const bool month_first : {false, true}) {
        util::set_month_first(month_first);
        CHECK_EQ(read("2027-03-14 18:30"), "2027-03-14 18:30");    // a year first is read the same in either order
        for (const char *wrong : {"31/02/2027", "13/13", "hello"}) CHECK_EQ(read(wrong), "rejected");
    }
    util::set_month_first(false);
    util::apply_timezone("");
}

// When a calendar event comes round again (next_occurrence): the 29th of February and the 31st in shorter
// months, the clock time kept across the clock changes, a one-off in the past never.
void test_next_occurrence() {
    g_test = "an event's next time";
    util::apply_timezone("Europe/London");
    auto at = [](const char *text) {
        time_t t = 0;
        util::parse_local(text, t);
        return t;
    };
    const struct { const char *start, *repeat, *from, *next; bool all_day; } cases[] = {
        {"1960-03-14", "yearly", "2026-11-17 10:00", "2027-03-14 00:00", true},
        {"1960-03-14", "yearly", "2027-03-14 00:00", "2027-03-14 00:00", true},   // the day itself
        {"1960-03-14", "yearly", "2027-03-14 00:01", "2028-03-14 00:00", true},
        {"2024-02-29", "yearly", "2026-11-17 10:00", "2027-02-28 00:00", true},
        {"2024-02-29", "yearly", "2027-03-01 10:00", "2028-02-29 00:00", true},
        {"2030-05-05", "yearly", "2026-11-17 10:00", "2030-05-05 00:00", true},   // not started yet
        {"2026-01-31 09:00", "monthly", "2026-02-01 00:00", "2026-02-28 09:00", false},
        {"2026-01-31 09:00", "monthly", "2026-11-17 10:00", "2026-11-30 09:00", false},
        {"2026-03-24 07:00", "weekly", "2026-03-27 00:00", "2026-03-31 07:00", false},   // clocks go forward 29 March
        {"2026-03-24 07:00", "weekly", "2026-11-17 06:00", "2026-11-17 07:00", false},
        {"2026-03-24 07:00", "weekly", "2026-11-17 08:00", "2026-11-24 07:00", false},
        {"2026-10-20 08:30", "daily", "2026-10-25 09:00", "2026-10-26 08:30", false},     // clocks go back 25 October
        {"2026-10-20 08:30", "daily", "2026-11-17 08:30", "2026-11-17 08:30", false},
        {"2026-12-01 10:00", "", "2026-11-17 10:00", "2026-12-01 10:00", false},
        {"2026-11-01 10:00", "", "2026-11-17 10:00", "never", false},
    };
    for (const auto &c : cases) {
        CalEvent e;
        e.start = at(c.start);
        e.repeat = c.repeat;
        e.all_day = c.all_day;
        const time_t next = next_occurrence(e, at(c.from));
        CHECK_EQ(next ? util::format_time(next, "%Y-%m-%d %H:%M") : "never", c.next);
    }
    util::apply_timezone("");
}

// ---------------------------------------------------------------- the breathing exercise's timing
void test_breathing() {
    g_test = "breathing timing";
    using namespace breath;
    const Pattern box = pattern("box");
    CHECK_EQ(box.cycle_ms(), 16000);
    CHECK_EQ(pattern("nonsense").id, "box");                     // an unknown pattern is box
    CHECK(std::string(pattern("calm").id) == "calm" && pattern("calm").cycle_ms() == 10000);   // in 4, out 6
    CHECK(clamp_minutes(5) == 5 && clamp_minutes(0) == kDefaultMinutes && clamp_minutes(99) == 20 && clamp_minutes(0.7) == 1);
    CHECK_EQ(clamp_minutes(NAN), kDefaultMinutes);
    const long total = session_ms(box, 5);
    CHECK_EQ(total, 304000);                                     // five minutes, ended on a whole breath
    CHECK_EQ(session_ms(pattern("calm"), 1), 60000);             // exactly six breaths
    Moment m = at(box, 0, total);
    CHECK(m.step == 0 && m.level == 0.f && m.seconds_left == 4 && std::string(m.label) == "Breathe in");
    m = at(box, 2000, total);
    CHECK(std::fabs(m.level - 0.5f) < 0.01f && m.seconds_left == 2);   // half way in, the orb half full
    m = at(box, 3999, total);
    CHECK(m.level > 0.99f && m.seconds_left == 1);
    m = at(box, 4000, total);
    CHECK(m.step == 1 && m.level == 1.f && std::string(m.label) == "Hold");
    m = at(box, 10000, total);
    CHECK(m.step == 2 && std::fabs(m.level - 0.5f) < 0.01f);      // half way out
    m = at(box, 15000, total);
    CHECK(m.step == 3 && m.level == 0.f);
    m = at(box, 16000, total);
    CHECK(m.step == 0 && m.level == 0.f);                        // the next cycle starts where the last ended
    // The orb never jumps between frames (15 a second), in the box pattern or the sighing one.
    for (const char *id : {"box", "sigh"}) {
        const Pattern p = pattern(id);
        float prev = at(p, 0, 600000).level, jump = 0;
        for (long t = 66; t < 32000; t += 66) {
            const float l = at(p, t, 600000).level;
            jump = std::max(jump, std::fabs(l - prev));
            prev = l;
        }
        check(jump <= (std::string(id) == "box" ? 0.04f : 0.06f), id, __LINE__);
    }
    m = at(box, total - 1, total);
    CHECK(!m.finished && m.remaining_ms == 1);                   // not finished until the last millisecond
    m = at(box, total, total);
    CHECK(m.finished && m.level == 0.f);                         // and finished empty, on an out-breath
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: core_test STATE_FOLDER\n"); return 2; }
    const std::string root = argv[1];
    util::make_dirs(root);
    setenv("HOME", root.c_str(), 1);
    setenv("XDG_CONFIG_HOME", (root + "/config").c_str(), 1);
    setenv("XDG_DATA_HOME", (root + "/data").c_str(), 1);
    setenv("XDG_CACHE_HOME", (root + "/cache").c_str(), 1);
    unsetenv("ZB_API_BASE");
    unsetenv("ZB_API_KEY");
    unsetenv("ZB_SEARCH_KEY");
    test_atomic_writes(root);
    test_settings_threads(root);
    test_search_settings(root);
    test_all_day_alerts(root);
    test_store_writes_unlocked();
    test_ui_wake();
    test_power_on_leftover(root);
    const std::string helpers = root + "/helpers";
    util::make_dirs(helpers);
    test_result();
    test_battery(helpers);
    test_wifi_list();
    test_default_route(helpers);
    test_now_ms();
    test_start_of_day();
    test_spelt_dates();
    test_typed_dates();
    test_next_occurrence();
    test_breathing();
    printf("core: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
