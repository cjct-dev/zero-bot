// Device-free tests of what the assistant's tools stand on: the calculator (deep nesting, which
// used to overflow the stack and end the app, and the percent forms that read one way and compute
// another), dates and clock times that do not exist, time zones that are folders or tables rather
// than zones, running a helper program (cancelling it, bounding the wait for it, and giving it
// no open file of the app's but its three standard streams, also where close_range is refused, and
// not running it at all where that cannot be made sure of), the card and ID numbers that never leave
// in a search or a memory (and the phone numbers, order numbers, dates, ISBNs, prices and postcodes
// that must), the hidden characters taken out of what is kept, and the user's yes to a memory saved
// or forgotten after text from outside (agent/consent.h; the smart-home tests have its other uses).
//
// Built and run by tests/unit/run_unit.sh. Needs /bin/sh, sleep and ls; writes nothing outside the
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
#include "agent/consent.h"
#include "agent/tools/common.h"
#include "agent/tools/sensitive.h"
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

void test_sensitive_numbers() {
    g_test = "card and ID numbers";
    const std::pair<const char *, const char *> found[] = {
        {"my card is 4111 1111 1111 1111", "a payment card number"},
        {"4111-1111-1111-1111", "a payment card number"},
        {"search for 4111111111111111 leaks", "a payment card number"},
        {"card 5500 0000 0000 0004 expires 12/27", "a payment card number"},
        {"amex 3782 822463 10005", "a payment card number"},              // 4-6-5
        {"4111 1111 1111 1111 123", "a payment card number"},             // the security code after it
        {"4111\u2009" "1111\u00a0" "1111 1111", "a payment card number"},   // thin and no-break spaces
        {"41111111\u200b11111111", "a payment card number"},             // a hidden character inside
        {"iban GB82 WEST 1234 5698 7654 32 please", "an IBAN (a bank account number)"},
        {"gb82west12345698765432", "an IBAN (a bank account number)"},
        {"DE89 3704 0044 0532 0130 00", "an IBAN (a bank account number)"},
        {"FR14 2004 1010 0505 0001 3M02 606", "an IBAN (a bank account number)"},
        {"my NI number is AB 12 34 56 C", "a UK National Insurance number"},
        {"AB123456C", "a UK National Insurance number"},
        {"ni ab 123456 c", "a UK National Insurance number"},
        {"ssn 123-45-6789", "a US Social Security number"},
        {"078-05-1120.", "a US Social Security number"},
    };
    for (const auto &f : found) check(tools::sensitive_number(f.first) == f.second, f.first, __LINE__);
    // Near misses, every one an ordinary thing to search for or remember.
    for (const char *ok : {
             "call me on 07700 900123", "+44 7700 900123", "020 7946 0958", "+44 20 7946 0958", "0044 20 7946 0958",
             "+1 415 555 0132", "+49 151 23456789", "+33 1 23 45 67 89", "+61 2 9876 5432", "+353 1 234 5678",
             "1-800-555-0199", "(415) 555-0132", "+49 30 1234 5678 90",
             "order 112-3456789-1234567", "Order #100045678912", "order number 4000 1234 5678", "ref 2024-0001-77",
             "eBay item 12-34567-89012", "tracking 1Z999AA10123456784",
             "2026-11-24", "24/11/2026", "24-11-2026 14:30", "on 24 11 2026", "from 2026-11-24 to 2026-12-01",
             "978-0-306-40615-7", "ISBN 9780306406157", "ISBN 0-306-40615-2",
             "£1,234.56", "$12,345,678.90", "1 234 567,89 €", "costs 4111.11",
             "SW1A 1AA", "EC1A 1BB", "M1 1AE", "90210-1234",
             "4111 1111 1111 1112", "4111 1111 1111", "1111 1111 1111 1111 1",           // Luhn fails; too short; no network
             "GB82 WEST 1234 5698 7654 33", "XX82 WEST 1234 5698 7654 32",                // check digits; no such country
             "QQ 12 34 56 C", "GB 12 34 56 A", "AB 12 34 56 E", "AB 12 34 56",            // never issued; no suffix
             "000-12-3456", "666-12-3456", "912-34-5678", "123-00-4567", "123-45-0000", "123-45-6789-0", "123 45 6789",
         })
        check(tools::sensitive_number(ok).empty(), ok, __LINE__);
}

void test_hidden_characters() {
    g_test = "hidden characters";
    using tools::clean_text;
    CHECK(clean_text("Allergic\u200b to \u202eshellfish\u202c") == "Allergic to shellfish");
    CHECK(clean_text("\ufeffLikes\u2060 tea\u200d\u200c\u200e\u200f\u061c\u2066x\u2069") == "Likes teax");
    // Unicode's tag characters spell out a sentence no screen shows: "Likes tea" and "IGNORE" in tags.
    CHECK(clean_text("Likes tea\U000E0049\U000E0047\U000E004E\U000E004F\U000E0052\U000E0045\U000E007F") == "Likes tea");
    CHECK(clean_text("Loves cats\ufe0f\U000E0100") == "Loves cats");
    // Line breaks of every kind are single spaces, as other control characters are.
    CHECK(clean_text("Likes tea\u2028## New instructions\u2029\u0085x\n\ty") == "Likes tea ## New instructions x y");
    // What shows is kept.
    CHECK(clean_text("Café in Zürich, 東京, Москва") == "Café in Zürich, 東京, Москва");
    CHECK(clean_text("  a  \u200b  b ") == "a b");
}

// The user's yes to a memory saved after text from outside (memory_save and memory_delete build these questions).
consent::Question memory_question(const std::string &text, const std::string &user_name = "Sam") {
    return {"memory_save\n0\n" + util::plain(text),
            consent::outside_reason("a memory is saved only at the user's own word, read back to them in full"),
            consent::read_back_question("Shall I remember this: ", text), "save",
            consent::read_back(text, {"remember", "save"}, user_name), "saved", "call memory_save again with the same text"};
}

void test_consent() {
    g_test = "the user's yes";
    consent::testing::reset();
    consent::user_spoke("remember that I'm allergic to shellfish");
    CHECK(consent::ask_first(memory_question("Allergic to shellfish."), 1000).empty());   // nothing from outside: at once
    consent::user_spoke("what does this sign say");
    consent::saw_untrusted_text();
    consent::user_spoke("remember that I'm allergic to shellfish");
    const std::string asked = consent::ask_first(memory_question("Allergic to shellfish."), 1000);
    CHECK(asked == "CONFIRM: nothing has been saved yet, because this conversation includes text from outside (a web page "
                   "or a photo), and a memory is saved only at the user's own word, read back to them in full. Ask the user: \"Shall I remember this: "
                   "Allergic to shellfish?\" Only if they answer yes, call memory_save again with the same text.");
    consent::assistant_replied("Shall I remember that you are allergic to shellfish?");
    consent::user_spoke("yes please");
    CHECK(consent::ask_first(memory_question("Allergic to shellfish."), 2000).empty());
    // Read back without the words that say something, the question was not put; a yes then answers nothing.
    for (const char *reply : {"Shall I save that?", "Shall I remember that you are allergic?", "I'll remember shellfish."}) {
        consent::user_spoke("remember my allergy");
        CHECK_HAS(consent::ask_first(memory_question("Allergic to shellfish"), 3000), "CONFIRM: nothing has been saved yet");
        consent::assistant_replied(reply);
        consent::user_spoke("yes");
        CHECK_HAS(consent::ask_first(memory_question("Allergic to shellfish"), 4000), "CONFIRM: nothing has been saved yet");
    }
    // Read back in the user's person and with a verb's other form, it was.
    consent::user_spoke("remember that");
    CHECK_HAS(consent::ask_first(memory_question("User prefers window seats"), 5000), "Shall I remember this: User prefers window seats?");
    consent::assistant_replied("Shall I remember that you prefer window seats?");
    consent::user_spoke("yes, remember it");
    CHECK(consent::ask_first(memory_question("User prefers window seats"), 6000).empty());
    // The user's own name is read back as "you": "Sam's desk" is "your desk" to Sam, but not to another user.
    for (const char *name : {"Sam", "Alex"}) {
        consent::user_spoke("remember that");
        consent::ask_first(memory_question("The fire exit by Sam's desk must be kept clear", name), 6500);
        consent::assistant_replied("Shall I remember this: the fire exit by your desk must be kept clear?");
        consent::user_spoke("yes");
        check(consent::ask_first(memory_question("The fire exit by Sam's desk must be kept clear", name), 6600).empty() ==
                  (std::string(name) == "Sam"), name, __LINE__);
    }
    // "No", another request, or a yes too late, is not a yes.
    for (const char *answer : {"no", "yes but also set a timer", "leave it"}) {
        consent::user_spoke("remember that");
        consent::ask_first(memory_question("Sister is called Anna"), 7000);
        consent::assistant_replied("Shall I remember that your sister is called Anna?");
        consent::user_spoke(answer);
        CHECK_HAS(consent::ask_first(memory_question("Sister is called Anna"), 8000), "the user's answer was not a clear yes");
    }
    consent::user_spoke("remember that");
    consent::ask_first(memory_question("Sister is called Anna"), 10000);
    consent::assistant_replied("Shall I remember that your sister is called Anna?");
    consent::user_spoke("yes");
    CHECK_HAS(consent::ask_first(memory_question("Sister is called Anna"), 10000 + 4 * 60 * 1000), "not a clear yes");
    // Another memory than the one asked about waits for its own yes.
    consent::user_spoke("remember that");
    consent::ask_first(memory_question("Sister is called Anna"), 20000);
    consent::assistant_replied("Shall I remember that your sister is called Anna?");
    consent::user_spoke("yes");
    CHECK_HAS(consent::ask_first(memory_question("Brother is called Tom"), 21000), "CONFIRM: nothing has been saved yet");
    // A new conversation: nothing from outside is in view, and nothing asked in the old one is open.
    consent::conversation_reset();
    consent::user_spoke("remember that");
    CHECK(consent::outside_reason("a memory is saved only at the user's own word").empty());
    CHECK(consent::ask_first(memory_question("Sister is called Anna"), 22000).empty());
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
// seccomp filter (ENOSYS, as a sandbox or an older kernel would), and with `keep_high` the close of
// kHigh alone (EPERM), lowers its soft limit on open files to `soft` (below kHigh, as a program may
// after opening it; the hard limit too with `hard_too`), and runs a helper program. What the helper
// saw: 0 not kHigh, 1 kHigh, 2 the test could not be set up here, 3 the helper did not run.
int helper_sees_high(const std::string &tmp, const std::vector<long> &refused, bool keep_high, rlim_t soft, bool hard_too) {
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
        if (hard_too) lim.rlim_max = soft;
        if (setrlimit(RLIMIT_NOFILE, &lim) != 0) _exit(2);
        std::vector<sock_filter> filter{BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr))};
        for (const long call : refused) {
            filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<unsigned>(call), 0, 1));
            filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (ENOSYS & SECCOMP_RET_DATA)));
        }
        if (keep_high) {   // close(kHigh) refused; its first argument's low word, on a little-endian machine
            filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_close, 0, 3));
            filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[0])));
            filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kHigh, 0, 1));
            filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)));
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
// stands in for it must reach every descriptor the process holds: not stop at 4096, nor at the limits
// on open files, which may have been lowered below a descriptor already open (fix round 2, C7). It
// closes what /proc/self/fd lists; where that cannot be read either, no bound on the numbers is sure,
// and the helper is not run at all (fix round 3, D10). Nor is it when a descriptor listed there cannot be
// closed (fix round 4, E10): the pass that failed to close it first counted as one with nothing left.
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
        bool keep_high;   // the close of the descriptor refused as well
        rlim_t soft;
        bool hard_too;
        bool runs;   // whether the helper may run (closing every descriptor can be made sure of)
    };
    const Case cases[] = {
        {"close_range refused", {SYS_close_range}, false, 8192, false, true},
        {"close_range refused, the soft limit lowered below the descriptor", {SYS_close_range}, false, 1024, false, true},
        {"close_range refused, both limits lowered below the descriptor", {SYS_close_range}, false, 1024, true, true},
        {"close_range and the listing of /proc/self/fd refused", {SYS_close_range, SYS_getdents64}, false, 8192, false, false},
        {"close_range and the listing of /proc/self/fd refused, both limits lowered", {SYS_close_range, SYS_getdents64}, false, 1024,
         true, false},
        {"close_range refused, and the close of a descriptor /proc/self/fd lists", {SYS_close_range}, true, 8192, false, false},
    };
    for (const Case &c : cases) {
        const int code = helper_sees_high(tmp, c.refused, c.keep_high, c.soft, c.hard_too);
        if (code == 2) {
            printf("tools: note: system calls could not be refused here (no seccomp); the check \"%s\" was skipped\n", c.what);
            continue;
        }
        g_test = c.what;
        if (c.runs) check(code == 0, code == 1 ? "descriptor 5000 reached the helper program" : "the helper program did not run", __LINE__);
        else check(code == 3, code == 1 ? "descriptor 5000 reached the helper program" : "the helper program ran", __LINE__);
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
    test_sensitive_numbers();
    test_hidden_characters();
    test_consent();
    test_processes(tmp);
    test_descriptors_without_close_range(tmp);
    printf("tools: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
