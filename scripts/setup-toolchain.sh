#!/bin/bash
# Fetches everything needed to build, into the project folder. Nothing is installed system-wide
# and no root access is needed.
#
#   .tools/           CMake and Ninja (Python wheels in a private virtual environment)
#   .cache/toolchain  Arm GNU Toolchain 14.2 (aarch64 cross compiler)
#   .cache/sdk_bsp-src  the CardputerZero BSP, used as the sysroot
#   .cache/lvgl       LVGL v9.5.0
#
# Pass --sim-only to skip the cross compiler and sysroot (about 2 GB) if you only want the
# desktop simulator or the command-line build.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SIM_ONLY=0
[ "${1:-}" = "--sim-only" ] && SIM_ONLY=1

if [ ! -x .tools/bin/cmake ]; then
  echo "== CMake and Ninja"
  if command -v uv >/dev/null; then
    uv venv .tools -q && uv pip install -q --python .tools/bin/python cmake ninja
  else
    python3 -m venv .tools && .tools/bin/pip install -q cmake ninja
  fi
fi

mkdir -p .cache
if [ ! -f .cache/lvgl/lvgl.h ]; then
  echo "== LVGL v9.5.0"
  git clone -q --depth 1 -b v9.5.0 https://github.com/lvgl/lvgl.git .cache/lvgl
fi

if [ "$SIM_ONLY" = 0 ]; then
  if [ ! -d .cache/sdk_bsp-src/usr/include ]; then
    echo "== CardputerZero BSP sysroot (about 430 MB download)"
    curl -L --fail -o .cache/sdk_bsp.tar.gz \
      "https://github.com/CardputerZero/M5CardputerZero-UserDemo/releases/download/v0.0.4/sdk_bsp.tar.gz"
    mkdir -p .cache/sdk_bsp-src && tar -xzf .cache/sdk_bsp.tar.gz -C .cache/sdk_bsp-src
    rm -f .cache/sdk_bsp.tar.gz
  fi
  if [ ! -x .cache/toolchain/bin/aarch64-none-linux-gnu-gcc ]; then
    echo "== Arm GNU Toolchain 14.2 (about 150 MB download)"
    F=arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu.tar.xz
    curl -L --fail -o ".cache/$F" "https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/$F" ||
      curl -L --fail -o ".cache/$F" "https://armkeil.blob.core.windows.net/developer/Files/downloads/gnu/14.2.rel1/binrel/$F"
    mkdir -p .cache/toolchain && tar -xJf ".cache/$F" -C .cache/toolchain --strip-components=1
    rm -f ".cache/$F"
  fi
  # The toolchain ships a patched copy of pthread.h generated for its own C library. It does not
  # match the newer glibc in the BSP sysroot and breaks <mutex>, so take it out of the way.
  FIXED=$(echo .cache/toolchain/lib/gcc/aarch64-none-linux-gnu/*/include-fixed/pthread.h)
  [ -f "$FIXED" ] && mv "$FIXED" "$FIXED.disabled"
fi

echo
echo "Ready. Put the tools on your PATH for this shell:"
echo "  export PATH=\"$ROOT/.tools/bin:\$PATH\""
