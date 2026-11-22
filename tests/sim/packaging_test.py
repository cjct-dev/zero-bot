#!/usr/bin/env python3
"""The package scripts/package-deb.sh builds, checked without dpkg and without a device build.

It copies what the script reads into a temporary tree, with a stand-in binary, runs it there, and reads the
package back with its own ar and tar reader: the files (Zero's wake-screen picture among them), owners, modes,
control fields and maintainer scripts. The maintainer scripts are then run for every action dpkg can give them,
against a stand-in dpkg-divert that records its arguments (and a stand-in dpkg for --compare-versions): the
launcher's picture is diverted before Zero's is unpacked, on a first install and on an upgrade from a version
without it alike, put back when the app goes or a first install fails, and a failed diversion stops the install.

    tests/sim/packaging_test.py [-v]

Standard library only.
"""
import collections
import io
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))   # the repository
IMAGE = "/usr/share/APPLaunch/share/images/lockscreen.png"
DIVERT = ["--divert", IMAGE + ".distrib", IMAGE]
ADD = ["--package", "zero-bot", "--add", "--rename"] + DIVERT
REMOVE = ["--package", "zero-bot", "--remove", "--rename"] + DIVERT
# What the script reads (besides the binary, which a stand-in replaces).
INPUTS = ("scripts/package-deb.sh", "CMakeLists.txt", "README.md", "LICENSE", "licenses/THIRD-PARTY-NOTICES.md",
          "licenses/Apache-2.0.txt", "packaging/zero_bot.desktop", "packaging/zero_bot.png",
          "packaging/lockscreen.png", "packaging/preinst", "packaging/postrm")

failures = []
counted = [0]
VERBOSE = "-v" in sys.argv


def check(name, ok, detail=""):
    counted[0] += 1
    if not ok:
        failures.append(name)
        print("FAIL %s%s" % (name, (": %s" % (detail,)) if detail != "" else ""))
    elif VERBOSE:
        print("ok   %s" % name)


def ar_members(path):
    """The members of an ar archive, in order: [(name, bytes)]."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"!<arch>\n":
        return None
    out, pos = [], 8
    while pos < len(data):
        header = data[pos:pos + 60]
        name = header[:16].decode("ascii").strip().rstrip("/")
        size = int(header[48:58].decode("ascii").strip())
        out.append((name, data[pos + 60:pos + 60 + size]))
        pos += 60 + size + (size & 1)
    return out


Entry = collections.namedtuple("Entry", "mode uid gid isfile data")


def tar_entries(blob):
    """{path without the leading "./": Entry (data: a regular file's bytes, else None)}."""
    out = {}
    with tarfile.open(fileobj=io.BytesIO(blob), mode="r:gz") as t:
        for m in t.getmembers():
            data = t.extractfile(m).read() if m.isfile() else None
            out[m.name[2:] if m.name.startswith("./") else m.name] = Entry(m.mode, m.uid, m.gid, m.isfile(), data)
    return out


def png_facts(blob):
    """(width, height, bit depth, colour type, [chunk types]) of a PNG whose CRCs hold, or None."""
    if blob[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    pos, chunks, ihdr = 8, [], None
    while pos < len(blob):
        length, kind = struct.unpack(">I4s", blob[pos:pos + 8])
        body = blob[pos + 8:pos + 8 + length]
        if zlib.crc32(kind + body) != struct.unpack(">I", blob[pos + 8 + length:pos + 12 + length])[0]:
            return None
        chunks.append(kind.decode("latin-1"))
        if kind == b"IHDR":
            ihdr = struct.unpack(">IIBB", body[:10])
        pos += 12 + length
    return ihdr + (chunks,) if ihdr else None


def tree_file(rel):
    with open(os.path.join(ROOT, rel), "rb") as f:
        return f.read()


def build(tree):
    """Runs the packaging script in TREE with a stand-in binary; returns the package's path."""
    for rel in INPUTS:
        os.makedirs(os.path.dirname(os.path.join(tree, rel)), exist_ok=True)
        shutil.copy2(os.path.join(ROOT, rel), os.path.join(tree, rel))
    binary = os.path.join(tree, "build/cp0/zero_bot")
    os.makedirs(os.path.dirname(binary))
    with open(binary, "w") as f:
        f.write("#!/bin/sh\necho stand-in\n")
    os.chmod(binary, 0o755)
    run = subprocess.run(["bash", os.path.join(tree, "scripts/package-deb.sh")], capture_output=True, text=True,
                         env={"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": tree, "LC_ALL": "C"})
    check("package-deb.sh runs", run.returncode == 0, run.stderr.strip())
    return run.stdout.strip()


def check_package(path, version):
    check("named with version and architecture", os.path.basename(path) == "zero-bot_%s_arm64.deb" % version,
          os.path.basename(path))
    members = ar_members(path) if os.path.exists(path) else None
    check("an ar archive", members is not None)
    if not members:
        return
    check("members in dpkg's order", [m[0] for m in members] == ["debian-binary", "control.tar.gz", "data.tar.gz"],
          [m[0] for m in members])
    parts = dict(members)
    check("format 2.0", parts.get("debian-binary") == b"2.0\n")
    control, data = tar_entries(parts["control.tar.gz"]), tar_entries(parts["data.tar.gz"])
    fields = dict(re.findall(r"^([A-Za-z-]+): (.*)$", control["control"].data.decode(), re.M))
    for key, want in (("Package", "zero-bot"), ("Architecture", "arm64"), ("Version", version)):
        check("control: %s" % key, fields.get(key) == want, fields.get(key))
    for name, m in list(control.items()) + list(data.items()):
        check("%s owned by root" % (name or "."), m.uid == 0 and m.gid == 0, (m.uid, m.gid))
    check("control: the control file and the two maintainer scripts",
          sorted(p for p, m in control.items() if m.isfile) == ["control", "postrm", "preinst"],
          sorted(p for p, m in control.items() if m.isfile))
    for script in ("preinst", "postrm"):
        m = control.get(script)
        check("%s is executable" % script, m is not None and m.mode & 0o777 == 0o755, m and oct(m.mode))
        check("%s as in the tree" % script, m is not None and m.data == tree_file("packaging/" + script))
    files = sorted(p for p, m in data.items() if m.isfile)
    check("the app's files and Zero's wake-screen picture", files == [
        "usr/bin/zero_bot", "usr/share/APPLaunch/applications/zero_bot.desktop",
        IMAGE.lstrip("/"), "usr/share/APPLaunch/share/images/zero_bot.png",
        "usr/share/doc/zero-bot/Apache-2.0.txt", "usr/share/doc/zero-bot/README.md",
        "usr/share/doc/zero-bot/THIRD-PARTY-NOTICES.md", "usr/share/doc/zero-bot/copyright"], files)
    picture = data.get(IMAGE.lstrip("/"))
    check("the picture is readable by all", picture is not None and picture.mode & 0o777 == 0o644)
    check("the picture as in the tree", picture is not None and picture.data == tree_file("packaging/lockscreen.png"))


def check_picture():
    """The launcher draws the picture unscaled, centred under its 20 px bar: exactly 320x150 fills it."""
    facts = png_facts(tree_file("packaging/lockscreen.png"))
    check("picture: a PNG whose chunks check out", facts is not None)
    if facts:
        w, h, depth, colour, chunks = facts
        check("picture: 320x150", (w, h) == (320, 150), (w, h))
        check("picture: 8-bit RGB, opaque", (depth, colour) == (8, 2), (depth, colour))
        check("picture: no metadata (IHDR, IDAT, IEND only)", set(chunks) == {"IHDR", "IDAT", "IEND"}, chunks)


# A stand-in for dpkg --compare-versions (the postrm asks it), with Debian's ordering of versions.
STAND_IN_DPKG = r'''
import sys

def order(c):
    return 0 if c.isdigit() else ord(c) if c.isalpha() else -1 if c == "~" else ord(c) + 256

def compare_part(a, b):
    i = j = 0
    while i < len(a) or j < len(b):
        while (i < len(a) and not a[i].isdigit()) or (j < len(b) and not b[j].isdigit()):
            x = order(a[i]) if i < len(a) and not a[i].isdigit() else 0
            y = order(b[j]) if j < len(b) and not b[j].isdigit() else 0
            if x != y:
                return x - y
            i, j = i + (i < len(a) and not a[i].isdigit()), j + (j < len(b) and not b[j].isdigit())
        m = i
        while m < len(a) and a[m].isdigit():
            m += 1
        n = j
        while n < len(b) and b[n].isdigit():
            n += 1
        x, y = int(a[i:m] or 0), int(b[j:n] or 0)
        if x != y:
            return x - y
        i, j = m, n
    return 0

def split(v):
    epoch, _, rest = v.rpartition(":") if ":" in v else ("0", "", v)
    upstream, _, revision = rest.rpartition("-") if "-" in rest else (rest, "", "")
    return int(epoch or 0), upstream, revision

def compare(a, b):
    (ea, ua, ra), (eb, ub, rb) = split(a), split(b)
    return (ea - eb) or compare_part(ua, ub) or compare_part(ra, rb)

if sys.argv[1:2] != ["--compare-versions"] or len(sys.argv) != 5:
    sys.exit(2)
a, op, b = sys.argv[2:]
c = compare(a, b)
sys.exit(0 if {"lt": c < 0, "le": c <= 0, "eq": c == 0, "ne": c != 0, "ge": c >= 0, "gt": c > 0}[op] else 1)
'''


def run_script(script, args, tmp, status=0):
    """Runs a maintainer script as dpkg would (sh SCRIPT ARGS...) with stand-ins for dpkg-divert, which records
    its arguments and exits STATUS, and for dpkg; returns (exit status, [argument lists dpkg-divert got])."""
    bindir, log = os.path.join(tmp, "bin"), os.path.join(tmp, "divert.log")
    os.makedirs(bindir, exist_ok=True)
    with open(os.path.join(bindir, "dpkg-divert"), "w") as f:
        f.write('#!/bin/sh\nfor a in "$@"; do printf "%%s\\t" "$a"; done >> "%s"\necho >> "%s"\nexit %d\n'
                % (log, log, status))
    with open(os.path.join(bindir, "dpkg"), "w") as f:
        f.write("#!%s -I\n%s" % (sys.executable, STAND_IN_DPKG))
    for name in ("dpkg-divert", "dpkg"):
        os.chmod(os.path.join(bindir, name), 0o755)
    if os.path.exists(log):
        os.remove(log)
    code = subprocess.run(["sh", os.path.join(ROOT, "packaging", script)] + list(args),
                          env={"PATH": bindir + ":/usr/bin:/bin"}, capture_output=True).returncode
    calls = []
    if os.path.exists(log):
        with open(log) as f:
            calls = [line.rstrip("\n").rstrip("\t").split("\t") for line in f if line.strip()]
    return code, calls


def check_scripts(tmp):
    for script in ("preinst", "postrm"):
        path = os.path.join(ROOT, "packaging", script)
        check("%s: POSIX sh syntax" % script, subprocess.run(["sh", "-n", path]).returncode == 0)
        text = tree_file("packaging/" + script).decode()
        check("%s: starts #!/bin/sh and stops on errors" % script,
              text.startswith("#!/bin/sh\n") and "\nset -e\n" in text)
        code = [line for line in text.splitlines() if not line.lstrip().startswith("#")]
        touches = [line for line in code if ">" in line or
                   re.search(r"(?:^|[\s;&|(])(rm|mv|cp|ln|install|touch|chmod|chown|tee)\s", line)]
        check("%s: touches no file itself (dpkg-divert does, so DPKG_ROOT is honoured)" % script, not touches, touches)
    # Every way dpkg calls a preinst and a postrm (deb-preinst(5), deb-postrm(5)), with the versions it passes.
    # 0.2.0 is what the owner's device records (later versions were copied in by scripts/deploy.sh), 0.4.8 the
    # last release without the picture.
    expected = [
        ("preinst", ["install"], [ADD]),
        ("preinst", ["install", "0.4.8", "0.4.9"], [ADD]),          # reinstalled after a remove without purge
        ("preinst", ["upgrade", "0.2.0", "0.4.9"], [ADD]),          # the device's first install of the picture
        ("preinst", ["upgrade", "0.4.8", "0.4.9"], [ADD]),
        ("preinst", ["upgrade", "0.4.9", "0.4.10"], [ADD]),         # already diverted: dpkg-divert leaves it
        ("preinst", ["abort-upgrade", "0.4.10"], []),
        ("postrm", ["remove"], [REMOVE]),
        ("postrm", ["purge"], [REMOVE]),
        ("postrm", ["abort-install"], [REMOVE]),
        ("postrm", ["abort-install", "0.4.8"], [REMOVE]),
        ("postrm", ["disappear", "other-package", "1.0"], [REMOVE]),
        ("postrm", ["upgrade", "0.4.10"], []),
        ("postrm", ["failed-upgrade", "0.4.9"], []),
        ("postrm", ["abort-upgrade", "0.2.0"], [REMOVE]),           # back to a version without the picture
        ("postrm", ["abort-upgrade", "0.4.8"], [REMOVE]),
        ("postrm", ["abort-upgrade", "0.4.9"], []),                 # back to one with it: the diversion stays
        ("postrm", ["abort-upgrade", "0.4.10"], []),
    ]
    for script, args, want in expected:
        name = "%s %s" % (script, " ".join(args))
        code, calls = run_script(script, args, tmp)
        check("%s: exits 0" % name, code == 0, code)
        check("%s: dpkg-divert called as expected" % name, calls == want, calls)
    for args in (["install"], ["upgrade", "0.2.0", "0.4.9"]):
        code, _ = run_script("preinst", args, tmp, status=2)
        check("preinst %s: a failed diversion stops the install before the picture is unpacked" % args[0], code != 0, code)
    code, _ = run_script("postrm", ["remove"], tmp, status=2)
    check("postrm remove: a failed removal of the diversion is reported to dpkg", code != 0, code)


def main():
    with open(os.path.join(ROOT, "CMakeLists.txt")) as f:
        version = re.search(r"^project\(zero_bot VERSION ([0-9.]+)", f.read(), re.M).group(1)
    tmp = tempfile.mkdtemp(prefix="zb-packaging-")
    try:
        check_picture()
        check_scripts(tmp)
        check_package(build(os.path.join(tmp, "tree")), version)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("packaging: %d checks, %d failed" % (counted[0], len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
