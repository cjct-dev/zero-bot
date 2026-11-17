// Small subprocess helpers (audio, camera, sandboxed Python and system tools run as children).
#pragma once
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
// unless merge_err is set, in which case it joins stdout.
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
    bool ok() const { return code == 0 && !timed_out; }
};

// Runs a command to completion with optional stdin text, capturing stdout (+stderr if asked).
Result run(const std::vector<std::string> &argv, const std::string &input = "", int timeout_ms = 10000,
           size_t max_out = 64 * 1024, bool merge_err = false);

bool have(const std::string &program);     // is it on PATH?

}  // namespace proc
