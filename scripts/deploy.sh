#!/bin/bash
# Development deploy: copies a freshly cross-built binary, the launcher entry and the icon to a
# CardputerZero attached over USB (ADB must be enabled in the device's Settings > Developer).
#
#   cmake --preset cp0 && cmake --build --preset cp0 && scripts/deploy.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ADB="$ROOT/scripts/adbx"
BIN="$ROOT/build/cp0/zero_bot"
[ -x "$BIN" ] || { echo "Build the device binary first: cmake --preset cp0 && cmake --build --preset cp0" >&2; exit 1; }

# Transfers over this USB link have been seen to arrive with flipped bytes, so every file is
# checked against its checksum on the device before it is installed, and re-sent if it differs.
push_checked() {
  local src="$1" dst="$2" want got
  want=$(md5sum "$src" | cut -d' ' -f1)
  for attempt in 1 2 3; do
    "$ADB" push "$src" "$dst" >/dev/null
    got=$("$ADB" shell "md5sum '$dst'" | cut -d' ' -f1)
    [ "$got" = "$want" ] && return 0
    echo "checksum mismatch for $(basename "$src") (attempt $attempt), sending again" >&2
  done
  echo "could not transfer $(basename "$src") intact" >&2
  return 1
}

# The device gets the binary without its symbols, a quarter smaller (as the package does);
# build/cp0 keeps them for reading a crash.
STRIP="$ROOT/.cache/toolchain/bin/aarch64-none-linux-gnu-strip"
SHIPPED=$(mktemp)
trap 'rm -f "$SHIPPED"' EXIT
cp "$BIN" "$SHIPPED"
[ -x "$STRIP" ] && "$STRIP" "$SHIPPED"

push_checked "$SHIPPED" /tmp/zero_bot.new
push_checked "$ROOT/packaging/zero_bot.desktop" /tmp/zero_bot.desktop
push_checked "$ROOT/packaging/zero_bot.png" /tmp/zero_bot.png
"$ADB" shell '
  set -e
  install -m 0755 /tmp/zero_bot.new /usr/bin/zero_bot
  install -m 0644 /tmp/zero_bot.png /usr/share/APPLaunch/share/images/zero_bot.png
  install -m 0644 /tmp/zero_bot.desktop /usr/share/APPLaunch/applications/zero_bot.desktop
  rm -f /tmp/zero_bot.new /tmp/zero_bot.png /tmp/zero_bot.desktop
  sync
  /usr/bin/zero_bot --version
'
echo "Installed. Open \"Zero Bot\" from the launcher on the device."
