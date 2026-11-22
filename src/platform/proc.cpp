#include "platform/proc.h"

#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace proc {

// A descriptor's number from its name in /proc/self/fd; -1 for anything else ("." and "..").
static int fd_number(const char *name) {
    int n = 0;
    if (!*name) return -1;
    for (; *name; ++name) {
        if (*name < '0' || *name > '9' || n > (INT_MAX - 9) / 10) return -1;
        n = n * 10 + (*name - '0');
    }
    return n;
}

#ifdef SYS_getdents64
// Whether `fd` is closed now. Linux lets a descriptor go even when close() reports an error (EINTR,
// EIO), but not when the call was refused before it ran (a sandbox's filter): that one stays open.
static bool gone(int fd) { return fcntl(fd, F_GETFD) == -1 && errno == EBADF; }

// One pass over /proc/self/fd (open as `dir`), read with getdents64 into a buffer on the stack:
// closes each descriptor it lists from `lowest` up, but `dir`. How many it closed; -1 when the
// listing could not be read, or a descriptor it lists could not be closed.
static int close_pass(int dir, int lowest) {
    alignas(dirent64) char buf[4096];
    if (lseek(dir, 0, SEEK_SET) != 0) return -1;
    int closed = 0;
    long n;
    while ((n = syscall(SYS_getdents64, dir, buf, sizeof buf)) > 0)
        for (long at = 0; at < n;) {
            const auto *entry = reinterpret_cast<const dirent64 *>(buf + at);
            at += entry->d_reclen;
            const int fd = fd_number(entry->d_name);
            if (fd < lowest || fd == dir) continue;
            if (close(fd) != 0 && !gone(fd)) return -1;
            ++closed;
        }
    return n < 0 ? -1 : closed;
}
#endif

// In the child: closes every descriptor from `lowest` up that /proc/self/fd lists (system calls
// only). Closing changes the listing, so it is read again until a pass closes nothing. False when
// it cannot be read (no /proc, or the call refused), or a descriptor it lists cannot be closed.
static bool close_listed(int lowest) {
#ifdef SYS_getdents64
    const int dir = open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) return false;
    int closed;
    while ((closed = close_pass(dir, lowest)) > 0) {}
    close(dir);
    return closed == 0;
#else
    (void)lowest;
    return false;
#endif
}

// In the child, between fork and exec: closes every descriptor from `lowest` up. Only system calls
// here (the parent has other threads, so nothing that might take a lock). False when that cannot be
// made sure of: close_range missing or refused, and /proc/self/fd not to be listed or a descriptor it
// lists not to be closed. Closing every number up to a bound is no stand-in: a process can hold a
// descriptor above any bound it can learn (its limits lowered after it was opened, or raised past any
// fixed ceiling).
static bool close_from(int lowest) {
#ifdef SYS_close_range
    if (syscall(SYS_close_range, static_cast<unsigned>(lowest), ~0U, 0U) == 0) return true;
#endif
    return close_listed(lowest);
}

Child spawn(const std::vector<std::string> &argv, bool pipe_in, bool pipe_out, bool merge_err, bool die_with_parent) {
    Child c;
    const pid_t parent = getpid();
    if (argv.empty()) return c;
    int in_pipe[2] = {-1, -1}, out_pipe[2] = {-1, -1};
    if (pipe_in && pipe2(in_pipe, O_CLOEXEC) != 0) return c;
    if (pipe_out && pipe2(out_pipe, O_CLOEXEC) != 0) {
        if (pipe_in) { close(in_pipe[0]); close(in_pipe[1]); }
        return c;
    }
    std::vector<char *> args;
    for (auto &a : argv) args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1]})
            if (fd >= 0) close(fd);
        return c;
    }
    if (pid == 0) {
        if (die_with_parent) {
            // Kept across exec (env, then the player). If the parent has already gone, so do we.
            prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (getppid() != parent) _exit(127);
        }
        setpgid(0, 0);
        int devnull = open("/dev/null", O_RDWR);
        dup2(pipe_in ? in_pipe[0] : devnull, STDIN_FILENO);
        dup2(pipe_out ? out_pipe[1] : devnull, STDOUT_FILENO);
        dup2(merge_err && pipe_out ? out_pipe[1] : devnull, STDERR_FILENO);
        // Many descriptors are opened without close-on-exec (the graphics library's framebuffer,
        // files read with streams, possibly sockets kept alive for the next request), and a
        // sandbox does not close what it inherits: the sandboxed Python could otherwise write to
        // the screen or to a device's open connection. The child gets its three standard streams, or
        // the program is not run at all (as if it could not be started).
        if (!close_from(3)) _exit(127);
        signal(SIGPIPE, SIG_DFL);
        execvp(args[0], args.data());
        _exit(127);
    }
    setpgid(pid, pid);
    c.pid = pid;
    if (pipe_in) { close(in_pipe[0]); c.in = in_pipe[1]; }
    if (pipe_out) { close(out_pipe[1]); c.out = out_pipe[0]; }
    return c;
}

void close_in(Child &c) {
    if (c.in >= 0) { close(c.in); c.in = -1; }
}

static int decode(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

static void close_fds(Child &c) {
    if (c.in >= 0) { close(c.in); c.in = -1; }
    if (c.out >= 0) { close(c.out); c.out = -1; }
}

bool alive(Child &c) {
    if (c.pid <= 0) return false;
    int status = 0;
    pid_t r = waitpid(c.pid, &status, WNOHANG);
    if (r == 0) return true;
    c.pid = -1;
    close_fds(c);
    return false;
}

int wait(Child &c) {
    if (c.pid <= 0) return -1;
    int status = 0;
    while (waitpid(c.pid, &status, 0) < 0 && errno == EINTR) {}
    c.pid = -1;
    close_fds(c);
    return decode(status);
}

void signal_group(const Child &c, int sig) {
    if (c.pid > 0) kill(-c.pid, sig);
}

void stop(Child &c, int sig) {
    if (c.pid <= 0) { close_fds(c); return; }
    kill(-c.pid, sig);
    int status = 0;
    for (int i = 0; i < 40; ++i) {  // up to 400 ms of grace, then force
        if (waitpid(c.pid, &status, WNOHANG) != 0) { c.pid = -1; close_fds(c); return; }
        usleep(10 * 1000);
    }
    kill(-c.pid, SIGKILL);
    while (waitpid(c.pid, &status, 0) < 0 && errno == EINTR) {}
    c.pid = -1;
    close_fds(c);
}

Result run(const std::vector<std::string> &argv, const std::string &input, int timeout_ms, size_t max_out,
           bool merge_err, const std::atomic<bool> *cancel) {
    Result res;
    if (cancel && *cancel) { res.cancelled = true; return res; }
    Child c = spawn(argv, !input.empty(), true, merge_err);
    if (!c.running()) return res;
    if (c.in >= 0) {
        // Inputs here are small (scripts, expressions); a blocking write is fine.
        size_t off = 0;
        while (off < input.size()) {
            ssize_t n = write(c.in, input.data() + off, input.size() - off);
            if (n <= 0) break;
            off += static_cast<size_t>(n);
        }
        close_in(c);
    }
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
    auto left_ms = [&] { return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count(); };
    // With a cancel flag, wait in short slices so that it is seen promptly.
    auto slice = [&](long long left) { return static_cast<int>(cancel ? std::min<long long>(left, 50) : left); };
    char buf[4096];
    for (;;) {
        if (cancel && *cancel) { res.cancelled = true; break; }
        const long long left = left_ms();
        if (left <= 0) { res.timed_out = true; break; }
        pollfd pfd{c.out, POLLIN, 0};
        int r = poll(&pfd, 1, slice(left));
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) continue;
        ssize_t n = read(c.out, buf, sizeof(buf));
        if (n <= 0) break;
        const size_t room = max_out - std::min(max_out, res.out.size());
        if (static_cast<size_t>(n) > room) res.truncated = true;
        res.out.append(buf, std::min(static_cast<size_t>(n), room));
    }
    // The output has ended, but a program that closed it can still be running: the wait for it
    // to end has the same deadline and the same cancel flag.
    while (!res.timed_out && !res.cancelled && c.running()) {
        int status = 0;
        const pid_t r = waitpid(c.pid, &status, WNOHANG);
        if (r == c.pid) { c.pid = -1; close_fds(c); res.code = decode(status); return res; }
        if (r < 0 && errno != EINTR) { c.pid = -1; close_fds(c); return res; }
        if (cancel && *cancel) { res.cancelled = true; break; }
        if (left_ms() <= 0) { res.timed_out = true; break; }
        usleep(10 * 1000);
    }
    stop(c, SIGKILL);
    res.code = -1;
    return res;
}

bool have(const std::string &program) {
    const char *path = getenv("PATH");
    std::string paths = path ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t start = 0;
    while (start <= paths.size()) {
        size_t end = paths.find(':', start);
        if (end == std::string::npos) end = paths.size();
        std::string full = paths.substr(start, end - start) + "/" + program;
        if (access(full.c_str(), X_OK) == 0) return true;
        start = end + 1;
    }
    return false;
}

}  // namespace proc
