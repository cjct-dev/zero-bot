// audio::Speaker written to on one thread and stopped on another, as the speech queue does: its
// thread writes each sentence while SpeechQueue::stop() (ESC, TAB) runs on the UI thread. Against a
// stand-in pw-cat that keeps what it is sent: nothing is heard.
//
// Built and run by tests/run_unit.sh, linked with -Wl,--wrap=write: `write` is wrapped so that the
// writer thread can be held at the one point where a busy processor may preempt it, between taking the
// pipe's descriptor and the system call. Everything it writes goes in a folder of its own under TMPDIR.
#include "platform/audio.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <functional>
#include <random>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "core/util.h"

// ---------------------------------------------------------------- the write hook
extern "C" ssize_t __real_write(int fd, const void *buf, size_t n);

namespace hook {
thread_local bool writer = false;           // this thread is the speaker's writer under test
std::atomic<bool> hold_next{false};         // hold the writer's next write until `go`
std::atomic<int> held_fd{-1};               // the descriptor the held write is for
std::atomic<bool> go{false};
// Before each of the writer's writes, while set: a pause of up to 300 us, and now and then (one write in
// thirty) a long one of 5 to 30 ms, as when a busy processor preempts the thread at that point.
std::atomic<bool> preempt{false};
thread_local std::minstd_rand rng{12345};
}  // namespace hook

extern "C" ssize_t __wrap_write(int fd, const void *buf, size_t n) {
    if (hook::writer) {
        if (hook::hold_next.exchange(false)) {
            hook::held_fd = fd;
            for (int i = 0; i < 300 && !hook::go; ++i) usleep(10 * 1000);   // at most 3 s
        }
        if (hook::preempt) {
            const unsigned r = static_cast<unsigned>(hook::rng());
            usleep(r % 30 == 0 ? 5000 + (r >> 8) % 25000 : (r >> 8) % 300);
        }
    }
    return __real_write(fd, buf, n);
}

namespace {

int g_checks = 0, g_failures = 0;
void check(bool ok, const std::string &what) {
    ++g_checks;
    if (!ok) { ++g_failures; printf("FAIL %s\n", what.c_str()); fflush(stdout); }
}

long long ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count();
}
bool wait_for(const std::function<bool()> &done, int ms) {
    for (int waited = 0; waited < ms && !done(); waited += 10) usleep(10 * 1000);
    return done();
}
long size_of(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0 ? static_cast<long>(st.st_size) : -1; }

std::string g_dir, g_received, g_log;

// The stand-in player: notes its process id; keeps what it is sent, or (hung) reads nothing at all.
void stand_in(bool hung) {
    util::write_file_atomic(g_dir + "/pw-cat", "#!/bin/sh\nPATH=/usr/bin:/bin\necho $$ >> '" + g_log + "'\n" +
                                                   (hung ? std::string("exec sleep 600\n") : "exec cat >> '" + g_received + "'\n"),
                            0755);
}
std::vector<pid_t> started() {
    std::string text;
    util::read_file(g_log, text);
    std::vector<pid_t> pids;
    for (const std::string &line : util::split(text, '\n'))
        if (!line.empty()) pids.push_back(static_cast<pid_t>(atol(line.c_str())));
    return pids;
}
bool all_gone() {
    for (pid_t pid : started())
        if (!(kill(pid, 0) != 0 && errno == ESRCH)) return false;
    return true;
}
std::string pattern(size_t bytes, unsigned char seed) {
    std::string s(bytes, '\0');
    for (size_t i = 0; i < bytes; ++i) s[i] = static_cast<char>(seed + i * 7 + (i >> 8));
    return s;
}

// stop() on another thread while the writer has taken the pipe's descriptor and not yet written:
// the number the pipe had is free again, and the next file anything in the app opens can get it. A
// write that then goes out must not land in that file.
void test_stop_while_a_write_is_about_to_go_out() {
    stand_in(false);
    unlink(g_received.c_str());
    audio::Speaker sp;
    check(sp.begin(24000), "the stand-in player starts");
    const std::string pcm = pattern(4800, 1);   // 100 ms at 24 kHz
    hook::go = false;
    hook::held_fd = -1;
    hook::hold_next = true;
    std::atomic<bool> done{false};
    std::thread w([&] {
        hook::writer = true;
        sp.write(pcm.data(), pcm.size());
        done = true;
    });
    check(wait_for([] { return hook::held_fd >= 0; }, 3000), "the writer reaches its write");
    const auto t = std::chrono::steady_clock::now();
    sp.stop();                                                     // SpeechQueue::stop(), on the UI thread
    check(ms_since(t) < 1000, "stop() does not wait for the writer (" + std::to_string(ms_since(t)) + " ms)");
    // Another thread opens a file now (the store saving the calendar, the next request's socket). The
    // lowest free number is what it gets; here the number the writer holds, if that is free.
    const std::string victim = g_dir + "/calendar.json";
    const int v = open(victim.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    const int taken = fcntl(v, F_DUPFD_CLOEXEC, hook::held_fd.load());
    close(v);
    hook::go = true;                                               // the writer is scheduled again
    check(wait_for([&] { return done.load(); }, 3000), "the writer returns");
    w.join();
    check(size_of(victim) == 0, "nothing the speaker was given went into the file opened after stop() (" +
                                    std::to_string(size_of(victim)) + " bytes did; the writer's descriptor " +
                                    std::to_string(hook::held_fd.load()) + (taken == hook::held_fd ? " was" : " was not") +
                                    " free for it)");
    if (taken >= 0) close(taken);
    check(wait_for(all_gone, 2000), "no stand-in player is left");
}

// Many rounds as the speech queue runs them: one thread writes sentence after sentence while another
// stops at a random moment, and a third opens files all the while, as the rest of the app does (a few
// at a time, so that a number freed by stop() is soon taken). The writer is preempted at random
// before its writes, as on a busy processor, far more often than a device would do it.
void test_stress() {
    stand_in(false);
    unlink(g_log.c_str());
    hook::preempt = true;
    std::minstd_rand rng(7);
    int leaked_rounds = 0, slow_stops = 0, stuck = 0;
    long long worst_stop = 0;
    const int kRounds = 150;
    for (int round = 0; round < kRounds; ++round) {
        unlink(g_received.c_str());
        audio::Speaker sp;
        if (!sp.begin(24000)) { check(false, "round " + std::to_string(round) + ": the player starts"); break; }
        std::atomic<bool> quit{false}, done{false};
        std::thread w([&] {
            hook::writer = true;
            const std::string sentence = pattern(2400, 3), gap(576, '\0');
            while (!quit) {
                sp.write(sentence.data(), sentence.size());
                sp.write(gap.data(), gap.size());
            }
            done = true;
        });
        // Files opened and closed by "the rest of the app" while the speaker is stopped.
        std::atomic<bool> opening{true};
        std::atomic<long> leaked{0};
        std::thread opener([&, round] {
            while (opening) {
                std::string files[3];
                int fds[3];
                for (int k = 0; k < 3; ++k) {
                    files[k] = g_dir + "/other-" + std::to_string(round) + "-" + std::to_string(k);
                    fds[k] = open(files[k].c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
                }
                usleep(500);
                for (int k = 0; k < 3; ++k) {
                    if (fds[k] >= 0) close(fds[k]);
                    if (size_of(files[k]) > 0) leaked += size_of(files[k]);   // written to while it was open
                    unlink(files[k].c_str());
                }
            }
        });
        usleep(static_cast<useconds_t>(rng() % 20000));
        const auto t = std::chrono::steady_clock::now();
        sp.stop();
        const long long took = ms_since(t);
        worst_stop = std::max(worst_stop, took);
        if (took > 1000) ++slow_stops;
        usleep(static_cast<useconds_t>(5000 + rng() % 5000));
        quit = true;
        if (!wait_for([&] { return done.load(); }, 3000)) ++stuck;
        w.join();
        opening = false;
        opener.join();
        if (leaked > 0) ++leaked_rounds;
    }
    hook::preempt = false;
    check(leaked_rounds == 0, "in " + std::to_string(kRounds) + " rounds, sound went into another file in " +
                                  std::to_string(leaked_rounds));
    check(slow_stops == 0, "stop() never waits long (worst " + std::to_string(worst_stop) + " ms)");
    check(stuck == 0, "the writer always returns");
    check(wait_for(all_gone, 3000), "no stand-in player is left");
}

// A long write to a player that has stopped reading (its pipe is full): stop() on another thread ends
// it at once, the write returns, and the next answer's player gets its own sound and nothing else.
void test_stop_during_a_long_write() {
    stand_in(true);
    unlink(g_log.c_str());
    audio::Speaker sp;
    check(sp.begin(24000), "the hung stand-in starts");
    const std::string lots = pattern(1 << 20, 5);   // about 22 s of audio, far more than a pipe holds
    std::atomic<bool> done{false};
    std::thread w([&] {
        hook::writer = true;
        sp.write(lots.data(), lots.size());
        done = true;
    });
    usleep(300 * 1000);
    check(!done, "the write is stuck on the full pipe");
    auto t = std::chrono::steady_clock::now();
    sp.stop();
    check(ms_since(t) < 1000, "stop() returns at once (" + std::to_string(ms_since(t)) + " ms)");
    check(wait_for([&] { return done.load(); }, 1000), "the stuck write returns");
    w.join();
    check(wait_for(all_gone, 2000), "the hung player is gone");

    // The next answer, on the same speaker, as the speech queue does after a stop.
    stand_in(false);
    unlink(g_received.c_str());
    const std::string next = pattern(9600, 9);
    check(sp.begin(24000), "the next player starts");
    std::thread w2([&] {
        hook::writer = true;
        sp.write(next.data(), next.size());
        sp.finish();
        for (int i = 0; i < 100 && sp.playing(); ++i) usleep(20 * 1000);
    });
    w2.join();
    std::string got;
    util::read_file(g_received, got);
    check(got == next, "the next player gets exactly its own sound (" + std::to_string(got.size()) + " bytes)");
}

// kill() from another thread (the alarm's stop() on the UI thread) while the ring's thread writes to
// a player that has stopped reading: the write returns, and the writer reaps the player with stop().
void test_kill_frees_a_stuck_writer() {
    stand_in(true);
    unlink(g_log.c_str());
    audio::Speaker sp;
    check(sp.begin(24000), "the hung stand-in starts");
    const std::string lots = pattern(1 << 20, 11);
    std::atomic<bool> done{false};
    std::thread w([&] {
        hook::writer = true;
        sp.write(lots.data(), lots.size());
        sp.stop();
        done = true;
    });
    usleep(300 * 1000);
    const auto t = std::chrono::steady_clock::now();
    sp.kill();
    check(ms_since(t) < 100, "kill() does not wait");
    check(wait_for([&] { return done.load(); }, 1500), "the writer returns and reaps the player");
    w.join();
    check(wait_for(all_gone, 2000), "the player is gone");
}

// What the speech queue and the chimes do every time: write, finish, wait while it plays.
void test_plain_use() {
    stand_in(false);
    unlink(g_received.c_str());
    audio::Speaker sp;
    check(sp.begin(24000), "the player starts");
    std::string all;
    for (unsigned char s = 0; s < 20; ++s) {
        const std::string part = pattern(3000 + s * 97, s);
        sp.write(part.data(), part.size());
        all += part;
    }
    sp.finish();
    for (int i = 0; i < 100 && sp.playing(); ++i) usleep(20 * 1000);
    check(!sp.playing(), "the player ends once it has everything");
    std::string got;
    util::read_file(g_received, got);
    check(got == all, "every byte arrives, in order (" + std::to_string(got.size()) + " of " + std::to_string(all.size()) + ")");
    sp.write(all.data(), 100);                                     // after the player has gone: nothing, no crash
    sp.stop();
    check(wait_for(all_gone, 2000), "no stand-in player is left");
}

}  // namespace

int main() {
    // The app ignores SIGPIPE (ui/app.cpp): a write to a player that has gone fails instead.
    signal(SIGPIPE, SIG_IGN);
    const char *tmp = getenv("TMPDIR");
    std::string root = std::string(tmp && *tmp ? tmp : "/tmp") + "/speaker-test-XXXXXX";
    if (!mkdtemp(&root[0])) { perror("speaker test: a private folder could not be made"); return 1; }
    g_dir = root;
    g_received = g_dir + "/received.raw";
    g_log = g_dir + "/started.log";
    // The search path holds the stand-in's folder alone, so the real player can never be the one
    // that starts, even should the stand-in fail to.
    stand_in(false);
    setenv("PATH", g_dir.c_str(), 1);
    test_plain_use();
    test_stop_while_a_write_is_about_to_go_out();
    test_stop_during_a_long_write();
    test_kill_frees_a_stuck_writer();
    test_stress();
    const std::string clean = "/bin/rm -r '" + g_dir + "'";
    if (g_dir.find("/speaker-test-") != std::string::npos) (void)system(clean.c_str());
    printf("speaker: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
