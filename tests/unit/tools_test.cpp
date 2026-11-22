// Device-free tests of what the assistant's tools stand on: the calculator (deep nesting, which
// used to overflow the stack and end the app, and the percent forms that read one way and compute
// another), dates and clock times that do not exist, time zones that are folders or tables rather
// than zones, and running a helper program (cancelling it, bounding the wait for it, and giving it
// no open file of the app's but its three standard streams, also where close_range is refused).
//
// Built and run by tests/run_unit.sh. Needs /bin/sh, sleep and ls; writes nothing outside the
// temporary folder it is given.
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "agent/calc.h"
#include "core/util.h"
#include "platform/proc.h"

namespace {

int g_checks = 0, g_failures = 0;
const char *g_test = "";

void check(bool ok, const char *what, int line) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    fprintf(stderr, "FAIL %s, line %d: %s\n", g_test, line, what);
}

void check_has(const std::string &got, const std::string &want, const char *what, int line) {
    ++g_checks;
    if (got.find(want) != std::string::npos) return;
    ++g_failures;
    fprintf(stderr, "FAIL %s, line %d: %s is \"%s\", expected it to contain \"%s\"\n", g_test, line, what, got.c_str(), want.c_str());
}

#define CHECK(cond) check((cond), #cond, __LINE__)
#define CHECK_HAS(got, want) check_has((got), (want), #got, __LINE__)

std::string calc_text(const std::string &expr) {
    double v = 0;
    std::string err;
    return calc::evaluate(expr, v, err) ? calc::format(v) : "ERROR " + err;
}

double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

void test_calculator() {
    g_test = "calculator";
    CHECK(calc_text("(84.50 + 12) * 1.125 / 4") == "27.140625");
    CHECK(calc_text("2^3^2") == "512");
    CHECK(calc_text("-2^2") == "-4");
    CHECK(calc_text("10 % 3") == "1");
    CHECK(calc_text("50% * 80") == "40");
    CHECK(calc_text("1,234.5 * 2") == "2469");
    CHECK(calc_text("200*1.1") == "220");
    // Deep nesting is refused with a sentence, not a crashed app (it overflowed the stack at 20 000).
    for (const std::string &deep : {std::string(50000, '(') + "1" + std::string(50000, ')'), std::string(50000, '-') + "1"}) {
        CHECK_HAS(calc_text(deep), "nested too deeply");
    }
    std::string pow_chain;
    for (int i = 0; i < 30000; ++i) pow_chain += "2^";
    CHECK_HAS(calc_text(pow_chain + "1"), "nested too deeply");
    CHECK(calc_text(std::string(90, '(') + "7" + std::string(90, ')')) == "7");   // ordinary nesting still works
    // "200 + 10%" computes 200.1; asked for a tip, that is a confident wrong answer.
    CHECK_HAS(calc_text("200 + 10%"), "ambiguous: to add 10% write 200*1.1, or for 10% of it alone 200*0.1");
    CHECK_HAS(calc_text("84.50 - 12.5%"), "to take off 12.5% write 84.50*0.875");
    CHECK_HAS(calc_text("(1 + 2) + 10%"), "write (1 + 2)*1.1");
    CHECK_HAS(calc_text("1 + 2 + 10%"), "write (1 + 2)*1.1");
    CHECK_HAS(calc_text("15% of 80"), "write '15% of 80' as 0.15*80");
    CHECK_HAS(calc_text("1e999"), "too large");
    CHECK_HAS(calc_text("\x01"), "control character");
    CHECK_HAS(calc_text("1/0"), "division by zero");
}

void test_dates() {
    g_test = "dates";
    time_t t = 0;
    CHECK(util::parse_local("2027-03-14 18:30", t));
    CHECK(util::parse_local("2027-03-14", t));
    CHECK(util::parse_local("2028-02-29 09:00", t));   // a leap day
    CHECK(util::parse_local("2027-12-31T23:59:59", t));
    // mktime would make these into other days without a word.
    CHECK(!util::parse_local("2027-02-29 09:00", t));
    CHECK(!util::parse_local("2027-02-31", t));
    CHECK(!util::parse_local("2027-04-31 10:00", t));
    CHECK(!util::parse_local("2027-01-10 25:00", t));
    CHECK(!util::parse_local("2027-01-10 24:00", t));
    CHECK(!util::parse_local("2027-01-10 10:99", t));
    CHECK(!util::parse_local("2027-01-10 10:30:75", t));
    CHECK(!util::parse_local("tomorrow at 5", t));
    // The typed form, which goes through parse_local, is unchanged.
    CHECK(util::parse_typed("14/03/2027 18:30", t) && util::format_time(t, "%Y-%m-%d %H:%M") == "2027-03-14 18:30");
    CHECK(!util::parse_typed("31/02/2027", t));
}

void test_zones() {
    g_test = "time zones";
    if (access("/usr/share/zoneinfo/Europe/London", R_OK) != 0) {
        fprintf(stderr, "(no zone database here: the time zone checks were skipped)\n");
        return;
    }
    CHECK(util::timezone_exists("Europe/London"));
    CHECK(util::timezone_exists("Asia/Tokyo"));
    CHECK(util::timezone_exists("UTC"));
    // Folders named like countries with several zones, and the database's own tables: TZ set to
    // any of these silently meant UTC.
    for (const char *bad : {"Australia", "Brazil", "Canada", "US", "Europe", "zone.tab", "iso3166.tab", "../../etc/passwd",
                            "/etc/passwd", "", "Nowhere/Special"})
        check(!util::timezone_exists(bad), bad, __LINE__);
}

void test_processes(const std::string &tmp) {
    g_test = "helper programs";
    // A descriptor the app holds open without close-on-exec does not reach the child.
    const std::string path = tmp + "/held-open";
    const int fd = open(path.c_str(), O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 3);
    proc::Result r = proc::run({"sh", "-c", "ls /proc/self/fd"}, "", 5000);
    CHECK(r.ok());
    // The shell's own 0, 1, 2 and the one ls opens to read the folder: nothing numbered like ours
    // (ls's own descriptor is the lowest free one, 3, so look for ours only when it is above that).
    if (fd > 3) CHECK(r.out.find("\n" + std::to_string(fd) + "\n") == std::string::npos);
    int count = 0;
    for (const auto &line : util::split(util::trim(r.out), '\n')) count += !line.empty();
    CHECK(count <= 4);
    close(fd);

    // The time limit holds, and the program is gone afterwards.
    auto t0 = std::chrono::steady_clock::now();
    r = proc::run({"sleep", "30"}, "", 300);
    CHECK(r.timed_out && !r.ok());
    CHECK(ms_since(t0) < 2000);

    // A program that closes its output but goes on running is not waited for beyond the limit.
    t0 = std::chrono::steady_clock::now();
    r = proc::run({"sh", "-c", "exec >&- 2>&-; sleep 30"}, "", 400);
    CHECK(r.timed_out);
    CHECK(ms_since(t0) < 2000);

    // Cancelling stops it within a fraction of a second.
    std::atomic<bool> cancel{false};
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        cancel = true;
    });
    t0 = std::chrono::steady_clock::now();
    r = proc::run({"sleep", "30"}, "", 20000, 4096, false, &cancel);
    canceller.join();
    CHECK(r.cancelled && !r.ok());
    CHECK(ms_since(t0) < 1500);
    // A flag already set: nothing is started.
    r = proc::run({"sh", "-c", "echo started"}, "", 5000, 4096, false, &cancel);
    CHECK(r.cancelled && r.out.empty());

    // Output beyond the limit is cut, and says so.
    r = proc::run({"sh", "-c", "i=0; while [ $i -lt 200 ]; do echo 0123456789; i=$((i+1)); done"}, "", 5000, 100);
    CHECK(r.ok() && r.truncated && r.out.size() == 100);
    r = proc::run({"sh", "-c", "echo short"}, "", 5000, 100);
    CHECK(r.ok() && !r.truncated && r.out == "short\n");
    // Exit codes still come through.
    r = proc::run({"sh", "-c", "exit 3"}, "", 5000);
    CHECK(r.code == 3 && !r.timed_out);
}

#ifdef SYS_close_range
constexpr int kHigh = 5000;

// A child of the test holds descriptor kHigh, refuses itself the system calls `refused` with a
// seccomp filter (ENOSYS, as a sandbox or an older kernel would), lowers its soft limit on open files
// to `soft` (below kHigh, as a program may after opening it), and runs a helper program. What the
// helper saw: 0 not kHigh, 1 kHigh, 2 the test could not be set up here, 3 the helper did not run.
int helper_sees_high(const std::string &tmp, const std::vector<long> &refused, rlim_t soft) {
    const pid_t pid = fork();
    if (pid == 0) {
        rlimit lim{};
        if (getrlimit(RLIMIT_NOFILE, &lim) != 0) _exit(2);
        lim.rlim_cur = lim.rlim_max == RLIM_INFINITY ? 8192 : std::min<rlim_t>(lim.rlim_max, 8192);
        if (setrlimit(RLIMIT_NOFILE, &lim) != 0) _exit(2);
        const int fd = open((tmp + "/held-high").c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
        if (fd < 0 || dup2(fd, kHigh) != kHigh) _exit(2);   // dup2 leaves kHigh without close-on-exec
        close(fd);
        lim.rlim_cur = soft;
        if (setrlimit(RLIMIT_NOFILE, &lim) != 0) _exit(2);
        std::vector<sock_filter> filter{BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr))};
        for (const long call : refused) {
            filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<unsigned>(call), 0, 1));
            filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (ENOSYS & SECCOMP_RET_DATA)));
        }
        filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
        sock_fprog prog{static_cast<unsigned short>(filter.size()), filter.data()};
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) _exit(2);
        if (syscall(SYS_close_range, 4000U, 4001U, 0U) == 0 || errno != ENOSYS) _exit(2);
        // Asked by name, not by listing the folder: the helper inherits the filter too.
        const proc::Result r = proc::run({"sh", "-c", "test -e /proc/self/fd/" + std::to_string(kHigh) + " && echo held"}, "", 5000);
        if (r.timed_out || (r.code != 0 && r.code != 1)) _exit(3);
        _exit(r.out.find("held") == std::string::npos ? 0 : 1);
    }
    int status = 0;
    if (pid <= 0 || waitpid(pid, &status, 0) != pid) return 3;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 3;
}
#endif

// Where close_range is missing (an older kernel) or refused (a sandbox's filter), the fallback that
// stands in for it must reach every descriptor the process holds: not stop at 4096, nor at the soft
// limit on open files, which may have been lowered below a descriptor already open (fix round 2, C7).
// It closes what /proc/self/fd lists, or, where that cannot be read, every number up to the hard limit.
void test_descriptors_without_close_range(const std::string &tmp) {
    g_test = "helper programs without close_range";
#ifdef SYS_close_range
    rlimit lim{};
    if (getrlimit(RLIMIT_NOFILE, &lim) != 0 || (lim.rlim_max != RLIM_INFINITY && lim.rlim_max <= kHigh)) {
        printf("tools: note: the open-file limit is too low here to hold descriptor %d; that check was skipped\n", kHigh);
        return;
    }
    struct Case {
        const char *what;
        std::vector<long> refused;
        rlim_t soft;
    };
    const Case cases[] = {
        {"close_range refused", {SYS_close_range}, 8192},
        {"close_range refused, the soft limit lowered below the descriptor", {SYS_close_range}, 1024},
        {"close_range and the listing of /proc/self/fd refused, the soft limit lowered", {SYS_close_range, SYS_getdents64}, 1024},
    };
    for (const Case &c : cases) {
        const int code = helper_sees_high(tmp, c.refused, c.soft);
        if (code == 2) {
            printf("tools: note: system calls could not be refused here (no seccomp); the check \"%s\" was skipped\n", c.what);
            continue;
        }
        g_test = c.what;
        check(code == 0, code == 1 ? "descriptor 5000 reached the helper program" : "the helper program did not run", __LINE__);
    }
#else
    printf("tools: note: this system's headers have no close_range; that check was skipped\n");
#endif
}

}  // namespace

int main(int argc, char **argv) {
    const std::string tmp = argc > 1 ? argv[1] : "/tmp";
    test_calculator();
    test_dates();
    test_zones();
    test_processes(tmp);
    test_descriptors_without_close_range(tmp);
    printf("tools: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
