#!/bin/sh
# Device-free unit tests: the smart-home layer (store, names, schema, safety rules, locking,
# pairing, the Tapo driver's refusals, colours) and the HTTP options for devices. Nothing but
# 127.0.0.1 is contacted and no AI service is needed.
#
#   tests/run_unit.sh                   build into build/unit and run
#   ZB_UPDATE_SNAPSHOTS=1 tests/run_unit.sh    rewrite tests/snapshots/*.json from the current schema
#   ZB_BUILD_WRAPPER=cmd tests/run_unit.sh     run each compile through cmd (e.g. a lock on a shared machine)
set -eu
cd "$(dirname "$0")/.."
OUT=build/unit
mkdir -p "$OUT"
CXX="${CXX:-g++}"
FLAGS="-std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Isrc -Ithird_party \
 -DZB_VERSION=\"unit\" -DZB_DESKTOP=1 -DZB_CLI_ONLY=1"
WRAP="${ZB_BUILD_WRAPPER:-}"

APP="src/smarthome/store.cpp src/smarthome/targets.cpp src/smarthome/control.cpp src/smarthome/tool.cpp
     src/smarthome/discovery.cpp src/smarthome/tapo.cpp src/smarthome/drivers/registry.cpp
     src/smarthome/drivers/tapo_driver.cpp src/core/util.cpp src/platform/proc.cpp src/platform/sys.cpp src/net/http.cpp"

# Objects are rebuilt only when their source or a header in src/ is newer.
newest_header=$(find src -name '*.h' -newer "$OUT/.stamp" 2>/dev/null | head -n 1 || true)
objs=""
for src in $APP tests/unit/smarthome_test.cpp tests/unit/http_test.cpp; do
    obj="$OUT/$(echo "$src" | tr '/' '_' | sed 's/\.cpp$/.o/')"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ -n "$newest_header" ]; then
        echo "compiling $src"
        $WRAP $CXX $FLAGS -c "$src" -o "$obj"
    fi
    case "$src" in tests/*) ;; *) objs="$objs $obj" ;; esac
done
touch "$OUT/.stamp"
$WRAP $CXX -o "$OUT/smarthome_test" "$OUT/tests_unit_smarthome_test.o" $objs -lcurl -lcrypto -lpthread
$WRAP $CXX -o "$OUT/http_test" "$OUT/tests_unit_http_test.o" $objs -lcurl -lcrypto -lpthread

failed=0
echo "=== smart home"
"$OUT/smarthome_test" tests/snapshots || failed=1
echo "=== http options"
python3 -I tests/unit/http_peer.py "$OUT/http_test" || failed=1
exit $failed
