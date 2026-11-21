#!/usr/bin/env python3
"""Golden screenshots: renders every screen with the simulator build (zero_bot --shots) into a
temporary folder and compares each image with tests/golden/shots/, with a small tolerance.

    tests/shots_compare.py [path/to/sim/zero_bot]      compare (default build/sim/zero_bot)
    ZB_UPDATE_SHOTS=1 tests/shots_compare.py           replace the stored images with this render

Run it before and after a UI change; after an intended change, refresh the images and look at the
diff of every changed PNG before committing it. Images are matched by name without their number,
so a new screen in the middle only adds an image. Where they differ, a picture of the difference
(red where pixels changed) is written to build/shots-diff/.

To make renders repeatable and harmless:
  - the wall clock starts at a fixed moment (tests/shots/fixclock.c, built here with cc and loaded
    with LD_PRELOAD), so dates on the calendar pages and countdowns do not change from day to day;
  - the helper programs the app starts (audio, volume, network tools) are replaced on PATH by
    stand-ins that do nothing, so no sound is played and no setting of this PC is changed; their
    calls are listed;
  - TZ, HOME and the language are fixed.
Fonts come from the system (DejaVu, Noto CJK), so the stored images belong to the machine that
made them: refresh them on a new machine before relying on the comparison.

A pixel counts as changed when a colour channel differs by more than THRESHOLD; an image fails
when more than TOLERANCE of its pixels changed (outside the areas masked for it in
tests/golden/shots/masks.json, for parts that move with real time). Standard library only.
"""
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
APP = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(ROOT, "build", "sim", "zero_bot")
GOLDEN = os.path.join(HERE, "golden", "shots")
DIFFS = os.path.join(ROOT, "build", "shots-diff")
FIXED_TIME = 1795003200          # Wed 18 Nov 2026 12:00 in London
THRESHOLD = 24                   # per colour channel, 0..255
TOLERANCE = 0.001                # share of an image's pixels that may change
HELPERS = ["wpctl", "pw-cat", "pw-play", "pw-record", "pactl", "paplay", "aplay", "arecord", "amixer", "iw", "nmcli",
           "bluetoothctl", "ffmpeg", "ffplay", "rpicam-jpeg", "rpicam-still", "systemctl", "ip"]


# ------------------------------------------------------------------ PNG in and out (8-bit RGB/RGBA, grey, palette)
def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n": raise ValueError("%s is not a PNG" % path)
    pos, idat, palette = 8, b"", None
    width = height = depth = ctype = interlace = None
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b"IHDR": width, height, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE": palette = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif kind == b"IDAT": idat += body
        elif kind == b"IEND": break
    if depth != 8 or interlace: raise ValueError("%s: only 8-bit, non-interlaced PNGs are read" % path)
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = width * channels
    rows, prev, at = [], bytearray(stride), 0
    for _ in range(height):
        f, line = raw[at], bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    pixels = []
    for line in rows:
        for x in range(width):
            px = line[x * channels:(x + 1) * channels]
            if ctype == 3: pixels.append(palette[px[0]])
            elif ctype in (0, 4): pixels.append((px[0], px[0], px[0]))
            else: pixels.append(tuple(px[:3]))
    return width, height, pixels


def write_png(path, width, height, pixels):
    raw = b"".join(b"\x00" + bytes(v for px in pixels[y * width:(y + 1) * width] for v in px) for y in range(height))
    def chunk(kind, body): return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def key(filename):
    """"08-delete-once.png" -> "delete-once": images are matched by name, not by their place in the run."""
    stem = filename[:-4]
    return stem.split("-", 1)[1] if stem[:2].isdigit() and "-" in stem else stem


# ------------------------------------------------------------------ rendering
def render(out):
    tmp = os.path.dirname(out)
    bin_dir = os.path.join(tmp, "bin")
    os.makedirs(bin_dir)
    calls = os.path.join(tmp, "helper-calls.log")
    for name in HELPERS:
        p = os.path.join(bin_dir, name)
        with open(p, "w") as f:
            f.write("#!/bin/sh\necho \"%s $*\" >> '%s'\nexit 1\n" % (name, calls))
        os.chmod(p, 0o755)
    env = dict(os.environ, PATH=bin_dir + os.pathsep + os.environ.get("PATH", ""), TZ="Europe/London", LANG="C.UTF-8",
               LC_ALL="C.UTF-8", HOME=tmp, ZB_NO_AUDIO="1", ZB_FIXED_TIME=str(FIXED_TIME))
    for v in ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "ZB_API_BASE", "ZB_API_KEY", "ZB_SEARCH_KEY",
              "ZB_DATE_ORDER", "ZB_FAST_MODE", "LD_PRELOAD"):
        env.pop(v, None)
    env["ZB_TEST_NETWORK"] = "1"   # the stand-in network from the start: the real one is never identified
    shim = os.path.join(tmp, "fixclock.so")
    cc = shutil.which(os.environ.get("CC", "cc")) or shutil.which("gcc")
    if cc and subprocess.run([cc, "-shared", "-fPIC", "-O2", "-o", shim, os.path.join(HERE, "shots", "fixclock.c"), "-ldl"]).returncode == 0:
        env["LD_PRELOAD"] = shim
    else:
        print("warning: no C compiler for the clock shim; screens with dates will differ from day to day")
    p = subprocess.run([APP, "--shots", out], env=env, capture_output=True, text=True, timeout=120)
    if p.returncode != 0:
        sys.exit("--shots failed (exit %d):\n%s%s" % (p.returncode, p.stdout[-2000:], p.stderr[-2000:]))
    helpers = sorted(set(l.split()[0] for l in open(calls))) if os.path.exists(calls) else []
    return helpers, p.stderr


def main():
    if not os.access(APP, os.X_OK):
        sys.exit("no app at %s (build the sim preset first)" % APP)
    tmp = tempfile.mkdtemp(prefix="zb-shots-")
    try:
        out = os.path.join(tmp, "out")
        helpers, stderr = render(out)
        shots = {key(f): os.path.join(out, f) for f in sorted(os.listdir(out)) if f.endswith(".png")}
        print("%d screens rendered%s" % (len(shots), "; stand-ins answered: " + ", ".join(helpers) if helpers else ""))
        if stderr.strip(): print("the renderer reported:\n" + stderr.strip())
        if os.environ.get("ZB_UPDATE_SHOTS") == "1":
            os.makedirs(GOLDEN, exist_ok=True)
            old = {key(f) for f in os.listdir(GOLDEN) if f.endswith(".png")}
            for f in os.listdir(GOLDEN):
                if f.endswith(".png"): os.remove(os.path.join(GOLDEN, f))
            for name, path in shots.items(): shutil.copy(path, os.path.join(GOLDEN, os.path.basename(path)))
            print("stored %d images in %s (new: %s; gone: %s). Look at the diff before committing."
                  % (len(shots), os.path.relpath(GOLDEN, ROOT), ", ".join(sorted(set(shots) - old)) or "none",
                     ", ".join(sorted(old - set(shots))) or "none"))
            return 0
        golden = {key(f): os.path.join(GOLDEN, f) for f in sorted(os.listdir(GOLDEN)) if f.endswith(".png")} if os.path.isdir(GOLDEN) else {}
        if not golden: sys.exit("no stored images in %s: run with ZB_UPDATE_SHOTS=1 first" % os.path.relpath(GOLDEN, ROOT))
        masks_path = os.path.join(GOLDEN, "masks.json")
        masks = json.load(open(masks_path)) if os.path.exists(masks_path) else {}
        failed = 0
        shutil.rmtree(DIFFS, ignore_errors=True)
        for name in sorted(set(shots) | set(golden)):
            if name not in golden:
                print("FAIL  %s: a new screen with no stored image (ZB_UPDATE_SHOTS=1 stores it)" % name); failed += 1; continue
            if name not in shots:
                print("FAIL  %s: a stored screen that was not rendered" % name); failed += 1; continue
            w, h, a = read_png(golden[name])
            w2, h2, b = read_png(shots[name])
            if (w, h) != (w2, h2):
                print("FAIL  %s: size %dx%d, stored %dx%d" % (name, w2, h2, w, h)); failed += 1; continue
            masked = set()
            for x0, y0, x1, y1 in masks.get(name, []):
                masked.update(y * w + x for y in range(y0, y1 + 1) for x in range(x0, x1 + 1))
            changed = [i for i in range(w * h) if i not in masked and max(abs(p - q) for p, q in zip(a[i], b[i])) > THRESHOLD]
            if len(changed) > TOLERANCE * w * h:
                os.makedirs(DIFFS, exist_ok=True)
                diff = [(p[0] // 3, p[1] // 3, p[2] // 3) for p in a]
                for i in changed: diff[i] = (255, 0, 0)
                write_png(os.path.join(DIFFS, name + ".png"), w, h, diff)
                shutil.copy(shots[name], os.path.join(DIFFS, name + "-now.png"))
                xs, ys = [i % w for i in changed], [i // w for i in changed]
                print("FAIL  %s: %d pixels changed, in x %d..%d, y %d..%d (see %s)" % (name, len(changed), min(xs), max(xs), min(ys), max(ys),
                                                                                     os.path.relpath(os.path.join(DIFFS, name + ".png"), ROOT)))
                failed += 1
            else:
                print("ok    %s%s" % (name, " (%d pixels changed)" % len(changed) if changed else ""))
        print("%d screens, %d differ" % (len(set(shots) | set(golden)), failed))
        return 1 if failed else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
