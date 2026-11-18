// Unit tests for screen_idle.cpp: when the screen dims and goes off, what keeps it lit and what
// wakes it, and the choices of the "Screen off after" setting. No screen is needed (the --shots run
// checks what the app does with the answers: black, nothing drawn, a waking key swallowed).
// Each check prints one line on failure; the exit status is non-zero when any fails.
#include "ui/screen_idle.h"
#include "platform/display.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

using screen::Idle;
using screen::Light;

namespace {

int g_passed = 0, g_failed = 0;

void check(bool ok, const std::string &what) {
    if (ok) ++g_passed;
    else { ++g_failed; printf("FAIL %s\n", what.c_str()); }
}

const char *name(Light l) {
    switch (l) {
        case Light::Lit: return "lit";
        case Light::Dimmed: return "dimmed";
        case Light::Off: return "off";
    }
    return "?";
}

void expect(Light got, Light want, const std::string &what) {
    check(got == want, what + ": " + name(got) + ", expected " + name(want));
}

constexpr unsigned long kMin = 60000;
constexpr unsigned long kStart = 1000000;   // the app's clock when it starts: any time at all

void test_times() {
    // Five minutes (the default): dims at 45 seconds, off at five minutes, not a moment before.
    Idle idle;
    idle.use(kStart);
    expect(idle.update(kStart + 1000, false, 5), Light::Lit, "a second after a key");
    expect(idle.update(kStart + screen::kDimAfterMs - 1, false, 5), Light::Lit, "just before 45 seconds");
    expect(idle.update(kStart + screen::kDimAfterMs, false, 5), Light::Dimmed, "at 45 seconds");
    expect(idle.update(kStart + 5 * kMin - 1, false, 5), Light::Dimmed, "just before five minutes");
    expect(idle.update(kStart + 5 * kMin, false, 5), Light::Off, "at five minutes");
    expect(idle.update(kStart + 60 * kMin, false, 5), Light::Off, "an hour on");
    expect(idle.light(), Light::Off, "light() says what update() said");

    // Each choice, from a fresh use.
    for (int minutes : {1, 2, 5, 10, 15}) {
        Idle i;
        i.use(kStart);
        const std::string m = std::to_string(minutes) + " minute(s)";
        expect(i.update(kStart + minutes * kMin - 1, false, minutes), Light::Dimmed, m + ": just before");
        expect(i.update(kStart + minutes * kMin, false, minutes), Light::Off, m + ": at the time set");
    }

    // Never: it dims as before and stays dimmed.
    Idle never;
    never.use(kStart);
    expect(never.update(kStart + 24 * 60 * kMin, false, 0), Light::Dimmed, "never: a day later");
    expect(never.update(kStart + 24 * 60 * kMin, false, -3), Light::Dimmed, "a negative setting counts as never");

    // Ticks that come late (a busy second) jump straight to off when that time has passed.
    Idle late;
    late.use(kStart);
    expect(late.update(kStart + 6 * kMin, false, 5), Light::Off, "lit to off in one late tick");

    // The app starts at a clock of 0 and asks before any key: nothing goes off at once.
    Idle fresh;
    expect(fresh.update(0, false, 5), Light::Lit, "at start");
    expect(fresh.update(1000, false, 5), Light::Lit, "a second after start");
}

void test_waking() {
    Idle idle;
    idle.use(kStart);
    idle.update(kStart + 10 * kMin, false, 5);
    // A key wakes it, and says it was off (so the app swallows the key).
    expect(idle.use(kStart + 10 * kMin + 5), Light::Off, "use() says the screen was off");
    expect(idle.light(), Light::Lit, "lit after use()");
    expect(idle.use(kStart + 10 * kMin + 6), Light::Lit, "a second key finds it lit (and acts)");
    // The times count again from the waking key.
    const unsigned long woke = kStart + 10 * kMin + 6;
    expect(idle.update(woke + screen::kDimAfterMs - 1, false, 5), Light::Lit, "after waking: lit for 45 seconds");
    expect(idle.update(woke + screen::kDimAfterMs, false, 5), Light::Dimmed, "after waking: dims again");
    // A key on a dimmed screen says so: it brightens it and does what it does.
    expect(idle.use(woke + screen::kDimAfterMs + 1), Light::Dimmed, "use() on a dimmed screen says dimmed");
    expect(idle.light(), Light::Lit, "lit after a key on a dimmed screen");
}

void test_busy() {
    // Listening, a request, speech, an alarm, the breathing exercise: lit however long it lasts.
    Idle idle;
    idle.use(kStart);
    for (unsigned long t = kStart; t <= kStart + 30 * kMin; t += 1000)
        if (idle.update(t, true, 1) != Light::Lit) { check(false, "busy for half an hour stays lit"); return; }
    check(true, "busy for half an hour stays lit");
    // And the idle time counts from when it ended.
    const unsigned long ended = kStart + 30 * kMin;
    expect(idle.update(ended + screen::kDimAfterMs - 1, false, 1), Light::Lit, "lit for 45 seconds after the work ends");
    expect(idle.update(ended + kMin, false, 1), Light::Off, "off a minute after the work ends");
    // Something starting while it is off (a reminder spoken, an alarm) lights it at once.
    expect(idle.update(ended + kMin + 1000, true, 1), Light::Lit, "busy while off lights it");
}

void test_order() {
    // Only use() lights the screen: changing the setting to a longer time while dimmed does not
    // brighten it by itself, and a shorter one darkens it at the next tick.
    Idle idle;
    idle.use(kStart);
    expect(idle.update(kStart + 3 * kMin, false, 10), Light::Dimmed, "three minutes with ten set");
    expect(idle.update(kStart + 3 * kMin + 1000, false, 2), Light::Off, "the setting cut to two minutes");
    expect(idle.update(kStart + 3 * kMin + 2000, false, 15), Light::Off, "raised to fifteen: still off until a key");

    // A use stamped a moment after the time a tick read (an alert, then the second's tick that read
    // the clock just before it) is no idle time at all, not a very long one.
    Idle race;
    race.use(kStart);
    race.use(kStart + 2000);
    expect(race.update(kStart + 1999, false, 1), Light::Lit, "a use just after the tick's time");
    // An earlier time given to use() does not move the last use back.
    race.use(kStart + 500);
    expect(race.update(kStart + 2000 + screen::kDimAfterMs - 1, false, 1), Light::Lit, "use() never steps back");
}

void test_choices() {
    const std::vector<int> &c = screen::off_choices();
    check(c == std::vector<int>({0, 1, 2, 5, 10, 15}), "the choices are never, 1, 2, 5, 10 and 15 minutes");
    // Right goes up and round; left goes down and round.
    int v = 5;
    std::string seen;
    for (int i = 0; i < 6; ++i) { v = screen::step_off_choice(v, 1); seen += std::to_string(v) + " "; }
    check(seen == "10 15 0 1 2 5 ", "right from 5 goes round: " + seen);
    seen.clear();
    for (int i = 0; i < 6; ++i) { v = screen::step_off_choice(v, -1); seen += std::to_string(v) + " "; }
    check(seen == "2 1 0 15 10 5 ", "left from 5 goes round: " + seen);
    // A value from a hand-edited file moves to the nearest choice in the direction asked.
    check(screen::step_off_choice(3, 1) == 5 && screen::step_off_choice(3, -1) == 2, "3 goes to 5 or 2");
    check(screen::step_off_choice(60, 1) == 0 && screen::step_off_choice(60, -1) == 15, "60 goes round to never, or down to 15");
    check(screen::step_off_choice(-4, 1) == 1 && screen::step_off_choice(-4, -1) == 15, "a negative value counts as never");
    // What the settings row shows.
    check(screen::off_label(0) == "Never" && screen::off_label(-1) == "Never", "0 shows as Never");
    check(screen::off_label(1) == "1 minute" && screen::off_label(5) == "5 minutes" && screen::off_label(15) == "15 minutes",
          "minutes show with their unit");
}

}  // namespace

// A crash while the screen is dimmed or off puts the brightness setting back (the launcher does
// not), and the process still ends by the same signal. Each case runs in a child of its own, which
// writes to a file of the test's own instead of the backlight.
std::string crash(int sig, int restore, bool fault, const std::string &file) {
    { std::ofstream(file) << "0"; }                    // the backlight as the screen off leaves it
    const pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_DUMPABLE, 0);                     // no core files from the deliberate crashes
        display::detail::set_backlight_file(file.c_str());
        display::restore_after_crash(55);              // the screen dims ...
        display::restore_after_crash(restore);         // ... and then, say, lights again (-1)
        if (fault) {
            volatile int *nowhere = nullptr;
            *nowhere = 1;                              // a real fault, not a raised signal
        }
        raise(sig);
        _exit(0);                                      // not reached when the signal ends the process
    }
    int status = 0;
    waitpid(pid, &status, 0);
    std::ostringstream text;
    text << (WIFSIGNALED(status) ? WTERMSIG(status) : -1) << " ";
    std::ifstream in(file);
    text << in.rdbuf();
    return text.str();
}

void test_crash() {
    const char *tmp = getenv("TMPDIR");
    std::string dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/zb-backlight-XXXXXX";
    if (!mkdtemp(&dir[0])) { check(false, "a folder for the crash tests"); return; }
    const std::string file = std::string(dir) + "/brightness";
    auto ends = [](int sig, const char *backlight) { return std::to_string(sig) + " " + backlight; };
    std::string got = crash(SIGSEGV, 80, true, file);
    check(got == ends(SIGSEGV, "80"), "a fault while dimmed or off: the setting put back, still a SIGSEGV: " + got);
    got = crash(SIGABRT, 100, false, file);
    check(got == ends(SIGABRT, "100"), "an abort (a failed assertion, an uncaught exception): " + got);
    got = crash(SIGHUP, 5, false, file);
    check(got == ends(SIGHUP, "5"), "a hang-up ends it as before, the light put back first: " + got);
    got = crash(SIGBUS, -1, false, file);
    check(got == ends(SIGBUS, "0"), "lit as set (-1): nothing written: " + got);
    got = crash(SIGFPE, 7, false, file);
    check(got == ends(SIGFPE, "7"), "one digit: " + got);
    unlink(file.c_str());
    rmdir(dir.c_str());
}

int main() {
    test_times();
    test_waking();
    test_busy();
    test_order();
    test_choices();
    test_crash();
    printf("screen idle: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
