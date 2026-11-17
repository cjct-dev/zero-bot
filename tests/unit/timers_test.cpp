// Device-free tests of the timers: the timer tool as the model sees it, the timers kept in the
// store (and old data files), the countdown text and the alarm (ringing, keys, the time limit,
// waiting while the microphone is open, holding the music). No screen, no sound, no network.
//
// Built and run by tests/run_timer_tests.sh, which points XDG_DATA_HOME at an empty temporary
// folder and sets TZ=UTC.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <string>
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
json parsed(const std::string &result) { return json::parse(result, nullptr, false); }
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
    Alarm::Sound sound() { return {[this] { ++starts; }, [this] { ++stops; }}; }
};

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
    std::vector<timers::Ended> e = alarm.poll(T0, 3000, false, false);
    CHECK(e.size() == 1 && e[0].missed);
    CHECK(!alarm.ringing());
    Store::get().add_timer("Edge 2", 60, T0 - 179);
    e = alarm.poll(T0, 3000, false, false);
    CHECK(e.size() == 1 && !e[0].missed);
    CHECK(alarm.ringing());
    alarm.stop();
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

    // A timer that ends while another rings joins it, with a full minute of its own, and one sound.
    reset_store();
    tool(R"({"action":"start","minutes":1,"label":"A"})", T0);
    tool(R"({"action":"start","seconds":90,"label":"B"})", T0);
    alarm.poll(T0 + 60, 1000, false, false);
    alarm.poll(T0 + 90, 31000, false, false);
    CHECK(alarm.labels() == std::vector<std::string>({"A", "B"}));
    CHECK_EQ(fs.starts, 2);                                        // one for this ring (one before)
    CHECK(!alarm.tick(61000));
    CHECK(alarm.tick(91000));
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
    setenv("TZ", "UTC", 1);
    tzset();
    util::make_dirs(util::data_dir());
    run("countdown text", test_countdown_text);
    run("start, list, cancel, add_time", test_start_list_cancel_add_time);
    run("tool errors", test_tool_errors);
    run("label matching", test_label_matching);
    run("ambiguity reported", test_ambiguity_reported);
    run("survives a restart", test_survives_restart);
    run("old reminders.json", test_old_reminders_file);
    run("reminders unchanged", test_reminders_unchanged);
    run("cap of ten", test_cap);
    run("missed on restart", test_missed_on_restart);
    run("alarm: any key", test_alarm_rings_and_any_key_silences);
    run("alarm: TAB", test_alarm_tab_silences_and_listens);
    run("alarm: 60 s", test_alarm_stops_after_a_minute);
    run("alarm: waits while recording", test_alarm_waits_while_recording);
    run("alarm: muted", test_alarm_quiet_when_muted);
    run("music hold", test_music_hold);
    run("more time after the alarm", test_more_time_after_the_alarm);
    run("more time: which timer", test_more_time_which_timer);
    run("alarm sound", test_alarm_sound_pattern);
    printf("timers: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
