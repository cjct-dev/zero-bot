// The music player's MP3 path (audio::Music with codec::Mp3File and pacat), against a stand-in pacat
// that keeps what it is given. An MP3 is made at the start with ffmpeg; without ffmpeg or libmpg123
// the test says so and passes (the app then plays MP3s with ffplay, as before).
#include "platform/audio.h"
#include "platform/codec.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

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
}  // namespace

int main() {
    char tmpl[] = "/tmp/music_test.XXXXXX";
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 1; }
    const std::string dir = tmpl;
    const std::string mp3 = dir + "/tone.mp3";
    const std::string make = "ffmpeg -loglevel error -y -f lavfi -i sine=f=440:d=3 -ac 2 -ar 44100 -c:a libmp3lame -b:a 192k '" + mp3 + "'";
    if (system(make.c_str()) != 0 || size_of(mp3) <= 0) { printf("music: no ffmpeg to make a test MP3; skipped\n"); return 0; }
    codec::Mp3File probe;
    if (!probe.open(mp3)) { printf("music: no libmpg123 here; skipped\n"); return 0; }
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
