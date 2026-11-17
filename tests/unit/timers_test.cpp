// Device-free tests of the timers: the timer tool as the model sees it, the timers kept in the
// store (and old data files), the countdown text and the alarm (ringing, keys, the time limit,
// waiting while the microphone is open, holding the music). No screen, no sound, no network.
//
// Built and run by tests/run_unit.sh. Whoever runs it, everything it writes goes to a
// temporary folder of its own (main() points HOME and the XDG folders there), so the real
// calendar, memory and reminders are never touched.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "core/store.h"
#include "core/timers.h"
#include "core/util.h"
#include "platform/audio.h"

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

// 2027-01-15 08:00:00 UTC: a fixed "now", so that every result is the same on every run.
constexpr time_t T0 = 1800000000;

using timers::Alarm;

void reset_store() {
    for (const char *name : {"reminders.json", "calendar.json", "memory.json"})
        unlink((util::data_dir() + "/" + name).c_str());
    Store::get().load();
}

std::string tool(const std::string &args, time_t now) { return timers::run_tool(json::parse(args), now); }
bool is_error(const std::string &result) { return result.compare(0, 7, "ERROR: ") == 0; }
bool mentions(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }
// An "ERROR: ..." result is no JSON: an empty object then, so a wrong result fails its checks
// rather than throwing out of the test.
json parsed(const std::string &result) {
    json j = json::parse(result, nullptr, false);
    return j.is_discarded() ? json::object() : j;
}
std::string text(const json &j) { return j.is_string() ? j.get<std::string>() : j.dump(); }

int timer_id(const std::string &label) {
    for (auto &t : Store::get().timers())
        if (t.text == label) return t.id;
    return 0;
}

time_t timer_end(const std::string &label) {
    for (auto &t : Store::get().timers())
        if (t.text == label) return t.due;
    return 0;
}

Reminder timer(int id, const std::string &label) {
    Reminder r;
    r.id = id;
    r.text = label;
    r.kind = "timer";
    r.due = T0 + id * 60;
    return r;
}

// Stands in for the alarm's sound and counts what the alarm asks of it.
struct FakeSound {
    int starts = 0, stops = 0;
    std::function<void()> on_start;   // optional: a look at the world as the sound starts
    Alarm::Sound sound() {
        return {[this] { ++starts; if (on_start) on_start(); }, [this] { ++stops; }};
    }
};

// Whether reminders.json, as it is on disk now, has a record with this text.
bool file_lists(const std::string &text) {
    const json file = util::load_json(util::data_dir() + "/reminders.json", json::array());
    return std::any_of(file.begin(), file.end(), [&](const json &rec) { return rec.value("text", "") == text; });
}

long long ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count();
}

// ---------------------------------------------------------------- countdown text
void test_countdown_text() {
    CHECK_EQ(timers::clock_text(0), "0:00");
    CHECK_EQ(timers::clock_text(1), "0:01");
    CHECK_EQ(timers::clock_text(59), "0:59");
    CHECK_EQ(timers::clock_text(60), "1:00");
    CHECK_EQ(timers::clock_text(299), "4:59");
    CHECK_EQ(timers::clock_text(3581), "59:41");
    CHECK_EQ(timers::clock_text(3599), "59:59");
    CHECK_EQ(timers::clock_text(3600), "1:00:00");
    CHECK_EQ(timers::clock_text(3723), "1:02:03");
    CHECK_EQ(timers::clock_text(86400), "24:00:00");
    CHECK_EQ(timers::clock_text(-3), "0:00");

    // The status bar shows the soonest, and how many more run.
    CHECK_EQ(timers::badge({}, T0), "");
    CHECK_EQ(timers::badge({T0 + 299}, T0), "4:59");
    CHECK_EQ(timers::badge({T0 + 3600, T0 + 3599}, T0), "59:59 +1");
    CHECK_EQ(timers::badge({T0 + 7200, T0 + 3600, T0 + 5000}, T0), "1:00:00 +2");
    CHECK_EQ(timers::badge({T0 - 5}, T0), "0:00");
    // It changes when a whole second has passed, and only then.
    CHECK_EQ(timers::badge({T0 + 300}, T0), "5:00");
    CHECK_EQ(timers::badge({T0 + 300}, T0 + 1), "4:59");

    CHECK_EQ(timers::length_words(1), "1 second");
    CHECK_EQ(timers::length_words(45), "45 seconds");
    CHECK_EQ(timers::length_words(90), "1 minute 30 seconds");
    CHECK_EQ(timers::length_words(299), "4 minutes 59 seconds");
    CHECK_EQ(timers::length_words(3600), "1 hour");
    CHECK_EQ(timers::length_words(3723), "1 hour 2 minutes");

    CHECK_EQ(timers::make_label("food out of the oven", 300), "Food out of the oven");
    CHECK_EQ(timers::make_label("  \"Study.\" ", 3600), "Study");
    CHECK_EQ(timers::make_label("bad\x01label\nhere", 60), "Bad label here");
    CHECK_EQ(timers::make_label("", 300), "5 minute timer");
    CHECK_EQ(timers::make_label("", 90), "90 second timer");
    CHECK_EQ(timers::make_label("", 3600), "1 hour timer");
    CHECK_EQ(timers::make_label("", 5400), "1 hour 30 minute timer");
    CHECK(timers::make_label(std::string(200, 'x'), 60).size() <= 48 + 3);
}

// ---------------------------------------------------------------- the tool
void test_start_list_cancel_add_time() {
    reset_store();
    std::string r = tool(R"({"action":"start","minutes":5,"label":"food out of the oven"})", T0);
    json j = parsed(r);
    CHECK(!is_error(r));
    CHECK_EQ(text(j["started"]["label"]), "Food out of the oven");
    CHECK_EQ(text(j["started"]["length"]), "5 minutes");
    CHECK_EQ(text(j["started"]["ends_at"]), "08:05");            // the result says when it ends
    std::vector<Reminder> running = Store::get().timers();
    CHECK_EQ(running.size(), 1);
    CHECK_EQ(running[0].due, T0 + 300);
    CHECK_EQ(running[0].started, T0);
    CHECK_EQ(running[0].length, 300);

    // Seconds alone; minutes and seconds together; numbers sent as strings.
    CHECK(!is_error(tool(R"({"action":"start","seconds":90,"label":"Eggs"})", T0)));
    CHECK_EQ(timer_end("Eggs"), T0 + 90);
    j = parsed(tool(R"({"action":"start","minutes":"1","seconds":"40","label":"Tea"})", T0));
    CHECK_EQ(text(j["started"]["length"]), "1 minute 40 seconds");
    CHECK_EQ(timer_end("Tea"), T0 + 100);
    CHECK_EQ(j["other_timers_running"].get<int>(), 2);

    // list: soonest first, with the time left.
    j = parsed(tool(R"({"action":"list"})", T0 + 30));
    CHECK_EQ(j["timers"].size(), 3);
    CHECK_EQ(text(j["timers"][0]["label"]), "Eggs");
    CHECK_EQ(text(j["timers"][0]["time_left"]), "1 minute");
    CHECK_EQ(text(j["timers"][1]["label"]), "Tea");
    CHECK_EQ(text(j["timers"][2]["label"]), "Food out of the oven");
    CHECK_EQ(text(j["timers"][2]["time_left"]), "4 minutes 30 seconds");
    CHECK_EQ(text(j["timers"][2]["ends_at"]), "08:05");

    // add_time to one, picked by its label.
    j = parsed(tool(R"({"action":"add_time","minutes":2,"label":"eggs"})", T0 + 30));
    CHECK_EQ(text(j["extended"]["label"]), "Eggs");
    CHECK_EQ(text(j["extended"]["added"]), "2 minutes");
    CHECK_EQ(text(j["extended"]["time_left"]), "3 minutes");
    CHECK_EQ(timer_end("Eggs"), T0 + 210);
    CHECK_EQ(timer_end("Tea"), T0 + 100);                          // the others are untouched

    // cancel one by its label.
    j = parsed(tool(R"({"action":"cancel","label":"the tea timer"})", T0 + 40));
    CHECK_EQ(text(j["cancelled"]), R"(["Tea"])");
    CHECK_EQ(text(j["still_running"]), R"(["Eggs","Food out of the oven"])");
    CHECK_EQ(Store::get().timers().size(), 2);

    // cancel all.
    j = parsed(tool(R"({"action":"cancel","all":true})", T0 + 50));
    CHECK_EQ(j["cancelled"].size(), 2);
    CHECK(Store::get().timers().empty());
    j = parsed(tool(R"({"action":"list"})", T0 + 50));
    CHECK_EQ(j["timers"].size(), 0);
    CHECK_EQ(text(j["note"]), "no timer is running");
    r = tool(R"({"action":"cancel"})", T0 + 50);
    CHECK(is_error(r) && mentions(r, "no timer is running"));

    // With one timer running, "the timer" needs no label; one started without a label is named
    // after its length.
    j = parsed(tool(R"({"action":"start","minutes":10})", T0));
    CHECK_EQ(text(j["started"]["label"]), "10 minute timer");
    j = parsed(tool(R"({"action":"add_time","minutes":5})", T0 + 60));
    CHECK_EQ(text(j["extended"]["label"]), "10 minute timer");
    CHECK_EQ(timer_end("10 minute timer"), T0 + 900);
    j = parsed(tool(R"({"action":"cancel"})", T0 + 70));
    CHECK_EQ(text(j["cancelled"]), R"(["10 minute timer"])");

    // A length past midnight says the day.
    j = parsed(tool(R"({"action":"start","minutes":960,"label":"Bread"})", T0));
    CHECK_EQ(text(j["started"]["ends_at"]), "tomorrow 00:00");
}

void test_tool_errors() {
    reset_store();
    std::string r = tool(R"({"action":"start","label":"Pasta"})", T0);
    CHECK(is_error(r) && mentions(r, "ask them"));                  // no length: the model asks
    CHECK(is_error(tool(R"({"action":"start","minutes":0})", T0)));
    CHECK(is_error(tool(R"({"action":"start","minutes":-5})", T0)));
    CHECK(is_error(tool(R"({"action":"start","seconds":0.2})", T0)));
    CHECK(is_error(tool(R"({"action":"start","minutes":1441})", T0)));
    CHECK(!is_error(tool(R"({"action":"start","minutes":1440,"label":"A day"})", T0)));
    CHECK(is_error(tool(R"({"action":"add_time","minutes":1})", T0)));   // a day is the most
    CHECK(is_error(tool(R"({"action":"add_time","label":"a day"})", T0)));   // how much to add?
    CHECK(is_error(tool(R"({"action":"explode"})", T0)));
    CHECK(is_error(tool(R"({})", T0)));
    CHECK(Store::get().timers().size() == 1);
}

// ---------------------------------------------------------------- which timer is meant
void test_label_matching() {
    const std::vector<Reminder> v = {timer(1, "Study"), timer(2, "Study break"), timer(3, "Food out of the oven"),
                                     timer(4, "Pasta"), timer(5, "Pasta 2"), timer(6, "5 minute timer"),
                                     timer(7, "Dad's call")};
    auto ids = [&](const std::string &words) {
        std::vector<int> out;
        for (auto &r : timers::match(v, words)) out.push_back(r.id);
        return out;
    };
    CHECK(ids("study") == std::vector<int>{1});                   // the exact name beats "Study break"
    CHECK(ids("the study timer") == std::vector<int>{1});
    CHECK(ids("Study break") == std::vector<int>{2});
    CHECK(ids("break") == std::vector<int>{2});
    CHECK(ids("oven") == std::vector<int>{3});
    CHECK(ids("the food one") == std::vector<int>{3});
    CHECK(ids("pasta") == std::vector<int>{4});
    CHECK(ids("pasta 2") == std::vector<int>{5});
    CHECK(ids("five minute") == std::vector<int>{6});
    CHECK(ids("dads") == std::vector<int>{7});
    CHECK(ids("dad\xE2\x80\x99s") == std::vector<int>{7});         // a typographic apostrophe
    CHECK(ids("studying") == std::vector<int>({1, 2}));          // fits both equally: ambiguous
    CHECK(ids("pizza").empty());
    CHECK(ids("the timer").size() == v.size());                   // no timer in particular
    CHECK(ids("").size() == v.size());

    // "One" before a unit is a number, not a filler word: "the one minute timer" is one timer.
    const std::vector<Reminder> lengths = {timer(1, "1 minute timer"), timer(2, "2 minute timer"),
                                           timer(3, "10 minute timer"), timer(4, "1 hour timer"), timer(5, "Oven")};
    auto by_length = [&](const std::string &words) {
        std::vector<int> out;
        for (auto &r : timers::match(lengths, words)) out.push_back(r.id);
        return out;
    };
    CHECK(by_length("one minute") == std::vector<int>{1});
    CHECK(by_length("the one minute timer") == std::vector<int>{1});
    CHECK(by_length("the one-minute one") == std::vector<int>{1});
    CHECK(by_length("the one hour timer") == std::vector<int>{4});
    CHECK(by_length("ten minute") == std::vector<int>{3});
    CHECK(by_length("the oven one") == std::vector<int>{5});      // and a filler word otherwise

    // A number in two words is one number: "twenty five" is 25, not 20 and 5 (which fits the
    // 5 minute timer, and the one of 1 hour 20 minutes, better than the 25 minute one).
    const std::vector<Reminder> spelt = {timer(1, "5 minute timer"),  timer(2, "25 minute timer"), timer(3, "30 minute timer"),
                                         timer(4, "35 minute timer"), timer(5, "45 minute timer"), timer(6, "1 minute timer"),
                                         timer(7, "21 minute timer"), timer(8, "1 hour 20 minute timer")};
    auto by_number = [&](const std::string &words) {
        std::vector<int> out;
        for (auto &r : timers::match(spelt, words)) out.push_back(r.id);
        return out;
    };
    CHECK(by_number("twenty five minute timer") == std::vector<int>{2});
    CHECK(by_number("the thirty-five minute timer") == std::vector<int>{4});
    CHECK(by_number("forty five minute") == std::vector<int>{5});
    CHECK(by_number("twenty-one minutes") == std::vector<int>{7});
    CHECK(by_number("thirty minutes") == std::vector<int>{3});
    CHECK(by_number("five minute") == std::vector<int>{1});
    CHECK(by_number("the one minute one") == std::vector<int>{6});
    CHECK(by_number("the twenty minute one") == std::vector<int>{8});
    const std::vector<Reminder> bakes = {timer(1, "Bake 80"), timer(2, "Bake 8")};
    CHECK(timers::match(bakes, "bake eighty").size() == 1 && timers::match(bakes, "bake eighty")[0].id == 1);

    // A timer's whole name means that timer, even when another differs from it only in filler
    // words (as two timers set before names were kept apart could).
    const std::vector<Reminder> alike = {timer(1, "Pasta"), timer(2, "Pasta timer")};
    auto by_name = [&](const std::string &words) {
        std::vector<int> out;
        for (auto &r : timers::match(alike, words)) out.push_back(r.id);
        return out;
    };
    CHECK(by_name("pasta timer") == std::vector<int>{2});
    CHECK(by_name("Pasta") == std::vector<int>{1});
    CHECK(by_name("the pasta") == std::vector<int>({1, 2}));       // either: the model asks which
}

void test_ambiguity_reported() {
    reset_store();
    tool(R"({"action":"start","minutes":45,"label":"Study"})", T0);
    tool(R"({"action":"start","minutes":10,"label":"Study break"})", T0);
    tool(R"({"action":"start","minutes":8,"label":"Pasta"})", T0);

    std::string r = tool(R"({"action":"cancel"})", T0 + 1);       // "cancel the timer" with three running
    CHECK(is_error(r) && mentions(r, "3 timers are running") && mentions(r, "Ask the user which one") &&
          mentions(r, "all: true"));
    r = tool(R"({"action":"cancel","label":"studying"})", T0 + 1);
    CHECK(is_error(r) && mentions(r, "more than one timer could be meant") && mentions(r, "Study (id") &&
          mentions(r, "Study break (id") && !mentions(r, "Pasta") && mentions(r, "Ask the user which one"));
    r = tool(R"({"action":"add_time","minutes":5})", T0 + 1);    // which one?
    CHECK(is_error(r) && mentions(r, "more than one timer could be meant"));
    r = tool(R"({"action":"cancel","label":"pizza"})", T0 + 1);
    CHECK(is_error(r) && mentions(r, "no running timer is called 'pizza'") && mentions(r, "Pasta (id"));
    CHECK_EQ(Store::get().timers().size(), 3);                     // nothing was cancelled by mistake

    json j = parsed(tool(R"({"action":"cancel","label":"study"})", T0 + 2));
    CHECK_EQ(text(j["cancelled"]), R"(["Study"])");
    CHECK(timer_id("Study break") != 0);
    j = parsed(tool(R"({"action":"add_time","minutes":5,"label":"break"})", T0 + 2));
    CHECK_EQ(text(j["extended"]["label"]), "Study break");

    // A second timer with the same name is numbered, so each can be named.
    j = parsed(tool(R"({"action":"start","minutes":3,"label":"pasta"})", T0 + 3));
    CHECK_EQ(text(j["started"]["label"]), "Pasta 2");
    j = parsed(tool(R"({"action":"cancel","label":"pasta 2"})", T0 + 4));
    CHECK_EQ(text(j["cancelled"]), R"(["Pasta 2"])");
    // And the id from a result works too.
    const int id = timer_id("Pasta");
    j = parsed(tool(R"({"action":"cancel","id":)" + std::to_string(id) + "}", T0 + 5));
    CHECK_EQ(text(j["cancelled"]), R"(["Pasta"])");

    // A name that differs from a running one only in filler words is numbered too, so that both
    // can still be named: "pasta timer" beside "Pasta" would otherwise be the same words.
    reset_store();
    tool(R"({"action":"start","minutes":8,"label":"Pasta"})", T0);
    j = parsed(tool(R"({"action":"start","minutes":12,"label":"pasta timer"})", T0));
    CHECK_EQ(text(j["started"]["label"]), "Pasta timer 2");
    j = parsed(tool(R"({"action":"add_time","minutes":1,"label":"pasta timer 2"})", T0 + 1));
    CHECK_EQ(text(j["extended"]["label"]), "Pasta timer 2");
    j = parsed(tool(R"({"action":"cancel","label":"the pasta"})", T0 + 2));
    CHECK_EQ(text(j["cancelled"]), R"(["Pasta"])");
    // Two such timers carried over from an earlier version: each is reached by its whole name.
    reset_store();
    Store::get().add_timer("Pasta", 480, T0);
    Store::get().add_timer("Pasta timer", 720, T0);
    j = parsed(tool(R"({"action":"cancel","label":"Pasta timer"})", T0 + 3));
    CHECK_EQ(text(j["cancelled"]), R"(["Pasta timer"])");
    CHECK(timer_id("Pasta") != 0);
}

// Timers named by their length, picked with the number spelt out in two words.
void test_spelt_out_numbers() {
    reset_store();
    tool(R"({"action":"start","minutes":5})", T0);
    tool(R"({"action":"start","minutes":25})", T0);
    json j = parsed(tool(R"({"action":"cancel","label":"twenty five minute timer"})", T0 + 1));
    CHECK_EQ(text(j["cancelled"]), R"(["25 minute timer"])");
    CHECK_EQ(text(j["still_running"]), R"(["5 minute timer"])");

    reset_store();
    tool(R"({"action":"start","minutes":30})", T0);
    tool(R"({"action":"start","minutes":35})", T0);
    j = parsed(tool(R"({"action":"cancel","label":"the thirty-five minute timer"})", T0 + 1));
    CHECK_EQ(text(j["cancelled"]), R"(["35 minute timer"])");

    reset_store();
    tool(R"({"action":"start","minutes":45})", T0);
    tool(R"({"action":"start","minutes":5})", T0);
    j = parsed(tool(R"({"action":"add_time","minutes":10,"label":"forty five minute"})", T0 + 1));
    CHECK_EQ(text(j["extended"]["label"]), "45 minute timer");
    CHECK_EQ(timer_end("45 minute timer"), T0 + 3300);
    CHECK_EQ(timer_end("5 minute timer"), T0 + 300);
}

// ---------------------------------------------------------------- storage
void test_survives_restart() {
    reset_store();
    tool(R"({"action":"start","minutes":60,"label":"Study"})", T0);
    tool(R"({"action":"start","minutes":8,"label":"Pasta"})", T0);
    Store::get().add_reminder("Call the hotel", T0 + 7200);

    Store::get().load();                                           // the app is closed and opened again
    const std::vector<Reminder> running = Store::get().timers();
    CHECK_EQ(running.size(), 2);
    CHECK_EQ(running[0].text, "Pasta");
    CHECK(running[0].timer());
    CHECK_EQ(running[0].due, T0 + 480);
    CHECK_EQ(running[0].started, T0);
    CHECK_EQ(running[0].length, 480);
    CHECK_EQ(running[1].text, "Study");
    CHECK_EQ(Store::get().next_timer_end(), T0 + 480);
    const std::vector<Reminder> reminders = Store::get().reminders();
    CHECK_EQ(reminders.size(), 1);
    CHECK_EQ(reminders[0].text, "Call the hotel");

    // On disk the timers are marked; the reminder's record keeps the shape it always had.
    const json file = util::load_json(util::data_dir() + "/reminders.json", json::array());
    CHECK_EQ(file.size(), 3);
    for (const json &rec : file) {
        if (rec["text"] == "Call the hotel") CHECK(!rec.contains("kind") && !rec.contains("length"));
        else CHECK(rec["kind"] == "timer" && rec.contains("started") && rec.contains("length"));
    }
    // New ids carry on from the stored ones.
    CHECK(Store::get().add_timer("Tea", 60, T0) > std::max(running[0].id, running[1].id));
}

void test_old_reminders_file() {
    reset_store();
    // reminders.json as written before timers existed, with a "timer" set by reminder_set.
    util::write_file_atomic(util::data_dir() + "/reminders.json",
                            R"([{"id":3,"text":"Check the laundry","due":1800000600,"fired":false},)"
                            R"({"id":7,"text":"","due":1800000060,"fired":false}])");
    Store::get().load();
    CHECK(Store::get().timers().empty());
    CHECK_EQ(Store::get().next_timer_end(), 0);
    std::vector<Reminder> reminders = Store::get().reminders();
    CHECK_EQ(reminders.size(), 2);
    CHECK_EQ(reminders[0].id, 7);
    CHECK_EQ(reminders[1].text, "Check the laundry");
    // They fire as they always did.
    const std::vector<std::string> alerts = Store::get().take_due_alerts(T0 + 61);
    CHECK_EQ(alerts.size(), 1);
    CHECK_EQ(alerts.empty() ? "" : alerts[0], "Timer finished");
    // Starting a timer keeps the old record, and takes a fresh id.
    tool(R"({"action":"start","minutes":5,"label":"Tea"})", T0 + 61);
    Store::get().load();
    reminders = Store::get().reminders();
    CHECK_EQ(reminders.size(), 1);
    CHECK_EQ(reminders.empty() ? "" : reminders[0].text, "Check the laundry");
    CHECK_EQ(Store::get().timers().size(), 1);
    CHECK(timer_id("Tea") > 7);
}

void test_reminders_unchanged() {
    reset_store();
    Store::get().add_reminder("Call the hotel", T0 + 60);
    tool(R"({"action":"start","minutes":1,"label":"Tea"})", T0);
    const int tea = timer_id("Tea");
    CHECK(!Store::get().delete_reminder(tea));                    // reminder_cancel cannot cancel a timer
    CHECK(timer_id("Tea") == tea);
    CHECK_EQ(Store::get().reminders().size(), 1);                  // reminder_list does not list timers
    const std::vector<std::string> alerts = Store::get().take_due_alerts(T0 + 61);
    CHECK_EQ(alerts.size(), 1);                                    // an ended timer is not a reminder alert
    CHECK_EQ(alerts.empty() ? "" : alerts[0], "Call the hotel");
    CHECK(timer_id("Tea") == tea);                                 // it waits for the alarm instead
    Store::get().add_reminder("Later", T0 + 999);
    Store::get().clear_reminders();                                // "Cancel all" reminders
    CHECK(Store::get().reminders().empty());
    CHECK(timer_id("Tea") == tea);
    CHECK_EQ(Store::get().clear_timers(), 1);
}

void test_cap() {
    reset_store();
    for (int i = 1; i <= Store::kMaxTimers; ++i)
        CHECK(!is_error(tool(R"({"action":"start","minutes":)" + std::to_string(i) + R"(,"label":"T)" + std::to_string(i) + "\"}", T0)));
    const std::string r = tool(R"({"action":"start","minutes":5,"label":"One too many"})", T0);
    CHECK(is_error(r) && mentions(r, "10 timers are already running"));
    CHECK_EQ(Store::get().timers().size(), 10);
    CHECK_EQ(Store::get().add_timer("Direct", 60, T0), 0);         // the store refuses as well
    CHECK(!is_error(tool(R"({"action":"cancel","label":"T3"})", T0)));
    CHECK(!is_error(tool(R"({"action":"start","minutes":5,"label":"Now it fits"})", T0)));
    CHECK_EQ(Store::get().timers().size(), 10);
}

// ---------------------------------------------------------------- the alarm
void test_missed_on_restart() {
    reset_store();
    // The app was closed while two of these ended.
    Store::get().add_timer("Long gone", 60, T0 - 660);             // ended ten minutes ago
    Store::get().add_timer("Just now", 60, T0 - 90);               // ended thirty seconds ago
    Store::get().add_timer("Still going", 600, T0);
    Store::get().load();                                           // and now it opens again
    FakeSound fs;
    Alarm alarm(fs.sound());
    const std::vector<timers::Ended> ended = alarm.poll(T0, 1000, false, false);
    CHECK_EQ(ended.size(), 2);
    if (ended.size() == 2) {
        CHECK_EQ(ended[0].label, "Long gone");
        CHECK(ended[0].missed);                                    // shown as missed, not rung
        CHECK_EQ(ended[0].end, T0 - 600);
        CHECK_EQ(ended[1].label, "Just now");
        CHECK(!ended[1].missed);                                   // under two minutes: it rings
    }
    CHECK(alarm.ringing());
    CHECK(alarm.labels() == std::vector<std::string>{"Just now"});
    CHECK_EQ(fs.starts, 1);
    CHECK(alarm.poll(T0 + 1, 2000, false, false).empty());         // each end is reported once
    CHECK(timer_id("Still going") != 0);
    alarm.stop();

    // Exactly two minutes late is missed; a second less still rings.
    reset_store();
    Store::get().add_timer("Edge", 60, T0 - 180);
    Store::get().load();
    std::vector<timers::Ended> e = alarm.poll(T0, 3000, false, false);
    CHECK(e.size() == 1 && e[0].missed);
    CHECK(!alarm.ringing());
    reset_store();
    Store::get().add_timer("Edge 2", 60, T0 - 179);
    Store::get().load();
    e = alarm.poll(T0, 3000, false, false);
    CHECK(e.size() == 1 && !e[0].missed);
    CHECK(alarm.ringing());
    alarm.stop();
}

// A timer set while the app runs rings however late its end is noticed: missing it is only for
// what ended while the app was closed.
void test_late_in_this_run() {
    reset_store();
    FakeSound fs;
    Alarm alarm(fs.sound());
    tool(R"({"action":"start","minutes":1,"label":"Tea"})", T0);
    // The microphone is open from just before the end until well over two minutes after it.
    CHECK(alarm.poll(T0 + 60, 61000, true, false).empty());
    CHECK(alarm.poll(T0 + 190, 191000, true, false).empty());
    const std::vector<timers::Ended> ended = alarm.poll(T0 + 200, 201000, false, false);
    CHECK(ended.size() == 1 && !ended[0].missed);
    CHECK(alarm.ringing());
    CHECK_EQ(fs.starts, 1);
    alarm.stop();
}

// The wall clock is set while timers run: a device without a clock of its own is set by a time
// sync some while after it starts. A timer still ends when its length has passed.
void test_clock_set_while_running() {
    Store &store = Store::get();
    const long long boot = 7000000;                                // the steady clock: any reading will do
    FakeSound fs;
    Alarm alarm(fs.sound());

    // An hour forward, half a minute into a five-minute timer.
    reset_store();
    store.follow_clock(T0 * 1000LL, boot);                         // the first reading moves nothing
    tool(R"({"action":"start","minutes":5,"label":"Food out of the oven"})", T0);
    store.follow_clock((T0 + 30) * 1000LL, boot + 30000);          // half a minute passes
    CHECK_EQ(timer_end("Food out of the oven"), T0 + 300);
    store.follow_clock((T0 + 30 + 3600) * 1000LL + 400, boot + 30000);   // the clock is set
    CHECK_EQ(timer_end("Food out of the oven"), T0 + 300 + 3600);
    CHECK(alarm.poll(T0 + 3630, 31000, false, false).empty());     // four and a half minutes still to go
    CHECK_EQ(text(parsed(tool(R"({"action":"list"})", T0 + 3630))["timers"][0]["time_left"]), "4 minutes 30 seconds");
    CHECK_EQ(timers::badge({timer_end("Food out of the oven")}, T0 + 3630), "4:30");
    const json file = util::load_json(util::data_dir() + "/reminders.json", json::array());   // the move is kept
    CHECK(file.size() == 1 && file[0].value("text", "") == "Food out of the oven" && file[0].value("due", 0LL) == T0 + 3900);
    std::vector<timers::Ended> ended = alarm.poll(T0 + 3900, 301000, false, false);
    CHECK(ended.size() == 1 && !ended[0].missed);                  // and it rings, on time
    CHECK(alarm.ringing());
    alarm.stop();

    // An hour back: it does not ring an hour late.
    reset_store();
    store.follow_clock(T0 * 1000LL, boot);
    tool(R"({"action":"start","minutes":5,"label":"Eggs"})", T0);
    store.follow_clock((T0 + 30 - 3600) * 1000LL, boot + 30000);
    CHECK_EQ(timer_end("Eggs"), T0 + 300 - 3600);
    ended = alarm.poll(T0 + 300 - 3600, 301000, false, false);
    CHECK(ended.size() == 1 && !ended[0].missed);
    alarm.stop();

    // A clock that drifts or is slewed a little at a time moves nothing.
    reset_store();
    store.follow_clock(T0 * 1000LL, boot);
    tool(R"({"action":"start","minutes":5,"label":"Tea"})", T0);
    for (int s = 1; s <= 10; ++s) store.follow_clock((T0 + s) * 1000LL + s * 150, boot + s * 1000);
    CHECK_EQ(timer_end("Tea"), T0 + 300);

    // A timer carried over from an earlier run keeps its end: it was reckoned by that run's clock,
    // and setting this run's clock right is what makes the two agree again.
    reset_store();
    store.add_timer("Bread", 600, T0);
    store.load();
    store.follow_clock(T0 * 1000LL, boot);
    store.follow_clock((T0 + 3610) * 1000LL, boot + 10000);
    CHECK_EQ(timer_end("Bread"), T0 + 600);
    ended = alarm.poll(T0 + 3610, 11000, false, false);
    CHECK(ended.size() == 1 && ended[0].missed);                   // it ended long ago, while the app was closed
    CHECK(!alarm.ringing());
}

// The assistant starts a timer just as the wall clock is set. The main loop sees a step on its next
// tick only, so the time the assistant read can be the new clock's while the timers still go by the
// old one, or the old clock's when the loop has moved them already. Either way the timer ends when
// its length has passed: not a whole step late, and not at once.
void test_clock_set_as_a_timer_starts() {
    Store &store = Store::get();
    const long long boot = 7000000;
    const long long before = T0 * 1000LL - boot, after = before + 3600 * 1000LL;   // wall minus steady, either side of the step
    FakeSound fs;
    Alarm alarm(fs.sound());
    auto run = [](const char *args, time_t now, long long offset) { return timers::run_tool(json::parse(args), now, offset); };

    // An hour forward ten seconds in: the assistant reads the new time, then the loop sees the step.
    reset_store();
    store.follow_clock(T0 * 1000LL, boot);
    CHECK(!is_error(run(R"({"action":"start","minutes":5,"label":"Tea"})", T0 + 3610, after)));
    store.follow_clock((T0 + 3610) * 1000LL + 66, boot + 10066);
    CHECK_EQ(timer_end("Tea"), T0 + 3610 + 300);
    CHECK(alarm.poll(T0 + 3610 + 299, 1000, false, false).empty());
    CHECK_EQ(alarm.poll(T0 + 3610 + 300, 2000, false, false).size(), 1);
    alarm.stop();

    // The assistant read the time just before the step, and the loop saw the step before the timer
    // went in.
    reset_store();
    store.follow_clock(T0 * 1000LL, boot);
    store.follow_clock((T0 + 3610) * 1000LL + 10, boot + 10010);
    CHECK(!is_error(run(R"({"action":"start","minutes":5,"label":"Eggs"})", T0 + 10, before)));
    CHECK_EQ(timer_end("Eggs"), T0 + 3610 + 300);
    CHECK(alarm.poll(T0 + 3611, 3000, false, false).empty());
    CHECK(timer_id("Eggs") != 0);

    // "Give it five more minutes" just after an alarm, as the clock is set: the timer that rang runs
    // again for five minutes from then, whichever side of the step the request was read on.
    for (const bool loop_first : {false, true}) {
        reset_store();
        store.follow_clock(T0 * 1000LL, boot);
        tool(R"({"action":"start","minutes":1,"label":"Pasta"})", T0);
        alarm.poll(T0 + 60, 4000, false, false);
        alarm.stop();
        if (loop_first) store.follow_clock((T0 + 3670) * 1000LL + 10, boot + 70010);
        json j = parsed(loop_first ? run(R"({"action":"add_time","minutes":5})", T0 + 70, before)
                                   : run(R"({"action":"add_time","minutes":5})", T0 + 3670, after));
        CHECK_EQ(text(j["restarted"]["label"]), "Pasta");
        if (!loop_first) store.follow_clock((T0 + 3670) * 1000LL + 30, boot + 70030);
        CHECK_EQ(timer_end("Pasta"), T0 + 3670 + 300);
    }

    // The loop sees the step while the assistant is between reading the time and adding the timer:
    // the store moves that time across the step as the timer goes in.
    reset_store();
    store.follow_clock(T0 * 1000LL, boot);
    tool(R"({"action":"start","minutes":1,"label":"Rice"})", T0);
    alarm.poll(T0 + 60, 6000, false, false);
    alarm.stop();
    store.follow_clock((T0 + 3670) * 1000LL, boot + 70000);
    CHECK(store.add_timer("Soup", 300, T0 + 70, before) != 0);
    CHECK_EQ(timer_end("Soup"), T0 + 3670 + 300);
    const std::vector<Reminder> rang = store.recent_timers(T0 + 3670);
    CHECK(rang.size() == 1 && store.restart_timer(rang[0].id, 120, T0 + 70, before) != 0);
    CHECK_EQ(timer_end("Rice"), T0 + 3670 + 120);
}

// As the assistant calls it, by the real clocks: read just after the wall clock was set, before the
// main loop has moved the timers, the tool waits for the loop's next tick, so that the time its
// result names is the one on the clock. Should the loop not come by, it waits only so long.
void test_tool_waits_for_the_loop() {
    Store &store = Store::get();
    auto clocks = [](long long &wall, long long &steady) {
        timespec w{}, s{};
        clock_gettime(CLOCK_REALTIME, &w);
        clock_gettime(CLOCK_BOOTTIME, &s);
        wall = static_cast<long long>(w.tv_sec) * 1000 + w.tv_nsec / 1000000;
        steady = static_cast<long long>(s.tv_sec) * 1000 + s.tv_nsec / 1000000;
    };
    auto wall_now = [&] {
        long long wall = 0, steady = 0;
        clocks(wall, steady);
        return static_cast<time_t>(wall / 1000);
    };
    auto loop_tick = [&](long long behind_ms) {   // the main loop's call, the clock `behind_ms` behind the real one
        long long wall = 0, steady = 0;
        clocks(wall, steady);
        store.follow_clock(wall - behind_ms, steady);
    };
    const char *const start = R"({"action":"start","minutes":5,"label":"Tea"})";

    reset_store();
    loop_tick(3600 * 1000LL);                      // the loop last saw the clock an hour back: it has just been set
    std::thread loop([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        loop_tick(0);                              // its next tick
    });
    const time_t before = wall_now();
    auto t = std::chrono::steady_clock::now();
    json j = parsed(timers::run_tool(json::parse(start)));
    long long waited = ms_since(t);
    const time_t after = wall_now();
    loop.join();
    CHECK(waited >= 40 && waited < timers::kClockWaitMs - 100);
    const std::string ends = text(j["started"]["ends_at"]);
    CHECK(ends == util::format_time(before + 300, "%H:%M") || ends == util::format_time(after + 300, "%H:%M"));
    CHECK(timer_end("Tea") >= before + 300 && timer_end("Tea") <= after + 300);

    // The loop does not come by: the tool goes on after kClockWaitMs, with the time as the timers
    // reckon it, and the new timer moves with them when the loop does come.
    reset_store();
    loop_tick(3600 * 1000LL);
    const time_t from = wall_now();
    t = std::chrono::steady_clock::now();
    CHECK(!is_error(timers::run_tool(json::parse(start))));
    waited = ms_since(t);
    CHECK(waited >= timers::kClockWaitMs - 20 && waited < timers::kClockWaitMs + 1500);
    loop_tick(0);
    CHECK(timer_end("Tea") >= from + 300 && timer_end("Tea") <= wall_now() + 300);
}

void test_alarm_rings_and_any_key_silences() {
    reset_store();
    audio::Music &music = audio::Music::get();
    FakeSound fs;
    Alarm alarm(fs.sound());
    tool(R"({"action":"start","minutes":5,"label":"Food out of the oven"})", T0);
    CHECK(alarm.poll(T0 + 299, 9000, false, false).empty());       // not yet
    CHECK(!alarm.ringing());
    CHECK(!music.holding(audio::Music::kAlarm));
    const std::vector<timers::Ended> ended = alarm.poll(T0 + 300, 10000, false, false);
    CHECK(ended.size() == 1 && ended[0].label == "Food out of the oven" && !ended[0].missed);
    CHECK(alarm.ringing() && alarm.sounding());
    CHECK(alarm.labels() == std::vector<std::string>{"Food out of the oven"});
    CHECK_EQ(fs.starts, 1);
    CHECK(music.holding(audio::Music::kAlarm));                    // the music waits while it rings
    CHECK(Store::get().timers().empty());

    CHECK(alarm.key(false) == Alarm::Key::Silence);                // any key: silenced, and used up
    CHECK(!alarm.ringing() && !alarm.sounding());
    CHECK_EQ(fs.stops, 1);
    CHECK(!music.holding(audio::Music::kAlarm));
    CHECK(alarm.labels().empty());
    CHECK(alarm.key(false) == Alarm::Key::None);                   // nothing ringing: keys do their usual job
    CHECK(alarm.key(true) == Alarm::Key::None);
}

void test_alarm_tab_silences_and_listens() {
    reset_store();
    FakeSound fs;
    Alarm alarm(fs.sound());
    tool(R"({"action":"start","seconds":30,"label":"Eggs"})", T0);
    alarm.poll(T0 + 30, 5000, false, false);
    CHECK(alarm.ringing());
    CHECK(alarm.key(true) == Alarm::Key::Listen);
    CHECK(!alarm.ringing());
    CHECK_EQ(fs.stops, 1);
    CHECK(!audio::Music::get().holding(audio::Music::kAlarm));
}

void test_alarm_stops_after_a_minute() {
    reset_store();
    FakeSound fs;
    Alarm alarm(fs.sound());
    tool(R"({"action":"start","minutes":1,"label":"Tea"})", T0);
    alarm.poll(T0 + 60, 5000, false, false);
    CHECK(!alarm.tick(5000 + 59999));
    CHECK(alarm.ringing());
    CHECK(alarm.tick(5000 + 60000));                               // 60 seconds: it stops by itself
    CHECK(!alarm.ringing());
    CHECK_EQ(fs.stops, 1);
    CHECK(!audio::Music::get().holding(audio::Music::kAlarm));
    CHECK(!alarm.tick(5000 + 70000));

    // A timer that ends while another rings joins it, with a full minute of its own: the banner
    // names both, and the sound starts again for it (the sound has a limit of its own, counted
    // from its start, so carrying on with the first one's would leave the second one short).
    reset_store();
    tool(R"({"action":"start","minutes":1,"label":"A"})", T0);
    tool(R"({"action":"start","seconds":90,"label":"B"})", T0);
    const int before = fs.starts;
    alarm.poll(T0 + 60, 1000, false, false);
    CHECK_EQ(fs.starts, before + 1);
    alarm.poll(T0 + 90, 31000, false, false);
    CHECK(alarm.labels() == std::vector<std::string>({"A", "B"}));
    CHECK_EQ(fs.starts, before + 2);
    CHECK(!alarm.tick(61000));
    CHECK(alarm.tick(91000));
    CHECK_EQ(fs.stops, 2);
}

// The alarm is under way before reminders.json is written: on a slow card the write can take a
// while, and the sound need not wait for it.
void test_rings_before_the_file_is_written() {
    reset_store();
    FakeSound fs;
    bool listed_when_it_started = false;
    fs.on_start = [&] { listed_when_it_started = file_lists("Tea"); };
    Alarm alarm(fs.sound());
    tool(R"({"action":"start","minutes":1,"label":"Tea"})", T0);
    alarm.poll(T0 + 60, 1000, false, false);
    CHECK_EQ(fs.starts, 1);
    CHECK(listed_when_it_started);
    CHECK(!file_lists("Tea"));                                     // and then the file follows
    Store::get().load();
    CHECK(Store::get().timers().empty());
    alarm.stop();
}

// Damaged records: a timer without an end is mended from its start and length, or left out, and
// never hides the other timers from the alarm.
void test_damaged_timer_records() {
    reset_store();
    util::write_file_atomic(util::data_dir() + "/reminders.json",
                            R"([{"id":1,"text":"Pasta","due":1800000060,"fired":false,"kind":"timer","started":1800000000,"length":60},)"
                            R"({"id":2,"text":"Broken","fired":false,"kind":"timer"},)"
                            R"({"id":3,"text":"Mended","due":"soon","fired":false,"kind":"timer","started":1800000000,"length":120},)"
                            R"({"id":4,"text":"Call the hotel","due":1800007200,"fired":false}])");
    Store::get().load();
    CHECK_EQ(Store::get().timers().size(), 2);
    CHECK_EQ(timer_end("Mended"), T0 + 120);
    CHECK_EQ(Store::get().next_timer_end(), T0 + 60);
    CHECK_EQ(Store::get().reminders().size(), 1);
    FakeSound fs;
    Alarm alarm(fs.sound());
    const std::vector<timers::Ended> ended = alarm.poll(T0 + 60, 1000, false, false);
    CHECK(ended.size() == 1 && ended[0].label == "Pasta" && !ended[0].missed);
    CHECK(alarm.ringing());
    alarm.stop();
}

// A save that runs out of room (a full card) leaves the file as it was, rather than cut short.
void test_full_disk() {
    reset_store();
    tool(R"({"action":"start","minutes":5,"label":"Pasta"})", T0);
    const std::string path = util::data_dir() + "/reminders.json";
    std::string before, after;
    util::read_file(path, before);
    signal(SIGXFSZ, SIG_IGN);                                      // a write past the limit fails, as on a full disk
    rlimit old{};
    getrlimit(RLIMIT_FSIZE, &old);
    rlimit tiny = old;
    tiny.rlim_cur = 64;
    setrlimit(RLIMIT_FSIZE, &tiny);
    const bool wrote = util::write_file_atomic(path, std::string(1000, 'x'));
    Store::get().add_timer("Sauce", 600, T0);                      // its save fails too
    setrlimit(RLIMIT_FSIZE, &old);
    CHECK(!wrote);
    util::read_file(path, after);
    CHECK(after == before);
    CHECK(access((path + ".tmp").c_str(), F_OK) != 0);            // nothing half-written is left about
    Store::get().load();
    CHECK_EQ(Store::get().timers().size(), 1);                     // what was saved before is all there
    CHECK(timer_id("Pasta") != 0);
}

void test_alarm_waits_while_recording() {
    reset_store();
    FakeSound fs;
    Alarm alarm(fs.sound());
    tool(R"({"action":"start","minutes":1,"label":"Tea"})", T0);
    CHECK(alarm.poll(T0 + 60, 1000, true, false).empty());         // the microphone is open: it waits
    CHECK(alarm.poll(T0 + 75, 16000, true, false).empty());
    CHECK(!alarm.ringing());
    CHECK_EQ(fs.starts, 0);
    CHECK(timer_id("Tea") != 0);                                   // still there, showing 0:00
    const std::vector<timers::Ended> ended = alarm.poll(T0 + 80, 21000, false, false);   // closed: now it rings
    CHECK(ended.size() == 1 && !ended[0].missed);
    CHECK(alarm.ringing());
    CHECK_EQ(fs.starts, 1);
    alarm.stop();
}

void test_alarm_quiet_when_muted() {
    reset_store();
    FakeSound fs;
    Alarm alarm(fs.sound());
    tool(R"({"action":"start","minutes":1,"label":"Tea"})", T0);
    alarm.poll(T0 + 60, 1000, false, true);
    CHECK(alarm.ringing());                                        // the banner shows
    CHECK(!alarm.sounding());
    CHECK_EQ(fs.starts, 0);                                        // but mute is respected
    CHECK(alarm.key(false) == Alarm::Key::Silence);
    CHECK_EQ(fs.stops, 0);
}

void test_music_hold() {
    audio::Music &music = audio::Music::get();
    music.hold(audio::Music::kUser, true);                         // the user had paused the music
    reset_store();
    {
        Alarm alarm;
        tool(R"({"action":"start","minutes":1,"label":"Tea"})", T0);
        alarm.poll(T0 + 60, 1000, false, false);
        CHECK(music.holding(audio::Music::kAlarm));
        alarm.stop();                                              // as when the app is left
        CHECK(!music.holding(audio::Music::kAlarm));
        CHECK(music.holding(audio::Music::kUser));                 // other reasons are left as they were
    }
    {
        Alarm alarm;
        tool(R"({"action":"start","minutes":1,"label":"Tea"})", T0);
        alarm.poll(T0 + 60, 1000, false, false);
        CHECK(music.holding(audio::Music::kAlarm));
    }                                                              // destroyed while ringing
    CHECK(!music.holding(audio::Music::kAlarm));
    music.hold(audio::Music::kUser, false);
}

void test_more_time_after_the_alarm() {
    reset_store();
    Alarm alarm;
    tool(R"({"action":"start","minutes":5,"label":"Food out of the oven"})", T0);
    tool(R"({"action":"start","minutes":30,"label":"Study"})", T0);
    alarm.poll(T0 + 300, 1000, false, false);
    CHECK(alarm.key(true) == Alarm::Key::Listen);                  // TAB: "give it five more minutes"
    json j = parsed(tool(R"({"action":"add_time","minutes":5})", T0 + 310));
    CHECK_EQ(text(j["restarted"]["label"]), "Food out of the oven");
    CHECK_EQ(timer_end("Food out of the oven"), T0 + 610);
    CHECK_EQ(timer_end("Study"), T0 + 1800);                       // not the one still running

    // Named a while later, an ended timer starts again too, and list mentions what has ended.
    alarm.poll(T0 + 610, 2000, false, false);
    alarm.stop();
    j = parsed(tool(R"({"action":"list"})", T0 + 900));
    CHECK_EQ(j["timers"].size(), 1);
    CHECK_EQ(text(j["ended_recently"][0]["label"]), "Food out of the oven");
    CHECK_EQ(text(j["ended_recently"][0]["ended_at"]), "08:10");
    j = parsed(tool(R"({"action":"add_time","minutes":2,"label":"food"})", T0 + 900));
    CHECK_EQ(text(j["restarted"]["label"]), "Food out of the oven");
    CHECK_EQ(timer_end("Food out of the oven"), T0 + 1020);
    // Without a label, much later, the only running timer is meant.
    alarm.poll(T0 + 1020, 3000, false, false);
    alarm.stop();
    j = parsed(tool(R"({"action":"add_time","minutes":1})", T0 + 1300));
    CHECK_EQ(text(j["extended"]["label"]), "Study");
    // Cancelling after it ended says so.
    const std::string r = tool(R"({"action":"cancel","label":"food"})", T0 + 1300);
    CHECK(is_error(r) && mentions(r, "Food out of the oven timer already ended at 08:17"));
    CHECK(timer_id("Study") != 0);
}

void test_more_time_which_timer() {
    // "Give it five more minutes" with a label that names no timer in particular.
    reset_store();
    Alarm alarm;
    tool(R"({"action":"start","minutes":5,"label":"Food out of the oven"})", T0);
    tool(R"({"action":"start","minutes":30,"label":"Study"})", T0);
    alarm.poll(T0 + 300, 1000, false, false);
    alarm.stop();
    json j = parsed(tool(R"({"action":"add_time","minutes":5,"label":"the timer"})", T0 + 310));
    CHECK_EQ(text(j["restarted"]["label"]), "Food out of the oven");
    CHECK_EQ(timer_end("Study"), T0 + 1800);

    // Two pasta timers, the first has rung: "pasta" means that one, "pasta 2" the one still running.
    reset_store();
    tool(R"({"action":"start","minutes":8,"label":"Pasta"})", T0);
    j = parsed(tool(R"({"action":"start","minutes":10,"label":"Pasta"})", T0));
    CHECK_EQ(text(j["started"]["label"]), "Pasta 2");
    alarm.poll(T0 + 480, 2000, false, false);
    alarm.stop();
    j = parsed(tool(R"({"action":"add_time","minutes":5,"label":"pasta"})", T0 + 490));
    CHECK_EQ(text(j["restarted"]["label"]), "Pasta");
    CHECK_EQ(timer_end("Pasta"), T0 + 790);
    CHECK_EQ(timer_end("Pasta 2"), T0 + 600);
    j = parsed(tool(R"({"action":"add_time","minutes":1,"label":"pasta 2"})", T0 + 495));
    CHECK_EQ(text(j["extended"]["label"]), "Pasta 2");
    CHECK_EQ(timer_end("Pasta 2"), T0 + 660);
}

// The alarm's player, against a stand-in for pw-cat that keeps what it is sent: nothing is heard.
void test_alarm_player() {
    const std::string dir = std::string(getenv("XDG_CACHE_HOME")) + "/stand-in";   // set by main()
    const std::string out = dir + "/received.raw", log = dir + "/started.log";
    util::make_dirs(dir);
    // Each start of the stand-in notes its process id. It keeps what it is sent or, hung, reads
    // nothing at all. The search path holds its folder alone, so the real player can never be the
    // one that starts, even should the stand-in fail to.
    auto stand_in = [&](bool hung) {
        return util::write_file_atomic(dir + "/pw-cat", "#!/bin/sh\nPATH=/usr/bin:/bin\necho $$ >> '" + log + "'\n" +
                                                            (hung ? std::string("exec sleep 600\n") : "exec cat >> '" + out + "'\n"),
                                       0755);
    };
    if (!stand_in(false) || access((dir + "/pw-cat").c_str(), X_OK) != 0) {
        check(false, "the stand-in player could be written", __LINE__);
        return;
    }
    setenv("PATH", dir.c_str(), 1);
    // SIGPIPE is left as it is by default (the app ignores it): cutting a ring off while its thread
    // writes to the player must not end the process wherever that is not done.
    signal(SIGPIPE, SIG_DFL);
    auto received = [&] {
        std::string raw;
        util::read_file(out, raw);
        return raw;
    };
    auto started = [&] {                                           // the stand-in's process ids, oldest first
        std::string text;
        util::read_file(log, text);
        std::vector<pid_t> pids;
        for (const std::string &line : util::split(text, '\n'))
            if (!line.empty()) pids.push_back(static_cast<pid_t>(atol(line.c_str())));
        return pids;
    };
    auto gone = [](pid_t pid) { return kill(pid, 0) != 0 && errno == ESRCH; };
    auto wait_for = [](const std::function<bool()> &done, int ms) {
        for (int waited = 0; waited < ms && !done(); waited += 20) usleep(20 * 1000);
        return done();
    };
    const size_t burst_bytes = audio::alarm_burst(24000).size() * 2, chunk_bytes = 24000 / 20 * 2;
    auto loud = [](const std::string &raw, size_t from, size_t to) {   // any sample not silent
        for (size_t i = from; i + 1 < std::min(to, raw.size()); i += 2)
            if (raw[i] != 0 || raw[i + 1] != 0) return true;
        return false;
    };
    audio::AlarmSound &player = audio::AlarmSound::get();

    // While the assistant speaks, only the first burst plays; then silence until it has finished.
    std::atomic<bool> speaking{true};
    player.start([&] { return speaking.load(); });
    CHECK(player.active());
    CHECK(wait_for([&] { return received().size() >= burst_bytes + 20000; }, 5000));
    std::string raw = received();
    CHECK(loud(raw, 0, burst_bytes));
    CHECK(!loud(raw, burst_bytes, raw.size()));
    speaking = false;                                              // the words are over: it rings again
    CHECK(wait_for([&] { return loud(received(), burst_bytes, std::string::npos); }, 5000));
    CHECK(player.active());

    player.stop();                                                 // a key: it stops at once
    CHECK(wait_for([&] { return !player.active(); }, 2000));
    const size_t after = received().size();
    usleep(300 * 1000);
    CHECK_EQ(received().size(), after);                            // and nothing more reaches the player

    // Speech that goes on and on (a slow service, a long answer) holds the alarm back only so long:
    // after give_way_ms of quiet in all, it rings on whatever is being said.
    unlink(out.c_str());
    player.start([] { return true; }, 65000, 600);
    CHECK(wait_for([&] { return received().size() >= burst_bytes + 12 * chunk_bytes; }, 5000));
    CHECK(!loud(received(), burst_bytes, burst_bytes + 12 * chunk_bytes));   // 600 ms of quiet after the first burst
    CHECK(wait_for([&] { return loud(received(), burst_bytes + 12 * chunk_bytes, std::string::npos); }, 3000));
    player.stop_and_wait();
    // What the app gives way for: about a short sentence ("Time's up: Food out of the oven."), no more.
    CHECK(audio::AlarmSound::kGiveWayMs >= 3000 && audio::AlarmSound::kGiveWayMs <= 6000);

    unlink(out.c_str());
    player.start({});                                              // leaving the app while it rings
    CHECK(wait_for([&] { return received().size() > 0; }, 5000));
    player.stop_and_wait();
    CHECK(!player.active());
    const size_t left = received().size();
    usleep(300 * 1000);
    CHECK_EQ(received().size(), left);                             // the player is gone with it
    for (pid_t pid : started()) CHECK(gone(pid));

    // A player that has hung (it reads nothing) holds up neither the next ring nor leaving the app:
    // start() and stop() never wait for a player.
    CHECK(stand_in(true));
    unlink(log.c_str());
    player.start({});
    usleep(1600 * 1000);                                           // its pipe is full: the ring's thread is stuck
    auto t = std::chrono::steady_clock::now();
    player.start({});                                              // the next timer ends
    CHECK(ms_since(t) < 150);
    auto first_gone = [&] {
        const std::vector<pid_t> pids = started();
        return !pids.empty() && gone(pids[0]);
    };
    CHECK(wait_for([&] { return started().size() == 2; }, 3000));
    CHECK(wait_for(first_gone, 1000));                             // the hung one has been ended
    t = std::chrono::steady_clock::now();
    player.stop_and_wait();
    CHECK(ms_since(t) < 250);
    CHECK(!player.active());
    for (pid_t pid : started()) CHECK(gone(pid));
    CHECK(stand_in(false));

    // As the app wires it: a timer that ends while another rings starts the sound again, with a
    // player and a whole ring of its own, rather than living on what is left of the first one's.
    reset_store();
    unlink(log.c_str());
    unlink(out.c_str());
    Alarm alarm(Alarm::Sound{[] { audio::AlarmSound::get().start({}); }, [] { audio::AlarmSound::get().stop(); }});
    tool(R"({"action":"start","minutes":1,"label":"A"})", T0);
    tool(R"({"action":"start","seconds":90,"label":"B"})", T0);
    alarm.poll(T0 + 60, 1000, false, false);
    CHECK(wait_for([&] { return started().size() == 1 && received().size() > 0; }, 3000));
    alarm.poll(T0 + 90, 31000, false, false);
    CHECK(wait_for([&] { return started().size() == 2; }, 3000));
    CHECK(wait_for(first_gone, 1000));
    const size_t now_size = received().size();
    CHECK(wait_for([&] { return received().size() > now_size; }, 2000));   // the new one plays
    alarm.stop();
    player.stop_and_wait();
    for (pid_t pid : started()) CHECK(gone(pid));
}

void test_alarm_sound_pattern() {
    const std::vector<int16_t> burst = audio::alarm_burst(24000);
    CHECK_EQ(burst.size(), 24000 * 110 / 100);                     // 4 x (100 + 50) ms, then 500 ms
    int peak = 0;
    for (int16_t s : burst) peak = std::max(peak, std::abs(static_cast<int>(s)));
    CHECK(peak > 12000 && peak < 32767);                           // loud, and not clipped
    CHECK(burst.front() == 0 && burst.back() == 0);                // starts and ends silent: no click
}

void run(const char *name, void (*fn)()) {
    g_test = name;
    fn();
}

}  // namespace

int main() {
    // Whoever runs this, by the runner or by hand, everything it writes (the store's files, the
    // stand-in player) goes to a folder of its own, removed at the end: the tests start by deleting
    // the store's files, and must never reach the real calendar, memory or reminders.
    const char *tmp = getenv("TMPDIR");
    std::string pattern = std::string(tmp && *tmp ? tmp : "/tmp") + "/timers-test-XXXXXX";
    if (!mkdtemp(&pattern[0])) {
        perror("timers test: a private folder could not be made");
        return 1;
    }
    const std::string root = pattern;
    setenv("HOME", root.c_str(), 1);
    setenv("XDG_CONFIG_HOME", (root + "/config").c_str(), 1);
    setenv("XDG_DATA_HOME", (root + "/data").c_str(), 1);
    setenv("XDG_CACHE_HOME", (root + "/cache").c_str(), 1);
    setenv("TZ", "UTC", 1);
    tzset();
    util::make_dirs(util::data_dir());
    run("countdown text", test_countdown_text);
    run("start, list, cancel, add_time", test_start_list_cancel_add_time);
    run("tool errors", test_tool_errors);
    run("label matching", test_label_matching);
    run("ambiguity reported", test_ambiguity_reported);
    run("spelt-out numbers", test_spelt_out_numbers);
    run("survives a restart", test_survives_restart);
    run("old reminders.json", test_old_reminders_file);
    run("reminders unchanged", test_reminders_unchanged);
    run("cap of ten", test_cap);
    run("missed on restart", test_missed_on_restart);
    run("late in this run", test_late_in_this_run);
    run("clock set while running", test_clock_set_while_running);
    run("clock set as a timer starts", test_clock_set_as_a_timer_starts);
    run("tool waits for the loop", test_tool_waits_for_the_loop);
    run("alarm: any key", test_alarm_rings_and_any_key_silences);
    run("alarm: TAB", test_alarm_tab_silences_and_listens);
    run("alarm: 60 s", test_alarm_stops_after_a_minute);
    run("alarm: before the file", test_rings_before_the_file_is_written);
    run("alarm: waits while recording", test_alarm_waits_while_recording);
    run("alarm: muted", test_alarm_quiet_when_muted);
    run("music hold", test_music_hold);
    run("more time after the alarm", test_more_time_after_the_alarm);
    run("more time: which timer", test_more_time_which_timer);
    run("damaged records", test_damaged_timer_records);
    run("full disk", test_full_disk);
    run("alarm sound", test_alarm_sound_pattern);
    run("alarm player", test_alarm_player);
    std::error_code gone;
    std::filesystem::remove_all(root, gone);
    printf("timers: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
