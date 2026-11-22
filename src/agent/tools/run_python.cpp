// The run_python tool: Python 3 in a sandbox (bwrap): no network, no user files, limits on time and memory.
#include "agent/tools/common.h"

#include <unistd.h>

#include "platform/proc.h"

namespace tools {

namespace {

std::string t_python(const json &a, Context &ctx) {
    const std::string code = jget(a, "code", std::string());
    if (util::trim(code).empty()) return fail("code is required: complete Python source as text");
    if (!proc::have("bwrap")) return fail("the Python sandbox is not available on this device");
    // No network, no access to user files, a throw-away /tmp, and limits on time and memory.
    std::vector<std::string> argv = {"bwrap", "--unshare-all", "--die-with-parent", "--new-session",
                                     "--ro-bind", "/usr", "/usr", "--symlink", "usr/bin", "/bin",
                                     "--symlink", "usr/lib", "/lib", "--proc", "/proc", "--dev", "/dev",
                                     "--tmpfs", "/tmp", "--tmpfs", "/home", "--setenv", "HOME", "/tmp",
                                     "--setenv", "PATH", "/usr/bin", "--chdir", "/tmp"};
    if (access("/usr/lib64", F_OK) == 0) { argv.insert(argv.end(), {"--symlink", "usr/lib64", "/lib64"}); }
    if (access("/etc/ld.so.cache", R_OK) == 0)
        argv.insert(argv.end(), {"--ro-bind", "/etc/ld.so.cache", "/etc/ld.so.cache"});
    // 200 MB of address space: several times what Python with its common modules needs (under
    // 30 MB measured on a PC), and
    // well short of the device's memory (about 350 MB, shared with everything else), so a runaway
    // list ends in a MemoryError rather than pushing the whole device into swap.
    argv.insert(argv.end(), {"sh", "-c", "ulimit -v 200000 2>/dev/null; exec python3 -I -"});
    const proc::Result res = proc::run(argv, code, 20000, 6000, true, ctx.cancel);
    std::string out = util::sanitize_utf8(util::trim(res.out));
    if (res.cancelled) return fail("stopped: the user cancelled the request");
    if (res.truncated) out += "\n(output cut short after 6000 bytes; print less, e.g. a summary or the first lines)";
    if (res.timed_out) return fail("the code ran for more than 20 seconds and was stopped." + (out.empty() ? "" : " Output so far:\n" + out));
    if (res.code != 0)
        return fail("the code stopped with an error (exit code " + std::to_string(res.code) +
                    "). Fix it and run it again, or tell the user it could not be worked out. Its output:\n" +
                    (out.empty() ? "(no output)" : out));
    return out.empty() ? "(the code ran but printed nothing; use print() to see a result)" : out;
}

}  // namespace

Tool make_run_python() {
    return {"run_python", "Running Python",
            "Run Python 3 in an isolated sandbox and return what it prints. Use it for multi-step calculations, "
            "text processing or anything calculator and date_info cannot do. The sandbox has the standard library "
            "only, no internet and no access to the user's files or the other tools; each call starts fresh. Always "
            "print() the result. Time limit 20 seconds.",
            obj({{"code", S("Complete Python source code")}}, {"code"}), t_python};
}

}  // namespace tools
