// The music player (audio::Music). First its queue under the holds, against a stand-in ffplay (the player
// of anything but an MP3), which needs nothing installed. Then its MP3 path (codec::Mp3File and pacat),
// against a stand-in pacat that keeps what it is given: an MP3 is made with ffmpeg; without ffmpeg or
// libmpg123 that part says so and passes (the app then plays MP3s with ffplay, as before).
#include "platform/audio.h"
#include "platform/codec.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
int passed = 0, failed = 0;
void check(bool ok, const std::string &what) {
    if (ok) ++passed;
    else { ++failed; printf("FAIL %s\n", what.c_str()); }
}
std::string slurp(const std::string &p) { std::ifstream f(p); std::stringstream s; s << f.rdbuf(); return s.str(); }
long size_of(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0 ? static_cast<long>(st.st_size) : -1; }
bool wait_until(const std::function<bool()> &done, int ms) {
    for (int i = 0; i < ms / 20; ++i) { if (done()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    return done();
}
void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(400)); }   // longer than the player's own 100 ms checks

// A process's state from /proc: 'T' while suspended (SIGSTOP), 0 once it has gone.
char state_of(pid_t pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    std::getline(f, line);
    const size_t close = line.rfind(')');
    const char state = close == std::string::npos || close + 2 >= line.size() ? 0 : line[close + 2];
    return state == 'Z' ? 0 : state;
}
bool suspended(pid_t pid) { return state_of(pid) == 'T'; }
bool playing(pid_t pid) { return state_of(pid) != 0 && !suspended(pid); }

// Each start of the stand-in ffplay notes its process id and track, then waits as a long track would. The
// search path holds its folder alone, so no real player can start.
struct StandInPlayer {
    std::string dir, log;
    bool make(const std::string &folder) {
        dir = folder;
        log = dir + "/started.log";
        std::ofstream(dir + "/ffplay") << "#!/bin/sh\nPATH=/usr/bin:/bin\necho \"$$ $*\" >> '" << log << "'\nexec sleep 600\n";
        chmod((dir + "/ffplay").c_str(), 0755);
        const bool env = symlink("/usr/bin/env", (dir + "/env").c_str()) == 0 || symlink("/bin/env", (dir + "/env").c_str()) == 0;
        setenv("PATH", dir.c_str(), 1);
        return env;
    }
    // (process id, track) of each start, in order.
    std::vector<std::pair<pid_t, std::string>> starts() const {
        std::vector<std::pair<pid_t, std::string>> out;
        std::istringstream lines(slurp(log));
        for (std::string line; std::getline(lines, line);)
            if (!line.empty()) out.push_back({static_cast<pid_t>(atol(line.c_str())), line.substr(line.rfind(' ') + 1)});
        return out;
    }
};

// The holds (AGENTS.md section 8, Audio): a queue asked for during a request waits until the reply has been
// spoken; the microphone, a question and its answer, and the user's pause suspend the player; "next" while
// paused waits for the pause to end; "previous" goes back a track; stop ends a suspended player.
void test_queue_holds() {
    using audio::Music;
    char tmpl[] = "/tmp/music_holds.XXXXXX";
    if (!mkdtemp(tmpl)) { check(false, "a folder for the stand-in player"); return; }
    const std::string dir = tmpl, path0 = getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin";
    StandInPlayer p;
    check(p.make(dir), "the stand-in player is in place");
    Music &m = Music::get();
    m.hold(Music::kExchange, true);                   // the music tool, called during a request
    m.play({dir + "/a.ogg", dir + "/b.ogg", dir + "/c.ogg"});
    settle();
    check(p.starts().empty(), "a queue asked for during a request waits");
    m.hold(Music::kSpeech, true);                     // the reply is spoken
    settle();
    check(p.starts().empty(), "... and while the reply is spoken");
    m.hold(Music::kSpeech | Music::kExchange, false);
    check(wait_until([&] { return p.starts().size() == 1; }, 3000) && p.starts()[0].second == dir + "/a.ogg",
          "once the reply has been spoken, the first track starts");
    const pid_t first = p.starts().empty() ? 0 : p.starts()[0].first;
    m.hold(Music::kMic, true);                        // TAB: the microphone opens
    check(wait_until([&] { return suspended(first); }, 2000), "the microphone open: the track is suspended");
    m.hold(Music::kExchange, true);                   // the question is sent
    m.hold(Music::kMic, false);
    settle();
    check(suspended(first), "the question sent: still suspended, until its answer");
    m.hold(Music::kSpeech, true);
    m.hold(Music::kSpeech | Music::kExchange, false);   // the answer has been spoken
    check(wait_until([&] { return playing(first); }, 2000), "the answer spoken: the track goes on");
    m.pause(true);                                    // fn+Q
    check(wait_until([&] { return suspended(first); }, 2000), "paused by the user: suspended");
    m.next();
    check(wait_until([&] { return state_of(first) == 0; }, 3000), "next while paused: the track ends");
    settle();
    check(p.starts().size() == 1, "... and the next waits for the pause to end");
    m.pause(false);
    check(wait_until([&] { return p.starts().size() == 2; }, 3000) && p.starts()[1].second == dir + "/b.ogg",
          "unpaused: the next track plays");
    m.previous();
    check(wait_until([&] { return p.starts().size() == 3; }, 3000) && p.starts()[2].second == dir + "/a.ogg",
          "previous: back to the track before");
    const pid_t last = p.starts().size() == 3 ? p.starts()[2].first : 0;
    m.hold(Music::kMic, true);
    check(wait_until([&] { return suspended(last); }, 2000), "the microphone open again: suspended");
    m.stop();
    check(state_of(last) == 0 && !m.playing(), "stopped while suspended: the player has gone");
    m.hold(Music::kMic, false);
    setenv("PATH", path0.c_str(), 1);
    if (dir.rfind("/tmp/music_holds.", 0) == 0) (void)system(("rm -r '" + dir + "'").c_str());
}
}  // namespace

int main() {
    test_queue_holds();
    char tmpl[] = "/tmp/music_test.XXXXXX";
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 1; }
    const std::string dir = tmpl;
    const std::string mp3 = dir + "/tone.mp3";
    const std::string make = "ffmpeg -loglevel error -y -f lavfi -i sine=f=440:d=3 -ac 2 -ar 44100 -c:a libmp3lame -b:a 192k '" + mp3 + "'";
    const char *skip = system(make.c_str()) != 0 || size_of(mp3) <= 0 ? "no ffmpeg to make a test MP3" : nullptr;
    codec::Mp3File probe;
    if (!skip && !probe.open(mp3)) skip = "no libmpg123 here";
    if (skip) {
        printf("music: %d passed, %d failed; the MP3 path skipped: %s\n", passed, failed, skip);
        return failed ? 1 : 0;
    }
    check(probe.rate() == 44100 && probe.channels() == 2, "the decoder reads the file's rate and channels");
    long total = 0;
    unsigned char buf[8192];
    for (long n; (n = probe.read(buf, sizeof buf)) > 0;) total += n;
    // 3 s at 44100 Hz, 2 channels, 2 bytes: 529200 bytes, give or take the encoder's padding frames.
    check(total > 500000 && total < 560000, "about 3 s of samples decoded (" + std::to_string(total) + " bytes)");
    codec::Mp3File bad;
    std::ofstream(dir + "/not.mp3") << std::string(4096, 'x');
    check(!bad.open(dir + "/not.mp3"), "a file with no MP3 frame is refused (ffplay then gets it)");

    // The stand-in pacat: its arguments and everything it is given are kept.
    std::ofstream(dir + "/pacat") << "#!/bin/sh\necho \"$@\" > \"$(dirname \"$0\")/args\"\ncat > \"$(dirname \"$0\")/played\"\n";
    chmod((dir + "/pacat").c_str(), 0755);
    const std::string path0 = getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin";
    setenv("PATH", (dir + ":" + path0).c_str(), 1);

    audio::Music &music = audio::Music::get();
    music.play({mp3});
    check(wait_until([&] { return !music.playing(); }, 10000), "the track ends by itself once it is all played");
    const std::string args = slurp(dir + "/args");
    check(args.find("--rate=44100") != std::string::npos && args.find("--channels=2") != std::string::npos &&
              args.find("--format=s16le") != std::string::npos && args.find("--latency-msec=300") != std::string::npos,
          "pacat is told the format and a 300 ms buffer: " + args);
    check(size_of(dir + "/played") == total, "pacat got every decoded byte (" + std::to_string(size_of(dir + "/played")) + ")");

    // A player that stops taking data (held, or hung): stop() still ends it at once, nothing hangs.
    std::ofstream(dir + "/pacat") << "#!/bin/sh\nsleep 30\n";
    music.play({mp3});
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto t0 = std::chrono::steady_clock::now();
    music.stop();
    const long ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    check(!music.playing() && ms < 3000, "stop() with a player that takes nothing returns in " + std::to_string(ms) + " ms");

    // Held (SIGSTOP) and let go: the whole track still arrives.
    std::ofstream(dir + "/pacat") << "#!/bin/sh\ncat > \"$(dirname \"$0\")/played\"\n";
    music.play({mp3});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    music.hold(audio::Music::kSpeech, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    music.hold(audio::Music::kSpeech, false);
    check(wait_until([&] { return !music.playing(); }, 10000) && size_of(dir + "/played") == total,
          "a track held and let go arrives whole");

    music.stop();   // as the app does on the way out: joins the player's thread
    setenv("PATH", path0.c_str(), 1);
    const std::string clean = "rm -r '" + dir + "'";
    if (dir.rfind("/tmp/music_test.", 0) == 0) (void)system(clean.c_str());
    printf("music: %d passed, %d failed\n", passed, failed);
    fflush(stdout);
    return failed ? 1 : 0;
}
