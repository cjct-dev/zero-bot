#!/bin/sh
# Device-free tests of the timers. Needs no network, no sound and no screen.
#
#   tests/run_timer_tests.sh
#
# 1. Builds tests/unit/timers_test.cpp with the few sources it needs and runs it.
# 2. If the command-line build exists (build/cli/zero_bot), runs the timer tool through it:
#    zero_bot --tool timer '{...}', with a fresh, temporary state folder.
# CXX picks the compiler (default g++). Exits non-zero when anything fails.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/build/tests"
mkdir -p "$out"
state=$(mktemp -d)
trap 'rm -rf "$state"' EXIT INT TERM

echo "== building the timer unit tests"
${CXX:-g++} -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
    -I"$root/src" -I"$root/third_party" \
    "$root/tests/unit/timers_test.cpp" "$root/src/core/timers.cpp" "$root/src/core/store.cpp" \
    "$root/src/core/util.cpp" "$root/src/platform/audio.cpp" "$root/src/platform/proc.cpp" \
    -o "$out/timers_test" -pthread

echo "== running them"
HOME="$state" XDG_CONFIG_HOME="$state/config" XDG_DATA_HOME="$state/data" XDG_CACHE_HOME="$state/cache" TZ=UTC \
    "$out/timers_test"

cli="$root/build/cli/zero_bot"
if [ ! -x "$cli" ]; then
    echo "== $cli not built: the tool checks through it were skipped"
    exit 0
fi

echo "== the timer tool through the command-line build"
tool_state="$state/cli"
mkdir -p "$tool_state"
passed=0
failed=0
# expect ARGS TEXT: the tool's result for ARGS must contain TEXT.
expect() {
    result=$(HOME="$tool_state" XDG_CONFIG_HOME="$tool_state/config" XDG_DATA_HOME="$tool_state/data" \
             XDG_CACHE_HOME="$tool_state/cache" ZB_DRY_RUN=1 "$cli" --tool timer "$1")
    case "$result" in
        *"$2"*) passed=$((passed + 1)) ;;
        *) failed=$((failed + 1)); echo "FAIL --tool timer '$1'"; echo "     expected: $2"; echo "     got:      $result" ;;
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
echo "timer tool: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
