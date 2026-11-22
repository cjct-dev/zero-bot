// Device helpers that do not depend on the graphics library.
#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>

#include <fcntl.h>
#include <unistd.h>

#include "platform/display.h"
#include "platform/sys.h"

namespace sys {

unsigned long now_ms() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return static_cast<unsigned long>(duration_cast<milliseconds>(steady_clock::now() - t0).count());
}

}  // namespace sys

namespace display {

void set_brightness(int percent) {
#if !ZB_DESKTOP
    if (percent < 5) percent = 5;
    if (percent > 100) percent = 100;
    std::ofstream f("/sys/class/backlight/backlight/brightness");
    if (f) f << percent;
#else
    (void)percent;
#endif
}

void backlight_off() {
#if !ZB_DESKTOP
    // 0 switches the PWM backlight off (the launcher's screen was found at 0 too). Not bl_power: the
    // launcher writes only the brightness, so a bl_power left off by an app that crashed would keep
    // the screen dark until a restart.
    std::ofstream f("/sys/class/backlight/backlight/brightness");
    if (f) f << 0;
#endif
}

namespace {
#if !ZB_DESKTOP
std::atomic<const char *> g_backlight_file{"/sys/class/backlight/backlight/brightness"};
#else
std::atomic<const char *> g_backlight_file{nullptr};
#endif
std::atomic<int> g_restore{-1};   // lock-free on every target, so the handler may read it
const int kFatal[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGTRAP, SIGSYS, SIGQUIT, SIGHUP};

// Runs in a process that is going down: only calls that are safe in a signal handler (open, write,
// close, raise), and no memory allocated.
void on_fatal(int sig) {
    const int percent = g_restore.load();
    const char *file = g_backlight_file.load();
    if (percent >= 5 && percent <= 100 && file) {
        char text[3];
        int n = 0;
        if (percent == 100) text[n++] = '1';
        if (percent >= 10) text[n++] = static_cast<char>('0' + percent / 10 % 10);
        text[n++] = static_cast<char>('0' + percent % 10);
        const int fd = open(file, O_WRONLY | O_TRUNC | O_CLOEXEC);
        if (fd >= 0) {
            const ssize_t written = write(fd, text, static_cast<size_t>(n));
            (void)written;
            close(fd);
        }
    }
    // The handler was reset as it ran (SA_RESETHAND): the signal now does what it would have done.
    raise(sig);
}
}  // namespace

void restore_after_crash(int percent) {
    g_restore = percent >= 5 && percent <= 100 ? percent : -1;
    static bool installed = false;
    if (installed) return;
    installed = true;
    // A stack of its own for the handler, so that a stack overflow on this thread (the UI's) still
    // runs it. Other threads have none: a stack overflow there ends the app without the handler.
    static char alt[64 * 1024];
    stack_t ss{};
    ss.ss_sp = alt;
    ss.ss_size = sizeof alt;
    const bool own_stack = sigaltstack(&ss, nullptr) == 0;
    struct sigaction sa {};
    sa.sa_handler = on_fatal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND | SA_NODEFER | (own_stack ? SA_ONSTACK : 0);
    // A signal the app was started with ignored (a hang-up, under nohup or a parent that ignores
    // it) stays ignored: the handler ends the process, which the signal did not do before.
    for (const int sig : kFatal) {
        struct sigaction was {};
        if (sigaction(sig, nullptr, &was) == 0 && was.sa_handler == SIG_IGN) continue;
        sigaction(sig, &sa, nullptr);
    }
}

namespace detail {
void set_backlight_file(const char *path) { g_backlight_file = path; }
}  // namespace detail

}  // namespace display
