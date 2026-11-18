// Small subprocess helpers (audio, camera, sandboxed Python and system tools run as children).
#pragma once
#include <atomic>
#include <string>
#include <sys/types.h>
#include <vector>

namespace proc {

struct Child {
    pid_t pid = -1;
    int in = -1;   // write end of the child's stdin, or -1
    int out = -1;  // read end of the child's stdout, or -1
    bool running() const { return pid > 0; }
};

// Starts argv[0] (looked up on PATH) in its own process group. stderr is discarded
// unless merge_err is set, in which case it joins stdout. The child inherits no other open file:
// not the screen, not a socket the app keeps open for its next request.
Child spawn(const std::vector<std::string> &argv, bool pipe_in, bool pipe_out, bool merge_err = false);

void close_in(Child &c);
bool alive(Child &c);                      // reaps the child if it has exited
int wait(Child &c);                        // blocks; returns exit code (or 128+signal)
void stop(Child &c, int sig = 15);         // signal the whole group, then reap
void signal_group(const Child &c, int sig);

struct Result {
    int code = -1;
    std::string out;
    bool timed_out = false;
    bool cancelled = false;   // `cancel` was set, and the command was stopped
    bool truncated = false;   // it printed more than max_out; the rest was dropped
    bool ok() const { return code == 0 && !timed_out && !cancelled; }
};

// Runs a command to completion with optional stdin text, capturing stdout (+stderr if asked).
// The whole run, waiting for the command to end included, is bounded by timeout_ms; when
// `cancel` is given and becomes true, the command's process group is killed within about 50 ms.
Result run(const std::vector<std::string> &argv, const std::string &input = "", int timeout_ms = 10000,
           size_t max_out = 64 * 1024, bool merge_err = false, const std::atomic<bool> *cancel = nullptr);

bool have(const std::string &program);     // is it on PATH?

}  // namespace proc
