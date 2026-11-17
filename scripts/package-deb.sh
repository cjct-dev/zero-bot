#!/bin/bash
# Builds dist/zero-bot_<version>_arm64.deb from the cross-compiled binary. Uses only ar and tar,
# so it works on hosts without dpkg.
#
#   cmake --preset cp0 && cmake --build --preset cp0 && scripts/package-deb.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/cp0/zero_bot"
[ -x "$BIN" ] || { echo "Build the device binary first: cmake --preset cp0 && cmake --build --preset cp0" >&2; exit 1; }
VERSION=$(sed -n 's/^project(zero_bot VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")
STRIP="$ROOT/.cache/toolchain/bin/aarch64-none-linux-gnu-strip"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
PKG="$WORK/pkg"
install -D -m 0755 "$BIN" "$PKG/usr/bin/zero_bot"
[ -x "$STRIP" ] && "$STRIP" "$PKG/usr/bin/zero_bot"
install -D -m 0644 "$ROOT/packaging/zero_bot.desktop" "$PKG/usr/share/APPLaunch/applications/zero_bot.desktop"
install -D -m 0644 "$ROOT/packaging/zero_bot.png" "$PKG/usr/share/APPLaunch/share/images/zero_bot.png"
install -D -m 0644 "$ROOT/README.md" "$PKG/usr/share/doc/zero-bot/README.md"
install -D -m 0644 "$ROOT/LICENSE" "$PKG/usr/share/doc/zero-bot/copyright"
install -D -m 0644 "$ROOT/licenses/THIRD-PARTY-NOTICES.md" "$PKG/usr/share/doc/zero-bot/THIRD-PARTY-NOTICES.md"
install -D -m 0644 "$ROOT/licenses/Apache-2.0.txt" "$PKG/usr/share/doc/zero-bot/Apache-2.0.txt"

SIZE=$(du -sk "$PKG" | cut -f1)
mkdir -p "$WORK/control"
cat > "$WORK/control/control" <<CONTROL
Package: zero-bot
Version: $VERSION
Architecture: arm64
Maintainer: Zero Bot contributors
Section: APPLaunch
Priority: optional
Installed-Size: $SIZE
Depends: libc6, libcurl4t64 | libcurl4, libfreetype6, libpng16-16t64 | libpng16-16, zlib1g, pipewire-bin, wireplumber, ffmpeg
Recommends: libmp3lame0, libopusfile0, bubblewrap, python3, rpicam-apps-lite | rpicam-apps, fonts-dejavu-core, fonts-noto-cjk
Description: Voice assistant with tools for the M5Stack CardputerZero
 Push-to-talk and typed conversations with any OpenAI-compatible service,
 with a calculator, sandboxed Python, camera, calendar, reminders, memory,
 web search, weather, currency, stock prices, music and a TV remote.
CONTROL

mkdir -p "$ROOT/dist"
OUT="$ROOT/dist/zero-bot_${VERSION}_arm64.deb"
( cd "$WORK/control" && tar --owner=0 --group=0 -czf "$WORK/control.tar.gz" ./control )
( cd "$PKG" && tar --owner=0 --group=0 -czf "$WORK/data.tar.gz" . )
printf '2.0\n' > "$WORK/debian-binary"
rm -f "$OUT"
( cd "$WORK" && ar rc "$OUT" debian-binary control.tar.gz data.tar.gz )
echo "$OUT"
