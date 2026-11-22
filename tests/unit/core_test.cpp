// Device-free tests of the core that every thread shares (ZB-037, from the external review):
// files saved by two threads at once, the settings read and changed on several threads, and the
// calendar's all-day alerts on the days the clocks change. Also the small shared helpers (ZB-035): the
// failure shape, the battery, the Wi-Fi list, the default route, the monotonic count and the start of
// a day, on known inputs.
//
// Built and run by tests/run_unit.sh with a state folder; everything it writes goes there (main()
// points HOME and the XDG folders at it).
#include <atomic>
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

#include "core/result.h"
#include "core/settings.h"
#include "core/store.h"
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
    test_all_day_alerts(root);
    test_store_writes_unlocked();
    test_power_on_leftover(root);
    const std::string helpers = root + "/helpers";
    util::make_dirs(helpers);
    test_result();
    test_battery(helpers);
    test_wifi_list();
    test_default_route(helpers);
    test_now_ms();
    test_start_of_day();
    printf("core: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
