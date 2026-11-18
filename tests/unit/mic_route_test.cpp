// The built-in microphone with a plug in the 3.5 mm jack (audio::detail::point_at_builtin_mic and
// put_back_jack_mic), against a stand-in amixer that keeps the codec's controls in a file.
// Usage: mic_route_test (makes its own temporary folder and puts it first on PATH).
#include "platform/audio.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
int passed = 0, failed = 0;
void check(bool ok, const std::string &what) {
    if (ok) ++passed;
    else { ++failed; printf("FAIL %s\n", what.c_str()); }
}

std::string dir;

void write_state(const std::map<std::string, std::string> &s) {
    std::ofstream f(dir + "/state");
    for (const auto &kv : s) f << kv.first << "=" << kv.second << "\n";
}
std::map<std::string, std::string> read_state() {
    std::map<std::string, std::string> s;
    std::ifstream f(dir + "/state");
    for (std::string line; std::getline(f, line);) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos) s[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return s;
}
int sets() {   // how many cset calls the stand-in has seen
    std::ifstream f(dir + "/sets");
    int n = 0;
    for (std::string line; std::getline(f, line);) ++n;
    return n;
}

// amixer -c CARD cget name=NAME   |   amixer -c CARD -q cset name=NAME VALUE
const char kAmixer[] = R"SH(#!/bin/sh
state="$(dirname "$0")/state"
[ "$2" = "ES8389Audio" ] || { echo "no such card: $2" >&2; exit 1; }
if [ "$3" = "cget" ]; then
  # The jack's sensor is a card control: like the real amixer, a plain name= finds mixer controls only.
  case "$4" in
    name=Headphones\ Jack) echo "amixer: Cannot find the given element from control sysdefault:1" >&2; exit 1;;
    iface=CARD,name=*) name="${4#iface=CARD,name=}";;
    *) name="${4#name=}";;
  esac
  v=$(grep -F "$name=" "$state" | head -n1 | cut -d= -f2)
  [ -n "$v" ] || exit 1
  printf "numid=1,iface=MIXER,name='%s'\n  ; type=ENUMERATED\n  : values=%s\n" "$name" "$v"
  exit 0
fi
if [ "$4" = "cset" ]; then
  name="${5#name=}"
  grep -v -F "$name=" "$state" > "$state.new"; echo "$name=$6" >> "$state.new"; mv "$state.new" "$state"
  echo "$name=$6" >> "$(dirname "$0")/sets"
  exit 0
fi
exit 1
)SH";
}  // namespace

int main() {
    char tmpl[] = "/tmp/mic_route_test.XXXXXX";
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 1; }
    dir = tmpl;
    {
        std::ofstream f(dir + "/amixer");
        f << kAmixer;
    }
    chmod((dir + "/amixer").c_str(), 0755);
    const std::string original_path = getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin";
    setenv("PATH", (dir + ":" + original_path).c_str(), 1);
    using audio::detail::point_at_builtin_mic;
    using audio::detail::put_back_jack_mic;

    // The owner's 0.4.2 case, as the device reports it: a speaker in, the input on the jack's pins.
    // The recording must use the built-in microphone (0.4.2 read the jack sensor as a mixer control,
    // got nothing, took it for "no plug", and recorded silence).
    write_state({{"Headphones Jack", "on"}, {"PGAL Select", "2"}, {"PGAR Select", "1"}});
    {
        const int g0 = point_at_builtin_mic();
        auto s0 = read_state();
        check(s0["PGAL Select"] == "1" && s0["PGAR Select"] == "2", "a speaker in: the built-in microphone for the recording");
        put_back_jack_mic(g0);
        s0 = read_state();
        check(s0["PGAL Select"] == "2" && s0["PGAR Select"] == "1", "a speaker in: the jack's input put back after");
    }
    std::remove((dir + "/sets").c_str());

    // Nothing in the jack: the driver has the built-in microphone already; nothing is changed.
    write_state({{"Headphones Jack", "off"}, {"PGAL Select", "1"}, {"PGAR Select", "2"}});
    int g = point_at_builtin_mic();
    put_back_jack_mic(g);
    check(sets() == 0, "no plug: nothing is set");

    // A speaker in the jack (the owner's case): the built-in microphone for the recording, the jack's after.
    write_state({{"Headphones Jack", "on"}, {"PGAL Select", "2"}, {"PGAR Select", "1"}});
    g = point_at_builtin_mic();
    auto s = read_state();
    check(s["PGAL Select"] == "1" && s["PGAR Select"] == "2", "plug in: Line 1P/1N while recording");
    put_back_jack_mic(g);
    s = read_state();
    check(s["PGAL Select"] == "2" && s["PGAR Select"] == "1", "plug in: the jack's input put back afterwards");
    put_back_jack_mic(g);
    check(sets() == 4, "put back once only");

    // Unplugged during the recording: the driver chose the built-in microphone; nothing is put back.
    write_state({{"Headphones Jack", "on"}, {"PGAL Select", "2"}, {"PGAR Select", "1"}});
    g = point_at_builtin_mic();
    write_state({{"Headphones Jack", "off"}, {"PGAL Select", "1"}, {"PGAR Select", "2"}});
    put_back_jack_mic(g);
    s = read_state();
    check(s["PGAL Select"] == "1" && s["PGAR Select"] == "2", "unplugged meanwhile: the built-in microphone stays");

    // A new recording starts before the last one's input is put back (the put-back runs on a thread of
    // its own): the old put-back does nothing, the new one restores the jack's input.
    write_state({{"Headphones Jack", "on"}, {"PGAL Select", "2"}, {"PGAR Select", "1"}});
    const int first = point_at_builtin_mic();
    const int second = point_at_builtin_mic();
    put_back_jack_mic(first);
    s = read_state();
    check(s["PGAL Select"] == "1" && s["PGAR Select"] == "2", "an older recording's put-back leaves the newer one alone");
    put_back_jack_mic(second);
    s = read_state();
    check(s["PGAL Select"] == "2" && s["PGAR Select"] == "1", "the newer recording puts the jack's input back");

    // No amixer at all (or another sound card): nothing happens, nothing hangs.
    setenv("PATH", "/nonexistent", 1);
    g = point_at_builtin_mic();
    put_back_jack_mic(g);
    check(true, "no amixer: no crash");
    setenv("PATH", original_path.c_str(), 1);

    std::string cmd = "rm -r '" + dir + "'";
    if (dir.rfind("/tmp/mic_route_test.", 0) == 0) (void)system(cmd.c_str());
    printf("mic route: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
