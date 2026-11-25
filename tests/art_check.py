#!/usr/bin/env python3
"""The art's pictures (assets/art/<folder>/*.png: a scene's, or a music family's), checked as the app and the publishing
rules need them:

    tests/art_check.py                every picture in assets/art/
    tests/art_check.py --self-test    this checker, against pictures made wrong on purpose

- Only PNGs, named in small letters, digits and dashes, one folder per scene.
- Each an 8-bit indexed PNG holding nothing but IHDR, PLTE, tRNS, IDAT and IEND (no text, time, colour profile,
  physical size or other metadata that a program adds), every checksum right, not interlaced.
- Its palette: 24 colours at most after index 0, which is the clear one (tRNS is exactly one byte, 0), each colour
  exact in RGB565 (the panel's colours, so the screenshots and the device show the same); no pixel outside it.
- A picture is named <part>[-<variant>][-<lighting>].png: "land-dusk", the city's "land-rain-night" (its weather),
  "flames" (the same in every lighting). A part's kind is its first word.
- The sizes a scene expects: its layers (sky, land, lights, twinkle) are 160x85 art pixels.
- Each folder has a sky and a land for dusk and for night (of each variant it has), and the land is the same
  picture in both (identical pixels, two palettes: the owner's "same scene" rule). So is every other part drawn in
  both lightings (a sprite sheet), but the sky and what each lighting has of its own (lights, twinkle, the roof the
  rain splashes on), which are free to differ.

Exits 1 when anything is wrong, with a line for each. Standard library only."""
import os
import re
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ART = os.path.join(ROOT, "assets", "art")
CHUNKS = (b"IHDR", b"PLTE", b"tRNS", b"IDAT", b"IEND")
MAX_COLOURS = 24
LAYER = (160, 85)
LAYERS = ("sky", "land", "lights", "twinkle")     # full-size layers: 160x85
FREE = ("sky", "lights", "twinkle", "roof")       # parts each lighting draws its own way
LIGHTINGS = ("dusk", "night")
NAME = re.compile(r"^[a-z0-9]+(-[a-z0-9]+)*\.png$")


class Bad(Exception):
    pass


def exact565(c):
    r, g, b = c
    return (r >> 3) << 3 | r >> 5 == r and (g >> 2) << 2 | g >> 6 == g and (b >> 3) << 3 | b >> 5 == b


def chunks(data):
    """The chunks in order, as (kind, body); Bad for anything not a well-formed PNG."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise Bad("not a PNG")
    out, at = [], 8
    while at < len(data):
        if at + 12 > len(data):
            raise Bad("cut short")
        n, kind = struct.unpack(">I4s", data[at:at + 8])
        body, crc = data[at + 8:at + 8 + n], data[at + 8 + n:at + 12 + n]
        if len(body) != n or len(crc) != 4:
            raise Bad("cut short")
        if struct.unpack(">I", crc)[0] != zlib.crc32(kind + body) & 0xffffffff:
            raise Bad("a wrong checksum in %s" % kind.decode("latin-1"))
        out.append((kind, body))
        at += 12 + n
    return out


def unfilter(raw, w, h):
    rows, prev, at = [], bytearray(w), 0
    for _ in range(h):
        if at + 1 + w > len(raw):
            raise Bad("its pixels are cut short")
        f, line = raw[at], bytearray(raw[at + 1:at + 1 + w])
        at += 1 + w
        for i in range(w):
            a = line[i - 1] if i else 0
            b, c = prev[i], prev[i - 1] if i else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
            elif f:
                raise Bad("an unknown row filter")
        rows.append(bytes(line))
        prev = line
    return rows


def picture(data):
    """(width, height, palette, rows of indices) of a picture that passes every rule for one file; Bad if not."""
    found = chunks(data)
    kinds = [k for k, _ in found]
    extra = sorted({k.decode("latin-1") for k in kinds if k not in CHUNKS})
    if extra:
        raise Bad("holds more than its pixels and palette: %s" % ", ".join(extra))
    order = [k for k in kinds if k != b"IDAT"]
    if order != [b"IHDR", b"PLTE", b"tRNS", b"IEND"] or kinds[-1] != b"IEND" or kinds.count(b"IDAT") < 1 or \
            kinds.index(b"IDAT") < kinds.index(b"tRNS"):
        raise Bad("its chunks are not IHDR, PLTE, tRNS, IDAT, IEND, in that order")
    body = dict((k, b) for k, b in found if k != b"IDAT")
    if len(body[b"IHDR"]) != 13:
        raise Bad("a broken header")
    w, h, depth, ctype, comp, filt, interlace = struct.unpack(">IIBBBBB", body[b"IHDR"])
    if (depth, ctype, comp, filt, interlace) != (8, 3, 0, 0, 0):
        raise Bad("not an 8-bit indexed, non-interlaced picture")
    plte = body[b"PLTE"]
    palette = [tuple(plte[i:i + 3]) for i in range(0, len(plte), 3)]
    if len(plte) % 3 or not 2 <= len(palette) <= MAX_COLOURS + 1:
        raise Bad("a palette of %d entries: index 0 and 1 to %d colours" % (len(palette), MAX_COLOURS))
    if body[b"tRNS"] != b"\x00":
        raise Bad("index 0 is not the one clear entry (tRNS must be one byte, 0)")
    inexact = [c for c in palette[1:] if not exact565(c)]
    if inexact:
        raise Bad("colours not exact in RGB565: %s" % " ".join("#%02x%02x%02x" % c for c in inexact))
    try:
        raw = zlib.decompress(b"".join(b for k, b in found if k == b"IDAT"))
    except zlib.error:
        raise Bad("its pixels cannot be read")
    rows = unfilter(raw, w, h)
    if any(v >= len(palette) for row in rows for v in row):
        raise Bad("a pixel outside the palette")
    return w, h, palette, rows


def check_scene(name, files, say):
    """The rules across a scene's pictures; `files` maps a file name to (w, h, palette, rows)."""
    parts = {}
    for f, pic in files.items():
        part, _, lighting = f[:-4].rpartition("-")
        if lighting not in LIGHTINGS:   # drawn the same in every lighting ("light.png")
            part, lighting = f[:-4], ""
        parts.setdefault(part, {})[lighting] = pic
    kind = lambda part: part.split("-")[0]
    for need in ("sky", "land"):
        variants = [p for p in parts if kind(p) == need] or [need]
        for part in variants:
            for lighting in LIGHTINGS:
                if lighting not in parts.get(part, {}):
                    say("%s: no %s-%s.png" % (name, part, lighting))
    for part, by_lighting in sorted(parts.items()):
        for lighting, (w, h, _, _) in by_lighting.items():
            if kind(part) in LAYERS and (w, h) != LAYER:
                say("%s: %s is %dx%d, not %dx%d" % (name, part + ("-" + lighting if lighting else ""), w, h, *LAYER))
        drawn = [by_lighting[l] for l in LIGHTINGS if l in by_lighting]
        if kind(part) not in FREE and len(drawn) > 1 and any(d[:2] != drawn[0][:2] or d[3] != drawn[0][3] for d in drawn[1:]):
            say("%s: %s is not the same picture at dusk and at night (only its palette may differ)" % (name, part))


def check_tree(art, say):
    """Every rule over a folder of scenes; returns how many pictures were read."""
    count = 0
    for scene in sorted(os.listdir(art)):
        folder = os.path.join(art, scene)
        if not os.path.isdir(folder):
            say("%s: not a scene's folder" % scene)
            continue
        files = {}
        for f in sorted(os.listdir(folder)):
            if not NAME.match(f):
                say("%s/%s: only PNGs named in small letters, digits and dashes" % (scene, f))
                continue
            with open(os.path.join(folder, f), "rb") as fh:
                data = fh.read()
            count += 1
            try:
                files[f] = picture(data)
            except Bad as e:
                say("%s/%s: %s" % (scene, f, e))
        check_scene(scene, files, say)
    return count


# ------------------------------------------------------------------ self-test
def make_png(w, h, palette, rows, extra=(), trns=b"\x00", ctype=3, depth=8, interlace=0, crc_ok=True):
    def chunk(kind, data):
        crc = zlib.crc32(kind + data) & 0xffffffff
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", crc if crc_ok else crc ^ 1)
    raw = b"".join(b"\x00" + bytes(r) for r in rows)
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, interlace))
    out += chunk(b"PLTE", b"".join(bytes(c) for c in palette))
    if trns is not None:
        out += chunk(b"tRNS", trns)
    for kind, data in extra:
        out += chunk(kind, data)
    return out + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


def self_test():
    import tempfile
    failures = []

    def expect(what, ok):
        if not ok:
            failures.append(what)

    def refused(what, data, words):
        try:
            picture(data)
            expect(what + ": refused", False)
        except Bad as e:
            expect(what + ": refused for " + words + " (said: %s)" % e, words in str(e))

    pal = [(0, 0, 0), (0xff, 0xff, 0xff), (0x08, 0x10, 0x18)]
    good = make_png(2, 1, pal, [[1, 2]])
    expect("a good picture passes", picture(good)[:2] == (2, 1))
    refused("a text chunk", make_png(2, 1, pal, [[1, 2]], extra=[(b"tEXt", b"Software\x00x")]), "more than its pixels")
    refused("a time chunk", make_png(2, 1, pal, [[1, 2]], extra=[(b"tIME", b"\x07\xea\x0b\x19\x00\x00\x00")]), "more than its pixels")
    refused("no tRNS", make_png(2, 1, pal, [[1, 2]], trns=None), "chunks are not")
    refused("tRNS for more than index 0", make_png(2, 1, pal, [[1, 2]], trns=b"\x00\xff"), "index 0")
    refused("a colour picture", make_png(1, 1, pal, [[1, 2, 3]], ctype=2), "8-bit indexed")
    refused("an interlaced picture", make_png(2, 1, pal, [[1, 2]], interlace=1), "8-bit indexed")
    refused("a colour not exact in RGB565", make_png(2, 1, [(0, 0, 0), (0x09, 0x10, 0x31)], [[1, 1]]), "RGB565")
    refused("25 colours", make_png(1, 1, [(0, 0, 0)] + [(8 * i, 0, 0) for i in range(25)], [[1]]), "palette of 26")
    refused("a pixel outside the palette", make_png(2, 1, pal, [[1, 5]]), "outside the palette")
    refused("a wrong checksum", make_png(2, 1, pal, [[1, 2]], crc_ok=False), "checksum")
    refused("cut short", good[:-20], "cut short")
    refused("not a PNG", b"GIF89a", "not a PNG")

    def scene_problems(pictures):
        with tempfile.TemporaryDirectory() as tmp:
            os.makedirs(os.path.join(tmp, "test"))
            for name, data in pictures.items():
                with open(os.path.join(tmp, "test", name), "wb") as fh:
                    fh.write(data)
            said = []
            check_tree(tmp, said.append)
            return said
    full = [[1] * 160 for _ in range(85)]
    other = [[2] * 160 for _ in range(85)]
    night = [(0, 0, 0), (0x08, 0x08, 0x08), (0x10, 0x10, 0x10)]
    layer, layer_night = make_png(160, 85, pal, full), make_png(160, 85, night, full)
    base = {"sky-dusk.png": layer, "sky-night.png": make_png(160, 85, night, other), "land-dusk.png": layer,
            "land-night.png": layer_night}
    expect("a whole scene passes: %s" % scene_problems(base), scene_problems(base) == [])
    expect("a land that differs at night", any("same picture" in p for p in scene_problems(dict(base, **{"land-night.png": make_png(160, 85, night, other)}))))
    expect("a sprite sheet that differs at night", any("rings is not the same" in p for p in scene_problems(dict(
        base, **{"rings-dusk.png": make_png(2, 1, pal, [[1, 2]]), "rings-night.png": make_png(2, 1, night, [[2, 1]])}))))
    expect("lights free to differ", scene_problems(dict(base, **{"lights-night.png": make_png(160, 85, night, other)})) == [])
    expect("a layer of the wrong size", any("is 2x1" in p for p in scene_problems(dict(base, **{"lights-night.png": good}))))
    expect("no night sky", any("no sky-night" in p for p in scene_problems({k: v for k, v in base.items() if k != "sky-night.png"})))
    expect("a file that is not a PNG", any("only PNGs" in p for p in scene_problems(dict(base, **{"notes.txt": b"x"}))))
    weather = {"sky-rain-dusk.png": layer, "sky-rain-night.png": make_png(160, 85, night, other), "land-rain-dusk.png": layer,
               "land-rain-night.png": layer_night}
    expect("a scene in its weather passes: %s" % scene_problems(weather), scene_problems(weather) == [])
    expect("a weather's land that differs at night", any("land-rain is not the same" in p for p in scene_problems(
        dict(weather, **{"land-rain-night.png": make_png(160, 85, night, other)}))))
    expect("a weather without its night sky", any("no sky-rain-night" in p for p in scene_problems(
        {k: v for k, v in weather.items() if k != "sky-rain-night.png"})))
    expect("a weather's layer of the wrong size", any("is 2x1" in p for p in scene_problems(dict(weather, **{"lights-rain-dusk.png": good}))))
    expect("a name with capitals", any("only PNGs" in p for p in scene_problems(dict(base, **{"Sky-dusk.png": layer}))))
    for f in failures:
        print("FAIL", f)
    print("art_check self-test: %d failed" % len(failures))
    return 1 if failures else 0


def main():
    if sys.argv[1:] == ["--self-test"]:
        return self_test()
    problems = []
    count = check_tree(ART, problems.append)
    for p in problems:
        print("FAIL", p)
    print("art: %d pictures checked, %d problems" % (count, len(problems)))
    return 1 if problems or not count else 0


if __name__ == "__main__":
    sys.exit(main())
