#!/bin/sh
# Device-free unit tests: the smart-home layer (store, names, schema, safety rules, locking,
# pairing, the Tapo driver's refusals, colours), the HTTP options for devices, the timers (the
# timer tool, storage, the countdown, the alarm and its player) and the keys (what each does on the
# chat screen, and the key guide). Nothing but 127.0.0.1 is
# contacted, and no AI service, sound or screen is needed.
#
#   tests/run_unit.sh                   build into build/unit and run
#   ZB_UPDATE_SNAPSHOTS=1 tests/run_unit.sh    rewrite tests/snapshots/*.json from the current schema
#   ZB_BUILD_WRAPPER=cmd tests/run_unit.sh     run each compile through cmd (e.g. a lock on a shared machine)
#
# When the command-line build exists (build/cli/zero_bot), the timer tool is also run through it,
# zero_bot --tool timer '{...}', with a fresh, temporary state folder. CXX picks the compiler
# (default g++). Exits non-zero when anything fails.
set -eu
cd "$(dirname "$0")/.."
OUT=build/unit
mkdir -p "$OUT"
CXX="${CXX:-g++}"
FLAGS="-std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Isrc -Ithird_party \
 -DZB_VERSION=\"unit\" -DZB_DESKTOP=1 -DZB_CLI_ONLY=1"
WRAP="${ZB_BUILD_WRAPPER:-}"
state=$(mktemp -d)
trap 'rm -rf "$state"' EXIT INT TERM

# Every driver in src/smarthome/drivers/ is built: registry.cpp names each family's make_<family>_driver(),
# so a family registered there (AGENTS.md section 9) links here with nothing else to change.
DRIVERS=$(echo src/smarthome/drivers/*.cpp)
APP="src/smarthome/store.cpp src/smarthome/targets.cpp src/smarthome/control.cpp src/smarthome/tool.cpp
     src/smarthome/discovery.cpp src/smarthome/tapo.cpp $DRIVERS src/core/util.cpp src/platform/proc.cpp
     src/platform/sys.cpp src/net/http.cpp"
# The timers need only these, with util.cpp and proc.cpp from above.
TIMERS="src/core/timers.cpp src/core/store.cpp src/platform/audio.cpp"

obj_of() { echo "$OUT/$(echo "$1" | tr '/' '_' | sed 's/\.cpp$/.o/')"; }

# Objects are rebuilt only when their source or a header in src/ is newer.
newest_header=$(find src -name '*.h' -newer "$OUT/.stamp" 2>/dev/null | head -n 1 || true)
for src in $APP $TIMERS tests/unit/smarthome_test.cpp tests/unit/http_test.cpp tests/unit/timers_test.cpp \
           tests/unit/firetv_test.cpp tests/unit/firetv_cli.cpp tests/unit/homekit_test.cpp tests/unit/homekit_cli.cpp \
           tests/unit/hue_test.cpp tests/unit/hue_cli.cpp \
           src/ui/key_guide.cpp tests/unit/key_guide_test.cpp; do
    obj=$(obj_of "$src")
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ -n "$newest_header" ]; then
        echo "compiling $src"
        $WRAP $CXX $FLAGS -c "$src" -o "$obj"
    fi
done
touch "$OUT/.stamp"
objs=""
for src in $APP; do objs="$objs $(obj_of "$src")"; done
timer_objs="$(obj_of src/core/util.cpp) $(obj_of src/platform/proc.cpp)"
for src in $TIMERS; do timer_objs="$timer_objs $(obj_of "$src")"; done
$WRAP $CXX -o "$OUT/smarthome_test" "$OUT/tests_unit_smarthome_test.o" $objs -lcurl -lcrypto -lpthread
$WRAP $CXX -o "$OUT/http_test" "$OUT/tests_unit_http_test.o" $objs -lcurl -lcrypto -lpthread
$WRAP $CXX -o "$OUT/timers_test" "$OUT/tests_unit_timers_test.o" $timer_objs -lpthread
# The Fire TV driver's own tests; firetv_cli is what tests/firetv_e2e.py drives against its mock.
$WRAP $CXX -o "$OUT/firetv_test" "$OUT/tests_unit_firetv_test.o" $objs -lcurl -lcrypto -lpthread
$WRAP $CXX -o "$OUT/firetv_cli" "$OUT/tests_unit_firetv_cli.o" $objs -lcurl -lcrypto -lpthread
# The same for the HomeKit driver (tests/homekit_e2e.py drives homekit_cli).
$WRAP $CXX -o "$OUT/homekit_test" "$OUT/tests_unit_homekit_test.o" $objs -lcurl -lcrypto -lpthread
$WRAP $CXX -o "$OUT/homekit_cli" "$OUT/tests_unit_homekit_cli.o" $objs -lcurl -lcrypto -lpthread
# The same for the Hue driver (tests/hue_e2e.py drives hue_cli).
$WRAP $CXX -o "$OUT/hue_test" "$OUT/tests_unit_hue_test.o" $objs -lcurl -lcrypto -lpthread
$WRAP $CXX -o "$OUT/hue_cli" "$OUT/tests_unit_hue_cli.o" $objs -lcurl -lcrypto -lpthread
# What each key does on the chat screen, and the key guide that lists them (no screen needed).
$WRAP $CXX -o "$OUT/key_guide_test" "$OUT/tests_unit_key_guide_test.o" "$(obj_of src/ui/key_guide.cpp)"

failed=0
echo "=== smart home"
"$OUT/smarthome_test" tests/snapshots || failed=1
echo "=== http options"
python3 -I tests/unit/http_peer.py "$OUT/http_test" || failed=1
echo "=== Fire TV driver"
"$OUT/firetv_test" tests/mocks/firetv || failed=1
echo "=== HomeKit driver"
"$OUT/homekit_test" || failed=1
echo "=== Hue driver"
"$OUT/hue_test" tests/mocks/hue || failed=1
echo "=== keys"
"$OUT/key_guide_test" || failed=1

echo "=== timers"
# As someone would run the binary by hand: no XDG folders set, and a home that holds data of its
# own, which the tests must leave exactly as it was (they keep to a temporary folder of their own).
home="$state/home"
mkdir -p "$home/.local/share/zero-bot"
for f in calendar memory reminders; do
    printf '[{"id":1,"text":"%s kept","due":1,"created":1}]\n' "$f" > "$home/.local/share/zero-bot/$f.json"
    cp "$home/.local/share/zero-bot/$f.json" "$state/$f.expected"
done
env -u XDG_CONFIG_HOME -u XDG_DATA_HOME -u XDG_CACHE_HOME HOME="$home" TMPDIR="$state" TZ=UTC "$OUT/timers_test" || failed=1
for f in calendar memory reminders; do
    if ! cmp -s "$home/.local/share/zero-bot/$f.json" "$state/$f.expected"; then
        echo "FAIL the tests changed $f.json in the home folder they were run from"
        failed=1
    fi
done

cli=build/cli/zero_bot
if [ ! -x "$cli" ]; then
    echo "=== $cli not built: the timer tool checks through it were skipped"
    exit $failed
fi

echo "=== the timer tool through the command-line build"
tool_state="$state/cli"
mkdir -p "$tool_state"
passed=0
tool_failed=0
# expect ARGS TEXT: the tool's result for ARGS must contain TEXT.
expect() {
    result=$(HOME="$tool_state" XDG_CONFIG_HOME="$tool_state/config" XDG_DATA_HOME="$tool_state/data" \
             XDG_CACHE_HOME="$tool_state/cache" ZB_DRY_RUN=1 ZB_TEST_NETWORK=1 "$cli" --tool timer "$1") || true
    case "$result" in
        *"$2"*) passed=$((passed + 1)) ;;
        *) tool_failed=$((tool_failed + 1)); echo "FAIL --tool timer '$1'"; echo "     expected: $2"; echo "     got:      $result" ;;
    esac
}
expect '{"action":"start","minutes":5,"label":"food out of the oven"}' '"label":"Food out of the oven"'
expect '{"action":"start","minutes":60,"label":"Study"}' '"length":"1 hour"'
expect '{"action":"start","seconds":90}' '"label":"90 second timer"'
expect '{"action":"list"}' '"label":"Food out of the oven"'
expect '{"action":"cancel"}' 'ERROR: 3 timers are running'
expect '{"action":"cancel","label":"studying"}' '"cancelled":["Study"]'
expect '{"action":"add_time","minutes":10,"label":"oven"}' '"added":"10 minutes"'
expect '{"action":"add_time","minutes":10,"label":"oven"}' '"time_left":"2'
expect '{"action":"cancel","label":"pizza"}' "ERROR: no running timer is called 'pizza'"
expect '{"action":"start"}' 'ERROR: give the length'
expect '{"action":"start","minutes":2000}' 'ERROR: a timer runs for at most 24 hours'
expect '{"action":"cancel","all":true}' '"cancelled":["90 second timer","Food out of the oven"]'
expect '{"action":"list"}' '"note":"no timer is running"'
expect '{"action":"start","minutes":5}' '"label":"5 minute timer"'
expect '{"action":"start","minutes":25}' '"label":"25 minute timer"'
expect '{"action":"cancel","label":"twenty-five minute timer"}' '"cancelled":["25 minute timer"]'
echo "timer tool: $passed passed, $tool_failed failed"
[ "$tool_failed" -eq 0 ] || failed=1
exit $failed
