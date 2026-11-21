#!/bin/sh
# Device-free unit tests: the smart-home layer (store, names, schema, safety rules, locking,
# pairing, the Tapo driver's refusals, colours), the drivers' own tests (Fire TV, HomeKit, Hue,
# Kasa, WiZ, Shelly), the HTTP options for devices, the timers (the timer tool, storage, the
# countdown, the alarm and its player), the radio (the station file, the tool, the check of a
# stream, the player's live mode), the keys (what each does on the chat screen, and the key guide),
# when the screen dims and goes off, what the other tools stand on (calculator, dates, time
# zones, helper programs), what every thread shares (one file saved by two threads, the settings,
# all-day alerts when the clocks change) and the diagnostics (the activity log and saved
# conversations; temporary). Nothing but 127.0.0.1 is contacted, and no AI service, sound or screen is needed.
#
#   tests/run_unit.sh                   build into build/unit and run
#   ZB_UPDATE_SNAPSHOTS=1 tests/run_unit.sh    rewrite tests/snapshots/*.json from the current schema
#   ZB_BUILD_WRAPPER=cmd tests/run_unit.sh     run each compile through cmd (e.g. a lock on a shared machine)
#   ZB_UNIT_BUILD_ONLY=1 tests/run_unit.sh     build the test programs (the drivers' *_cli too) and stop
#   ZB_UNIT_SKIP_SCRIPTS=1 tests/run_unit.sh   leave out tests/tools_cli.py and tests/agent_loop_test.py
#                                              (tests/run.py runs them as groups of their own)
#
# When the command-line build exists (build/cli/zero_bot), the timer and radio tools are also run
# through it, zero_bot --tool timer '{...}', with a fresh, temporary state folder, the system
# prompt and tool list are checked to say nothing of radio until stations are listed, the other
# tools that need no network are run too (tests/tools_cli.py: good, wrong and hostile arguments),
# and the request loop is run against a scripted chat service (tests/agent_loop_test.py). CXX
# picks the compiler (default g++). Exits non-zero when anything fails.
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
TIMERS="src/core/timers.cpp src/core/store.cpp src/platform/audio.cpp src/platform/codec.cpp"
# The radio needs these on top of the smart-home objects (for its name cleaning) and audio.cpp.
RADIO="src/core/radio.cpp src/core/tether.cpp"
# The tools' foundations need only the calculator, with util.cpp and proc.cpp from above.
TOOLS="src/agent/calc.cpp"
# DIAGNOSTICS (temporary): the activity log and saved conversations, with util.cpp from above.
DIAG="src/core/activity.cpp src/core/settings.cpp"
# What an older version left behind (core_test), with util.cpp and proc.cpp from above.
UPGRADE="src/core/upgrade.cpp"
# What every thread shares: files saved at once, the settings, the calendar's alerts (store.cpp
# from TIMERS, util.cpp from above, settings.cpp from DIAG).

obj_of() { echo "$OUT/$(echo "$1" | tr '/' '_' | sed 's/\.cpp$/.o/')"; }

# Objects are rebuilt only when their source or a header in src/ is newer.
newest_header=$(find src -name '*.h' -newer "$OUT/.stamp" 2>/dev/null | head -n 1 || true)
for src in $APP $TIMERS $RADIO $TOOLS $DIAG $UPGRADE tests/unit/activity_test.cpp tests/unit/core_test.cpp tests/unit/smarthome_test.cpp tests/unit/http_test.cpp tests/unit/timers_test.cpp tests/unit/mic_route_test.cpp tests/unit/music_test.cpp \
           tests/unit/radio_test.cpp tests/unit/tools_test.cpp tests/unit/firetv_test.cpp tests/unit/firetv_cli.cpp \
           tests/unit/homekit_test.cpp tests/unit/homekit_cli.cpp tests/unit/hue_test.cpp tests/unit/hue_cli.cpp \
           tests/unit/kasa_test.cpp tests/unit/kasa_cli.cpp tests/unit/wiz_test.cpp tests/unit/wiz_cli.cpp \
           tests/unit/shelly_test.cpp tests/unit/shelly_cli.cpp src/ui/key_guide.cpp tests/unit/key_guide_test.cpp \
           src/ui/screen_idle.cpp tests/unit/screen_idle_test.cpp tests/unit/models_test.cpp; do
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
$WRAP $CXX -o "$OUT/timers_test" "$OUT/tests_unit_timers_test.o" $timer_objs -ldl -lpthread
$WRAP $CXX -o "$OUT/mic_route_test" "$OUT/tests_unit_mic_route_test.o" $timer_objs -ldl -lpthread
$WRAP $CXX -o "$OUT/music_test" "$OUT/tests_unit_music_test.o" $timer_objs -ldl -lpthread
radio_objs="$(obj_of src/platform/audio.cpp) $(obj_of src/platform/codec.cpp)"
for src in $RADIO; do radio_objs="$radio_objs $(obj_of "$src")"; done
$WRAP $CXX -o "$OUT/radio_test" "$OUT/tests_unit_radio_test.o" $radio_objs $objs -lcurl -lcrypto -ldl -lpthread
$WRAP $CXX -o "$OUT/tools_test" "$OUT/tests_unit_tools_test.o" "$(obj_of src/agent/calc.cpp)" "$(obj_of src/core/util.cpp)" \
    "$(obj_of src/platform/proc.cpp)" -lpthread
# The Fire TV driver's own tests; firetv_cli is what tests/firetv_e2e.py drives against its mock.
$WRAP $CXX -o "$OUT/firetv_test" "$OUT/tests_unit_firetv_test.o" $objs -lcurl -lcrypto -lpthread
$WRAP $CXX -o "$OUT/firetv_cli" "$OUT/tests_unit_firetv_cli.o" $objs -lcurl -lcrypto -lpthread
# The same for the HomeKit driver (tests/homekit_e2e.py drives homekit_cli).
$WRAP $CXX -o "$OUT/homekit_test" "$OUT/tests_unit_homekit_test.o" $objs -lcurl -lcrypto -lpthread
$WRAP $CXX -o "$OUT/homekit_cli" "$OUT/tests_unit_homekit_cli.o" $objs -lcurl -lcrypto -lpthread
# The same for the Hue driver (tests/hue_e2e.py drives hue_cli).
$WRAP $CXX -o "$OUT/hue_test" "$OUT/tests_unit_hue_test.o" $objs -lcurl -lcrypto -lpthread
$WRAP $CXX -o "$OUT/hue_cli" "$OUT/tests_unit_hue_cli.o" $objs -lcurl -lcrypto -lpthread
# The same for the Kasa driver (tests/kasa_e2e.py drives kasa_cli).
$WRAP $CXX -o "$OUT/kasa_test" "$OUT/tests_unit_kasa_test.o" $objs -lcurl -lcrypto -lpthread
$WRAP $CXX -o "$OUT/kasa_cli" "$OUT/tests_unit_kasa_cli.o" $objs -lcurl -lcrypto -lpthread
# The same for the WiZ driver (tests/wiz_e2e.py drives wiz_cli).
$WRAP $CXX -o "$OUT/wiz_test" "$OUT/tests_unit_wiz_test.o" $objs -lcurl -lcrypto -lpthread
$WRAP $CXX -o "$OUT/wiz_cli" "$OUT/tests_unit_wiz_cli.o" $objs -lcurl -lcrypto -lpthread
# The same for the Shelly driver (tests/shelly_e2e.py drives shelly_cli).
$WRAP $CXX -o "$OUT/shelly_test" "$OUT/tests_unit_shelly_test.o" $objs -lcurl -lcrypto -lpthread
$WRAP $CXX -o "$OUT/shelly_cli" "$OUT/tests_unit_shelly_cli.o" $objs -lcurl -lcrypto -lpthread
# What each key does on the chat screen, and the key guide that lists them (no screen needed).
$WRAP $CXX -o "$OUT/key_guide_test" "$OUT/tests_unit_key_guide_test.o" "$(obj_of src/ui/key_guide.cpp)"
# When the screen dims and goes off, and the backlight a crash puts back (no screen needed).
$WRAP $CXX -o "$OUT/screen_idle_test" "$OUT/tests_unit_screen_idle_test.o" "$(obj_of src/ui/screen_idle.cpp)" "$(obj_of src/platform/sys.cpp)"
# Which models are chosen without the user, and which settings offer (ui/models.h: inline, nothing to link).
$WRAP $CXX -o "$OUT/models_test" "$OUT/tests_unit_models_test.o"
# Files saved by two threads at once, the settings shared between threads, all-day alerts when the clocks change.
$WRAP $CXX -o "$OUT/core_test" "$OUT/tests_unit_core_test.o" "$(obj_of src/core/settings.cpp)" "$(obj_of src/core/store.cpp)" \
    "$(obj_of src/core/upgrade.cpp)" "$(obj_of src/core/util.cpp)" "$(obj_of src/platform/proc.cpp)" -lpthread
# DIAGNOSTICS (temporary): saved conversations.
$WRAP $CXX -o "$OUT/activity_test" "$OUT/tests_unit_activity_test.o" "$(obj_of src/core/activity.cpp)" \
    "$(obj_of src/core/settings.cpp)" "$(obj_of src/core/util.cpp)" -lpthread
if [ "${ZB_UNIT_BUILD_ONLY:-}" = 1 ]; then
    echo "built the unit test programs into $OUT (ZB_UNIT_BUILD_ONLY=1: nothing run)"
    exit 0
fi

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
echo "=== Kasa driver"
"$OUT/kasa_test" || failed=1
echo "=== WiZ driver"
"$OUT/wiz_test" || failed=1
echo "=== Shelly driver"
"$OUT/shelly_test" || failed=1
echo "=== keys"
"$OUT/key_guide_test" || failed=1
echo "=== screen off"
"$OUT/screen_idle_test" || failed=1
echo "=== model choice"
"$OUT/models_test" || failed=1
echo "=== core: files saved at once, settings shared between threads, all-day alerts and clock changes, an old drop-in"
"$OUT/core_test" "$state/core" || failed=1
echo "=== diagnostics: saved conversations (temporary)"
"$OUT/activity_test" "$state/activity" || failed=1

echo "=== radio"
TMPDIR="$state" python3 -I tests/unit/radio_peer.py "$OUT/radio_test" || failed=1

echo "=== tools: calculator, dates, time zones, helper programs"
TZ=UTC "$OUT/tools_test" "$state" || failed=1

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
echo "=== the built-in microphone with a plug in the jack"
"$OUT/mic_route_test" || failed=1
echo "=== music: MP3 decoded in the app and played by pacat"
"$OUT/music_test" || failed=1
for f in calendar memory reminders; do
    if ! cmp -s "$home/.local/share/zero-bot/$f.json" "$state/$f.expected"; then
        echo "FAIL the tests changed $f.json in the home folder they were run from"
        failed=1
    fi
done

cli=build/cli/zero_bot
if [ ! -x "$cli" ]; then
    echo "=== $cli not built: the timer tool and request loop checks through it were skipped"
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

echo "=== radio through the command-line build"
# A dry run: stations are matched and nothing is checked or played.
radio_state="$state/radio-cli"
mkdir -p "$radio_state/config/zero-bot"
passed=0
radio_failed=0
in_radio_state() {
    HOME="$radio_state" XDG_CONFIG_HOME="$radio_state/config" XDG_DATA_HOME="$radio_state/data" \
        XDG_CACHE_HOME="$radio_state/cache" ZB_DRY_RUN=1 ZB_TEST_NETWORK=1 "$@"
}
# radio_expect WHAT OUTPUT TEXT [absent]: OUTPUT must contain TEXT (or, with "absent", must not, ignoring case).
radio_expect() {
    if [ "${4:-}" = absent ]; then
        if printf '%s' "$2" | grep -qi -- "$3"; then radio_failed=$((radio_failed + 1)); echo "FAIL $1: \"$3\" should not appear"
        else passed=$((passed + 1)); fi
    else
        case "$2" in
            *"$3"*) passed=$((passed + 1)) ;;
            *) radio_failed=$((radio_failed + 1)); echo "FAIL $1"; echo "     expected: $3"; echo "     got:      $2" ;;
        esac
    fi
}
# No station file: no tool, and not a word about radio in the system prompt.
radio_expect "the tool list with no station file" "$(in_radio_state "$cli" --schema)" '"name": "radio"' absent
radio_expect "the system prompt with no station file" "$(in_radio_state "$cli" --prompt)" 'radio' absent
radio_expect "the tool list without radio still has music" "$(in_radio_state "$cli" --schema)" '"name": "music"'
radio_expect "the radio tool with no stations" "$(in_radio_state "$cli" --tool radio '{"action":"play","station":"Radio 2"}')" \
    'ERROR: no radio stations are set up'
printf '{"stations": []}\n' > "$radio_state/config/zero-bot/radio.json"
radio_expect "the tool list with an empty station list" "$(in_radio_state "$cli" --schema)" '"name": "radio"' absent
radio_expect "the system prompt with an empty station list" "$(in_radio_state "$cli" --prompt)" 'radio' absent
cat > "$radio_state/config/zero-bot/radio.json" <<'STATIONS'
{"stations": [
  {"name": "Test Radio Two", "nicknames": ["T2"], "url": "http://127.0.0.1:9/two.m3u8"},
  {"name": "Test Jazz", "url": "http://127.0.0.1:9/jazz.mp3", "low_data_url": "http://127.0.0.1:9/jazz-low.mp3"}
]}
STATIONS
radio_expect "the tool list with stations" "$(in_radio_state "$cli" --schema)" '"name": "radio"'
radio_expect "the station names as allowed values" "$(in_radio_state "$cli" --schema)" '"Test Radio Two",'
radio_expect "the system prompt with stations" "$(in_radio_state "$cli" --prompt)" 'played with radio: Test Radio Two, Test Jazz'
radio_expect "play by nickname" "$(in_radio_state "$cli" --tool radio '{"action":"play","station":"t2"}')" '{"playing":"Test Radio Two"}'
radio_expect "play by name said in words" "$(in_radio_state "$cli" --tool radio '{"action":"play","station":"test radio two"}')" \
    '{"playing":"Test Radio Two"}'
radio_expect "an unknown station" "$(in_radio_state "$cli" --tool radio '{"action":"play","station":"Radio 4"}')" \
    'ERROR: the user has no station called "Radio 4"'
radio_expect "a phone's connection" "$(ZB_TEST_TETHER=1 in_radio_state "$cli" --tool radio '{"action":"play","station":"Test Jazz"}')" \
    '"stream":"low data'
radio_expect "stop" "$(in_radio_state "$cli" --tool radio '{"action":"stop"}')" 'radio stopped'
echo "radio through the command-line build: $passed passed, $radio_failed failed"
[ "$radio_failed" -eq 0 ] || failed=1

if [ "${ZB_UNIT_SKIP_SCRIPTS:-}" = 1 ]; then
    echo "=== tests/tools_cli.py and tests/agent_loop_test.py left out (ZB_UNIT_SKIP_SCRIPTS=1)"
    exit $failed
fi

echo "=== the other tools that need no network, through the command-line build"
python3 -I tests/tools_cli.py "$cli" || failed=1

echo "=== the request loop against a scripted chat service"
python3 -I tests/agent_loop_test.py -q "$cli" || failed=1
exit $failed
