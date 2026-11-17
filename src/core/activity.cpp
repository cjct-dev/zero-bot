#include "core/activity.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <ctime>
#include <mutex>

#include "core/util.h"

namespace activity {
namespace {

constexpr off_t kMaxBytes = 256 * 1024;
std::mutex g_mtx;

// One line: control characters (a result's line breaks among them) become spaces.
std::string flat(const std::string &s, size_t max_bytes) {
    std::string out = util::truncate_utf8(s, max_bytes, " …");
    for (char &c : out)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
    return out;
}

void write_line(const std::string &line) {
    std::lock_guard<std::mutex> lk(g_mtx);
    const std::string file = path();
    util::make_dirs(util::cache_dir());
    struct stat st{};
    if (stat(file.c_str(), &st) == 0 && st.st_size > kMaxBytes) rename(file.c_str(), (file + ".1").c_str());
    const int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return;
    fchmod(fd, 0600);
    const std::string text = util::format_time(time(nullptr), "%Y-%m-%d %H:%M:%S ") + line + "\n";
    if (write(fd, text.data(), text.size()) < 0) { /* a log that cannot be written is not worth stopping for */ }
    close(fd);
}

}  // namespace

std::string path() { return util::cache_dir() + "/activity.log"; }

void request() { write_line("--- request"); }

void tool(const std::string &name, const std::string &args, const std::string &result, long ms) {
    write_line(flat(name, 40) + " " + flat(args, 400) + " -> " + flat(result, 600) + " (" + std::to_string(ms) + " ms)");
}

}  // namespace activity
