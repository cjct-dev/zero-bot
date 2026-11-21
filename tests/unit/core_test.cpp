// Device-free tests of the core that every thread shares (ZB-037, from the external review):
// files saved by two threads at once, the settings read and changed on several threads, and the
// calendar's all-day alerts on the days the clocks change.
//
// Built and run by tests/run_unit.sh with a state folder; everything it writes goes there (main()
// points HOME and the XDG folders at it).
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <functional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include <unistd.h>

#include "core/settings.h"
#include "core/store.h"
#include "core/upgrade.h"
#include "core/util.h"

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
    test_power_on_leftover(root);
    printf("core: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
