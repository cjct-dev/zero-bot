#!/usr/bin/env python3
"""Kasa end to end: the driver, and the app, against simulated Kasa devices on 127.0.0.1.

usage: tests/sim/kasa_e2e.py [path/to/kasa_cli [path/to/zero_bot]] [group ...]
       (defaults: build/unit/kasa_cli, which tests/unit/run_unit.sh builds, and build/cli/zero_bot)
groups: discover enumerate apply read moved swap identity hostile app (default: all; "app" is
skipped with a note when the command-line build is missing)

tests/mocks/kasa/mock_kasa.py runs a plug and a second plug with meters of the two hardware
generations, a wall switch, a dimmer, a six-socket strip, a colour bulb, a light strip, a
white bulb, a camera and a plug that moved to the newer login, each on ports the system picks.
kasa_cli calls the driver as the app does; the "app" group runs the command-line build of the
app itself on the stand-in test network (ZB_TEST_NETWORK=1: only 127.x addresses are contacted,
nothing is scanned) with its state in a fresh temporary folder. Every check of what was sent is
made from the device's side: the requests the mock decoded, its connection and probe counters,
and its own state.
"""
import datetime
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

TESTS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # tests/: netguard.py, mocks/
sys.path.insert(0, TESTS)
import netguard   # noqa: E402
netguard.sanitize()   # no key or proxy of the caller's; the app pinned (ZB_EXP_NET) and on the stand-in network

paths = [a for a in sys.argv[1:] if os.sep in a or a.endswith("_cli") or a.endswith("zero_bot")]
GROUPS = [a for a in sys.argv[1:] if a not in paths] or ["discover", "enumerate", "apply", "read", "moved", "swap", "identity",
                                                         "hostile", "app"]
CLI = os.path.abspath(paths[0]) if paths else os.path.join(TESTS, "..", "build", "unit", "kasa_cli")
APP = os.path.abspath(paths[1]) if len(paths) > 1 else os.path.join(TESTS, "..", "build", "cli", "zero_bot")
MOCK = os.path.join(TESTS, "mocks", "kasa", "mock_kasa.py")
NEW_LOGIN = "moved to TP-Link's newer login"

checks = failed = 0
workdir = tempfile.mkdtemp(prefix="kasa-e2e-")


def check(name, ok, detail=""):
    global checks, failed
    checks += 1
    failed += not ok
    print("%s  %s%s" % ("ok  " if ok else "FAIL", name, "" if ok or detail == "" else "  -> %s" % (detail,)), flush=True)
    return ok


class Mock:
    def __init__(self):
        self.proc = subprocess.Popen([sys.executable, "-I", MOCK, "--max-seconds", "900"], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, text=True, env=netguard.mock_env())
        line = self.proc.stdout.readline().split()
        if line[:2] != ["MOCK", "listening"]:
            raise RuntimeError("the mock did not start: %r" % line)
        fields = dict(f.split("=", 1) for f in line[2:])
        self.control = int(fields["control"])
        self.ports = json.loads(fields["devices"])

    def _post(self, path, cfg):
        req = urllib.request.Request("http://127.0.0.1:%d%s" % (self.control, path), data=json.dumps(cfg).encode(), method="POST")
        with urllib.request.urlopen(req, timeout=5) as r:
            return json.load(r)

    def stats(self, device=None):
        with urllib.request.urlopen("http://127.0.0.1:%d/stats" % self.control, timeout=5) as r:
            s = json.load(r)
        return s[device] if device else s

    def reset(self):
        self._post("/reset", {})

    def mode(self, device, mode):
        self._post("/set", {"device": device, "mode": mode})

    def stop(self):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        self.proc.kill()
        self.proc.wait()


DEVICE_TYPES = {"plug": "IOT.SMARTPLUGSWITCH", "plug2": "IOT.SMARTPLUGSWITCH", "switch": "IOT.SMARTPLUGSWITCH",
                "dimmer": "IOT.SMARTPLUGSWITCH", "strip": "IOT.SMARTPLUGSWITCH", "bulb": "IOT.SMARTBULB",
                "lightstrip": "IOT.SMARTBULB", "edison": "IOT.SMARTBULB", "camera": "IOT.SMARTPLUGSWITCH",
                "moved": "IOT.SMARTPLUGSWITCH"}
# The MACs the simulated devices report (mock_kasa.py), as discovery would have saved them.
MACS = {"plug": "50:c7:bf:00:00:01", "plug2": "c0:06:c3:00:00:02", "switch": "50:c7:bf:00:00:03",
        "dimmer": "50:c7:bf:00:00:04", "strip": "c0:06:c3:00:00:05", "bulb": "1c:3b:f3:00:00:06",
        "lightstrip": "e8:48:b8:00:00:07", "edison": "1c:3b:f3:00:00:08", "camera": "50:c7:bf:00:00:09",
        "moved": "cc:32:e5:00:00:0a"}


def state_file(mock, name, scheme="XOR", port=None, fname=None, plug=False, mac=None):
    """A saved device, as discovery and the app would have it, pointing at the mock's port; with
    plug=True it already has the one thing a plug has (as if read before). It is saved with the MAC
    the device reports unless `mac` says otherwise: the driver changes only a device at the address
    that names that MAC ("" for a record saved without one, which is never changed)."""
    if mac is None:
        mac = MACS.get(name, "")
    path = os.path.join(workdir, (fname or name) + ".json")
    if not os.path.exists(path):
        dev = {"id": "tplink:" + name, "kind": "tplink", "name": name, "model": "", "mac": mac, "ip": "127.0.0.1",
               "port": port if port is not None else mock.ports[name]["tcp"],
               "info": {"device_type": DEVICE_TYPES[name], "scheme": scheme}}
        with open(path, "w") as f:
            saved = [{"id": "main", "name": name, "type": "plug", "caps": 1, "ref": {}, "state": {"on": False}}] if plug else []
            json.dump({"device": dev, "things": saved}, f)
    return path


def cli(*args, probe_port=None, deadline=None):
    cmd = [CLI, *[str(a) for a in args]]
    if probe_port is not None:
        cmd += ["--probe-port", str(probe_port)]
    if deadline is not None:
        cmd += ["--deadline", str(deadline)]
    t0 = time.monotonic()
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    ms = (time.monotonic() - t0) * 1000
    out = None
    if p.stdout.strip():
        try:
            out = json.loads(p.stdout.strip().splitlines()[-1])
        except ValueError:
            out = p.stdout
    return p.returncode, out, p.stderr.strip(), ms


def things(out):
    return {t["id"]: t for t in (out or {}).get("things", [])} if isinstance(out, dict) else {}


def requests(mock, name):
    return mock.stats(name)["requests"]


GET_SYSINFO = {"system": {"get_sysinfo": {}}}


# ---------------------------------------------------------------- discovery
def test_discover(mock):
    p = mock.ports
    rc, out, err, _ = cli("-", "discover-udp", p["plug"]["udp"])
    devs = (out or {}).get("devices", [])
    d = devs[0] if devs else {}
    check("discovery: the plug answers the older query", rc == 0 and len(devs) == 1, (rc, out, err))
    check("discovery: kind tplink, id from its MAC, its name and model, port 9999",
          d.get("kind") == "tplink" and d.get("id") == "tplink:50:c7:bf:00:00:01" and d.get("name") == "Kettle plug" and
          d.get("model") == "HS110(UK)" and d.get("port") == 9999 and d.get("found_by") == ["kasa"], d)
    check("discovery: type and scheme kept for the driver",
          d.get("info", {}).get("device_type") == "IOT.SMARTPLUGSWITCH" and d.get("info", {}).get("scheme") == "XOR", d)
    check("discovery: what the plug received was the scrambled get_sysinfo, with no length",
          mock.stats("plug")["udp"] == [GET_SYSINFO] and mock.stats("plug")["bad"] == [], mock.stats("plug"))
    rc, out, err, _ = cli("-", "discover-udp", p["bulb"]["udp"])
    devs = (out or {}).get("devices", [])
    check("discovery: a bulb's MAC comes from mic_mac", len(devs) == 1 and devs[0]["id"] == "tplink:1c:3b:f3:00:00:06" and
          devs[0]["info"]["device_type"] == "IOT.SMARTBULB", out)
    rc, out, err, _ = cli("-", "discover-udp", p["moved"]["udp"])
    check("discovery: a moved plug does not answer the older query", rc == 0 and out.get("devices") == [] and
          mock.stats("moved")["udp"] == [GET_SYSINFO], out)
    rc, out, err, _ = cli("-", "discover-probe", p["moved"]["probe"])
    devs = (out or {}).get("devices", [])
    d = devs[0] if devs else {}
    check("discovery: its 20002 answer files it under the Kasa driver with the newer login's name",
          d.get("kind") == "tplink" and d.get("info", {}).get("scheme") == "KLAP" and d.get("id") == "tplink:cc:32:e5:00:00:0a" and
          d.get("port") == 0, out)
    mock.mode("moved", "probe_tapo")
    rc, out, err, _ = cli("-", "discover-probe", p["moved"]["probe"])
    devs = (out or {}).get("devices", [])
    check("discovery: a Tapo device's 20002 answer stays Tapo's", len(devs) == 1 and devs[0]["kind"] == "tapo", out)
    mock.reset()


# ---------------------------------------------------------------- enumerate
def test_enumerate(mock):
    expect = {
        "plug": ("Kettle plug", {"main": ("plug", ["power", "sensor"], {"on": False})}),
        "plug2": ("Desk fan", {"main": ("plug", ["power", "sensor"], {"on": False})}),
        "switch": ("Porch light", {"main": ("switch", ["power"], {"on": False})}),
        "dimmer": ("Hall dimmer", {"main": ("light", ["power", "brightness"], {"on": False, "brightness": 25})}),
        "bulb": ("Reading lamp", {"main": ("light", ["power", "brightness", "color_temp", "color"], {"on": False})}),
        "lightstrip": ("Shelf strip", {"main": ("light", ["power", "brightness", "color_temp", "color"], {"on": False})}),
        "edison": ("Edison bulb", {"main": ("light", ["power", "brightness"], {"on": False})}),
    }
    for name, (title, want) in expect.items():
        rc, out, err, _ = cli(state_file(mock, name), "enumerate")
        got = {i: (t["type"], t["cap_names"], t["state"]) for i, t in things(out).items()}
        check("enumerate %s: %s" % (name, title), rc == 0 and out.get("name") == title and got == want, (rc, out, err))
        check("enumerate %s: the device received one get_sysinfo on one connection" % name,
              requests(mock, name) == [GET_SYSINFO] and mock.stats(name)["connections"] == 1, mock.stats(name))
    rc, out, err, _ = cli(state_file(mock, "strip"), "enumerate")
    t = things(out)
    base = "C006C30000050000000000000000000000000000"
    check("enumerate strip: six metered sockets by their child ids, the second on",
          rc == 0 and list(t) == [base + "%02d" % i for i in range(6)] and
          all(x["type"] == "plug" and x["cap_names"] == ["power", "sensor"] for x in t.values()) and
          [x["state"]["on"] for x in t.values()] == [False, True, False, False, False, False] and
          t[base + "02"]["name"] == "Socket C", (rc, out, err))
    check("enumerate strip: thing ref names its child", t.get(base + "00", {}).get("ref") == {"child": base + "00"}, t)
    rc, out, err, _ = cli(state_file(mock, "camera"), "enumerate")
    check("enumerate camera: refused as a kind not switched here", rc == 1 and "cannot control this kind of Kasa device" in err,
          (rc, out, err))
    check("enumerate camera: it was only asked what it is", requests(mock, "camera") == [GET_SYSINFO])


# ---------------------------------------------------------------- apply
def test_apply(mock):
    mock.reset()
    for name in ("plug", "switch", "dimmer", "strip", "bulb", "lightstrip", "edison"):
        cli(state_file(mock, name), "enumerate")
    mock.reset()

    def apply(name, thing, change, **kw):
        return cli(state_file(mock, name), "apply", thing, json.dumps(change), **kw)

    rc, out, err, _ = apply("plug", "main", {"on": True})
    st = mock.stats("plug")
    check("plug on: asked who it is, then sent set_relay_state 1, and nothing else", rc == 0 and
          st["requests"] == [GET_SYSINFO, {"system": {"set_relay_state": {"state": 1}}}] and st["state"]["relay_state"] == 1,
          (out, err, st))
    rc, out, err, _ = apply("plug", "main", {"on": False})
    check("plug off", rc == 0 and mock.stats("plug")["state"]["relay_state"] == 0, (out, err))
    n = len(requests(mock, "plug"))
    rc, out, err, _ = apply("plug", "main", {"brightness": 40})
    check("plug: brightness refused, nothing sent", rc == 1 and "dimmed" in err and len(requests(mock, "plug")) == n, err)
    rc, out, err, _ = apply("plug", "main", {"target_c": 21})
    check("plug: a temperature refused, nothing sent", rc == 1 and "only be switched on and off" in err and
          len(requests(mock, "plug")) == n, err)

    rc, out, err, _ = apply("switch", "main", {"on": True})
    check("wall switch on", rc == 0 and mock.stats("switch")["state"]["relay_state"] == 1, (out, err))

    base = "C006C30000050000000000000000000000000000"
    rc, out, err, _ = apply("strip", base + "02", {"on": True})
    st = mock.stats("strip")
    check("strip socket C on: the context names it alone", rc == 0 and st["requests"][-1] ==
          {"context": {"child_ids": [base + "02"]}, "system": {"set_relay_state": {"state": 1}}}, st["requests"])
    check("strip: only socket C changed", list(st["state"]["children"].values()) == [0, 1, 1, 0, 0, 0], st["state"])
    rc, out, err, _ = cli(state_file(mock, "strip"), "apply-many", base + "01," + base + "02", json.dumps({"on": False}))
    st = mock.stats("strip")
    check("strip: two sockets off in one request naming both", rc == 0 and (out or {}).get("handled") is True and
          st["requests"][-1] == {"context": {"child_ids": [base + "01", base + "02"]}, "system": {"set_relay_state": {"state": 0}}} and
          list(st["state"]["children"].values()) == [0, 0, 0, 0, 0, 0], (out, err, st["requests"][-1:], st["state"]))
    rc, out, err, _ = cli(state_file(mock, "plug"), "apply-many", "main,main", json.dumps({"on": True}))
    check("a plain plug has no single request for several things", rc == 0 and out == {"handled": False}, (out, err))

    rc, out, err, _ = apply("dimmer", "main", {"on": True, "brightness": 60})
    st = mock.stats("dimmer")
    check("dimmer to 60: brightness and relay in one request", rc == 0 and st["requests"][-1] ==
          {"smartlife.iot.dimmer": {"set_brightness": {"brightness": 60}}, "system": {"set_relay_state": {"state": 1}}} and
          st["state"]["brightness"] == 60 and st["state"]["relay_state"] == 1, (out, err, st))
    rc, out, err, _ = apply("dimmer", "main", {"brightness": 0})
    st = mock.stats("dimmer")
    check("dimmer to 0 is off (the dimmer itself refuses 0)", rc == 0 and st["requests"][-1] ==
          {"system": {"set_relay_state": {"state": 0}}} and st["state"]["relay_state"] == 0 and st["state"]["brightness"] == 60,
          (out, err, st))

    rc, out, err, _ = apply("bulb", "main", {"on": True, "kelvin": 2000})
    ls = mock.stats("bulb")["state"]["light_state"]
    check("bulb at 2000 K: clamped to the bulb's 2500 K, which it accepts", rc == 0 and ls.get("on_off") == 1 and
          ls.get("color_temp") == 2500, (out, err, ls))
    rc, out, err, _ = apply("bulb", "main", {"on": True, "hs": [240, 100]})
    ls = mock.stats("bulb")["state"]["light_state"]
    check("bulb blue: hue 240, saturation 100, colour mode", rc == 0 and ls.get("hue") == 240 and ls.get("saturation") == 100 and
          ls.get("color_temp") == 0, (out, err, ls))
    rc, out, err, _ = apply("bulb", "main", {"on": False})
    check("bulb off", rc == 0 and mock.stats("bulb")["state"]["light_state"]["on_off"] == 0, err)
    rc, out, err, _ = apply("bulb", "main", {"on": True})
    st = mock.stats("bulb")
    check("bulb on alone: comes back as it was (ignore_default 0)", rc == 0 and st["requests"][-1] ==
          {"smartlife.iot.smartbulb.lightingservice": {"transition_light_state": {"on_off": 1, "ignore_default": 0}}} and
          st["state"]["light_state"].get("hue") == 240, (out, err, st["requests"][-1:], st["state"]))
    rc, out, err, _ = apply("lightstrip", "main", {"on": True, "brightness": 40})
    st = mock.stats("lightstrip")
    check("light strip: its own service and method", rc == 0 and list(st["requests"][-1]) == ["smartlife.iot.lightStrip"] and
          st["state"]["light_state"].get("brightness") == 40, (out, err, st))
    n = len(requests(mock, "edison"))
    rc, out, err, _ = apply("edison", "main", {"on": True, "kelvin": 4000})
    check("white bulb: a shade of white refused, nothing sent", rc == 1 and len(requests(mock, "edison")) == n, err)
    rc, out, err, _ = apply("edison", "main", {"on": True, "brightness": 5})
    check("white bulb: dimmed to 5", rc == 0 and mock.stats("edison")["state"]["light_state"].get("brightness") == 5, err)
    check("no device received anything it could not decode", all(not s["bad"] for s in mock.stats().values()), mock.stats())


# ---------------------------------------------------------------- read
def test_read(mock):
    mock.reset()
    for name in ("plug", "plug2", "strip"):
        cli(state_file(mock, name), "enumerate")
    mock.reset()
    day = datetime.date.today().day
    rc, out, err, _ = cli(state_file(mock, "plug"), "read")
    check("read of everything: only get_sysinfo, no meter", rc == 0 and requests(mock, "plug") == [GET_SYSINFO], (out, err))
    cli(state_file(mock, "plug"), "apply", "main", json.dumps({"on": True}))
    rc, out, err, _ = cli(state_file(mock, "plug"), "read", "main")
    req = requests(mock, "plug")[-1]
    now = datetime.date.today()
    check("read of the plug: sysinfo and its meter in one request, for this month",
          req == {"system": {"get_sysinfo": {}}, "emeter": {"get_realtime": {}, "get_daystat": {"year": now.year, "month": now.month}}},
          req)
    st = things(out).get("main", {}).get("state", {})
    check("older meter: 95.5 W now and today's energy in watt-hours", rc == 0 and st == {"on": True, "power_w": 95.5,
                                                                                         "energy_today_wh": 250 + day}, (out, err))
    cli(state_file(mock, "plug2"), "apply", "main", json.dumps({"on": True}))
    rc, out, err, _ = cli(state_file(mock, "plug2"), "read", "main")
    st = things(out).get("main", {}).get("state", {})
    check("newer meter (milliwatts, watt-hours): the same figures", rc == 0 and st == {"on": True, "power_w": 95.5,
                                                                                        "energy_today_wh": 250 + day}, (out, err))
    base = "C006C30000050000000000000000000000000000"
    mock.reset()
    rc, out, err, _ = cli(state_file(mock, "strip"), "read", base + "00," + base + "01")
    reqs = requests(mock, "strip")
    t = things(out)
    check("strip sockets: sysinfo, then each socket's meter with its own context",
          reqs[0] == GET_SYSINFO and [r.get("context") for r in reqs[1:]] == [{"child_ids": [base + "00"]}, {"child_ids": [base + "01"]}],
          reqs)
    check("strip sockets: the socket that is on draws power, the other none",
          t.get(base + "01", {}).get("state", {}).get("power_w") == 95.5 and t.get(base + "00", {}).get("state", {}).get("power_w") == 0,
          t)
    mock.mode("plug", "energy")
    rc, out, err, _ = cli(state_file(mock, "plug"), "read", "main")
    st = things(out).get("main", {}).get("state", {})
    check("implausible meter readings are left out, the state still read", rc == 0 and st == {"on": False}, (out, err))
    mock.reset()


# ---------------------------------------------------------------- moved to the newer login
def test_moved(mock):
    mock.reset()
    p = mock.ports
    path = state_file(mock, "moved", plug=True)
    rc, out, err, _ = cli(path, "enumerate", probe_port=p["moved"]["probe"])
    check("moved: port 9999 refuses; its 20002 answer says the newer login", rc == 1 and NEW_LOGIN in err and "Tapo" in err,
          (rc, out, err))
    check("moved: one 16-byte query was sent to its discovery port", mock.stats("moved")["probes"] == 1, mock.stats("moved"))
    rc, out, err, _ = cli(path, "apply", "main", json.dumps({"on": True}), probe_port=p["moved"]["probe"])
    check("moved: apply says the same", rc == 1 and NEW_LOGIN in err, err)
    mock.mode("moved", "probe_tapo")
    rc, out, err, _ = cli(path, "enumerate", probe_port=p["moved"]["probe"])
    check("refused, and the 20002 answer is a Tapo device's: just refused", rc == 1 and err == "error: the Kasa device refused the connection",
          err)
    mock.mode("moved", "probe_silent")
    rc, out, err, ms = cli(path, "enumerate", probe_port=p["moved"]["probe"])
    check("refused, no 20002 answer: refused, within a second", rc == 1 and "refused the connection" in err and ms < 1500, (err, ms))
    # Discovery already said so: nothing is sent at all, even to a port that would answer.
    mock.reset()
    klap = state_file(mock, "plug", scheme="KLAP", fname="plug-klap", plug=True)
    for cmd in (["enumerate"], ["read"], ["apply", "main", json.dumps({"on": True})]):
        rc, out, err, _ = cli(klap, *cmd, probe_port=p["moved"]["probe"])
        check("scheme KLAP from discovery: %s refused with the newer login" % cmd[0], rc == 1 and NEW_LOGIN in err, err)
    st = mock.stats("plug")
    check("scheme KLAP: the device saw no connection and no probe", st["connections"] == 0 and st["requests"] == [] and
          mock.stats("moved")["probes"] == 0, st)
    rc, out, err, _ = cli(klap, "claims")
    check("claims: Kasa takes a moved device, Tapo does not", out == {"claims": True, "tapo_claims": False}, out)


# ---------------------------------------------------------------- another device at the saved address
def test_swap(mock):
    """The router gave a saved device's address to another Kasa device (both saved with their MACs,
    as discovery saves them): nothing may be switched on the device now at that address."""
    mock.reset()
    base = "C006C30000050000000000000000000000000000"
    fan = state_file(mock, "plug2", port=mock.ports["strip"]["tcp"], fname="fan-at-strip", plug=True, mac=MACS["plug2"])
    before = mock.stats("strip")["state"]["children"]
    rc, out, err, _ = cli(fan, "apply", "main", json.dumps({"on": True}))
    st = mock.stats("strip")
    check("swap: a plug's 'on' sent to the address the strip now has is refused",
          rc == 1 and "another Kasa device now answers" in err, (rc, out, err))
    check("swap: the strip was only asked what it is, and not one socket changed",
          st["requests"] == [GET_SYSINFO] and st["state"]["children"] == before, (st["requests"], st["state"]))
    rc, out, err, _ = cli(fan, "enumerate")
    with open(fan) as f:
        saved = json.load(f)
    check("swap: enumerate refuses too; the plug's saved name and thing are not replaced by the strip's",
          rc == 1 and "another Kasa device" in err and saved["device"]["name"] == "plug2" and
          [t["id"] for t in saved["things"]] == ["main"], (rc, err, saved))
    # The devices at their own addresses, saved with their MACs: asked who they are, then changed.
    mock.reset()
    rc, out, err, _ = cli(state_file(mock, "switch", fname="switch-mac", plug=True, mac=MACS["switch"]), "apply", "main",
                          json.dumps({"on": True}))
    check("own address: the switch is asked who it is, then switched on", rc == 0 and requests(mock, "switch") ==
          [GET_SYSINFO, {"system": {"set_relay_state": {"state": 1}}}] and mock.stats("switch")["state"]["relay_state"] == 1,
          (rc, err, requests(mock, "switch")))
    strip = state_file(mock, "strip", fname="strip-mac", mac=MACS["strip"].upper())
    rc, out, err, _ = cli(strip, "enumerate")
    rc2, out2, err2, _ = cli(strip, "apply-many", base + "03," + base + "04", json.dumps({"on": True}))
    reqs = requests(mock, "strip")
    check("own address: the strip's sockets, after the same question, in one request",
          rc == 0 and rc2 == 0 and reqs[-2:] == [GET_SYSINFO, {"context": {"child_ids": [base + "03", base + "04"]},
                                                              "system": {"set_relay_state": {"state": 1}}}],
          (err, err2, reqs[-2:]))
    bulb = state_file(mock, "bulb", fname="bulb-mac", mac=MACS["bulb"])
    rc, out, err, _ = cli(bulb, "enumerate")
    rc2, out2, err2, _ = cli(bulb, "apply", "main", json.dumps({"on": True, "brightness": 30}))
    check("own address: a bulb, known by its mic_mac", rc == 0 and rc2 == 0 and
          mock.stats("bulb")["state"]["light_state"].get("brightness") == 30, (err, err2))
    # The address now belongs to another Kasa plug, one that moved to the newer login: its port
    # refuses and its 20002 answer names its own MAC. The saved plug has not moved; it is elsewhere.
    mock.reset()
    kettle = state_file(mock, "plug", port=mock.ports["moved"]["tcp"], fname="kettle-at-moved", plug=True, mac=MACS["plug"])
    rc, out, err, _ = cli(kettle, "apply", "main", json.dumps({"on": True}), probe_port=mock.ports["moved"]["probe"])
    check("swap: another device's 20002 answer is not taken for this plug's (it is looked for again, not called moved)",
          rc == 1 and "another Kasa device now answers" in err and NEW_LOGIN not in err and mock.stats("moved")["probes"] == 1,
          (rc, err, mock.stats("moved")["probes"]))
    rc, out, err, _ = cli(state_file(mock, "moved", fname="moved-mac", plug=True, mac="cc:32:e5:00:00:0a"), "apply", "main",
                          json.dumps({"on": True}), probe_port=mock.ports["moved"]["probe"])
    check("the moved plug's own 20002 answer still says it moved", rc == 1 and NEW_LOGIN in err, (rc, err))
    mock.reset()


# ---------------------------------------------------------------- a device that does not say who it is
def test_identity(mock):
    """A change goes only to a device that names the MAC the saved one was found with. One at the
    saved address that names none (as another device could), or gives it empty, gets no change; nor
    does a device saved without a MAC, which cannot be checked. Checked from the device's side."""
    for mode in ("no_mac", "empty_mac"):
        mock.reset()
        mock.mode("plug", mode)
        rc, out, err, _ = cli(state_file(mock, "plug", plug=True), "apply", "main", json.dumps({"on": True}))
        st = mock.stats("plug")
        check("%s: the change is refused, in words that make the app look for the device" % mode,
              rc == 1 and "does not say which device it is" in err and "answers" in err, (rc, err))
        check("%s: the device was only asked who it is, and is still off" % mode,
              st["requests"] == [GET_SYSINFO] and st["state"]["relay_state"] == 0, (st["requests"], st["state"]))
        rc, out, err, _ = cli(state_file(mock, "plug", plug=True), "enumerate")
        check("%s: nor is it read as the saved plug" % mode, rc == 1 and "does not say which device it is" in err, (rc, err))
    mock.reset()
    # A record saved without a MAC: nothing at all is sent to change it.
    bare = state_file(mock, "switch", fname="switch-bare", plug=True, mac="")
    rc, out, err, _ = cli(bare, "apply", "main", json.dumps({"on": True}))
    st = mock.stats("switch")
    check("saved without a MAC: the change is refused and nothing reaches the device",
          rc == 1 and "no hardware address is saved" in err and st["connections"] == 0 and st["requests"] == [] and
          st["state"]["relay_state"] == 0, (rc, err, st))
    rc, out, err, _ = cli(bare, "enumerate")
    check("saved without a MAC: it can still be read", rc == 0 and things(out).get("main", {}).get("state") == {"on": False}, (rc, err))
    mock.reset()


# ---------------------------------------------------------------- hostile answers
def test_hostile(mock):
    mock.reset()
    cli(state_file(mock, "plug"), "enumerate")
    cases = [
        ("silent", "did not answer", 1500, 2300),
        ("close", "did not answer", 0, 800),
        ("garbage", "could not be understood", 0, 800),
        ("huge", "could not be understood", 0, 800),
        ("oversize", "could not be understood", 0, 1200),
        ("partial", "could not be understood", 0, 800),
        ("crawl", "did not answer", 1500, 2300),
        ("deep", "could not be understood", 0, 1200),
        ("array", "could not be understood", 0, 800),
        ("refuse", "refused the command", 0, 800),
        ("unsupported", "does not support that", 0, 800),
    ]
    for mode, words, at_least, at_most in cases:
        mock.mode("plug", mode)
        before = mock.stats("plug")["state"]["relay_state"]
        rc, out, err, ms = cli(state_file(mock, "plug"), "apply", "main", json.dumps({"on": not before}), deadline=1500)
        check("%s: '%s' within the deadline (%d ms)" % (mode, words, ms), rc == 1 and words in err and at_least <= ms <= at_most,
              (rc, out, err, int(ms)))
    mock.reset()
    mock.mode("plug", "split")
    rc, out, err, ms = cli(state_file(mock, "plug"), "apply", "main", json.dumps({"on": True}))
    check("a reply in one-byte pieces is put together", rc == 0 and mock.stats("plug")["state"]["relay_state"] == 1, (err, ms))
    rc, out, err, ms = cli(state_file(mock, "plug"), "enumerate")
    check("an enumerate in one-byte pieces too", rc == 0 and len(things(out)) == 1, (err, ms))
    mock.reset()
    mock.mode("strip", "names")
    rc, out, err, _ = cli(state_file(mock, "strip"), "enumerate")
    t = things(out)
    base = "C006C30000050000000000000000000000000000"
    names = [x["name"] for x in t.values()]
    check("hostile strip: sockets with bad, long, numeric or repeated ids are left out", rc == 0 and list(t) == [base + "00", base + "05"],
          list(t))
    check("hostile names are cleaned and capped at 40 bytes", all(len(n.encode()) <= 40 and not any(c in n for c in '"{}<>`\\\x00\x07\n')
                                                               for n in names + [out.get("name", "")]) and names[0].startswith("Sock"),
          (names, out.get("name")))
    mock.mode("dimmer", "names")
    rc, out, err, _ = cli(state_file(mock, "dimmer"), "enumerate")
    check("hostile brightness is clamped", things(out).get("main", {}).get("state", {}).get("brightness") == 100, out)
    mock.mode("bulb", "names")
    rc, out, err, _ = cli(state_file(mock, "bulb"), "enumerate")
    check("hostile light values are left out", things(out).get("main", {}).get("state") == {"on": True}, out)
    mock.mode("strip", "many")
    rc, out, err, _ = cli(state_file(mock, "strip"), "enumerate")
    check("a strip claiming 600 sockets gives at most 16", rc == 0 and len(things(out)) == 16, (rc, len(things(out)), err))
    # A strip that lists no sockets is not taken for one plug: "on" for it would go without a
    # socket and switch the whole strip, each socket without its own question.
    mock.reset()
    mock.mode("strip", "no_sockets")
    empty = state_file(mock, "strip", fname="strip-empty")
    rc, out, err, _ = cli(empty, "enumerate")
    with open(empty) as f:
        kept = json.load(f)["things"]
    check("a strip listing no sockets: enumerate refuses and saves no plug for it", rc == 1 and "could not be understood" in err and
          not any(t["id"] == "main" for t in kept), (rc, out, err, kept))
    mock.reset()
    before = mock.stats("strip")["state"]["children"]
    stale = state_file(mock, "strip", fname="strip-stale", plug=True, mac=MACS["strip"])   # "main", as an older read left it
    rc, out, err, _ = cli(stale, "apply", "main", json.dumps({"on": True}))
    st = mock.stats("strip")
    check("a thing saved without a socket is never switched as the whole strip: asked who it is, nothing more",
          rc == 1 and "one at a time" in err and st["requests"] == [GET_SYSINFO] and st["state"]["children"] == before,
          (rc, err, st["requests"], st["state"]))
    mock.reset()


# ---------------------------------------------------------------- the app
def test_app(mock):
    if not os.access(APP, os.X_OK):
        print("note: %s is not built: the app group was skipped (build the cli preset)" % APP, flush=True)
        return
    mock.reset()
    state = os.path.join(workdir, "app")
    env = dict(os.environ, XDG_CONFIG_HOME=state + "/config", XDG_DATA_HOME=state + "/data", XDG_CACHE_HOME=state + "/cache",
               ZB_TEST_NETWORK="1", ZB_NO_AUDIO="1")
    env.pop("ZB_DRY_RUN", None)
    home_file = state + "/data/zero-bot/smarthome.json"

    def run(*args):
        p = subprocess.run([APP, *args], env=env, capture_output=True, text=True, timeout=60)
        return p.stdout

    def tool(args):
        out = run("--tool", "smart_home", json.dumps(args)).strip()
        try:
            return json.loads(out)
        except ValueError:
            return out

    out = run("--home-refresh")
    network = next((l.split(" id ")[1].strip() for l in out.splitlines() if l.startswith("network: Test network, id ")), "")
    check("app: runs on the stand-in test network", network.startswith("net-"), out)

    def saved(name, scheme="XOR", kind="tplink", at=None):
        return {"id": "tplink:" + name, "kind": kind, "name": "Kasa", "model": "", "mac": MACS.get(name, ""),
                "last_ip": "127.0.0.1", "port": mock.ports[at or name]["tcp"],
                "info": {"device_type": DEVICE_TYPES[name], "scheme": scheme},
                "found_by": ["kasa"], "enabled": True, "last_seen": 1, "problem": "", "things": []}
    moved = saved("moved", scheme="KLAP", kind="tapo")   # as an older version filed it
    # The desk fan's plug, read before; the router has since given its address to the kettle plug.
    fan = saved("plug2", at="plug")
    fan["name"] = "Desk fan"
    fan["things"] = [{"id": "main", "name": "Desk fan", "type": "plug", "caps": 1, "enabled": True, "ask_first": False,
                      "ref": {}, "state": {"on": False}}]
    root = {"version": 2, "secrets": {}, "networks": {network: {
        "label": "Test network", "gateway_mac": "02:00:00:00:00:00", "allowed_at": 1, "last_scan": 1,
        "devices": [saved("plug"), saved("switch"), saved("dimmer"), saved("strip"), saved("bulb"), moved, fan]}}}
    os.makedirs(os.path.dirname(home_file), exist_ok=True)
    with open(home_file, "w") as f:
        json.dump(root, f)

    out = run("--home-refresh")
    check("app refresh: five devices ready, the moved one and the desk fan not", "5 ready, 2 not: " + "this Kasa device " +
          NEW_LOGIN in out, out)
    check("app refresh: the moved one says it needs the newer login", any("Moved" in l or NEW_LOGIN in l for l in out.splitlines())
          and NEW_LOGIN in out, out)
    for name in ("Kettle plug", "Porch light", "Hall dimmer", "Socket C", "Reading lamp"):
        check("app refresh lists %s" % name, name in out, out)
    with open(home_file) as f:
        devices = json.load(f)["networks"][network]["devices"]
    kettle = next((t for d in devices for t in d["things"] if t["name"] == "Kettle plug"), {})
    check("app: a kettle plug asks before it is switched on (by its name)", kettle.get("ask_first") is True, kettle)

    r = tool({"action": "on", "target": "Kettle plug"})
    check("app: switching the kettle on asks first, and nothing is sent", isinstance(r, str) and r.startswith("CONFIRM") and
          not any("set_relay_state" in json.dumps(q) for q in mock.stats("plug")["requests"]), r)
    r = tool({"action": "on", "target": "Desk fan"})
    check("app: the desk fan's 'on' does not reach the kettle now at its address (no question was asked for it)",
          not (isinstance(r, dict) and r.get("now") == "on") and "another Kasa device" in json.dumps(r) and
          mock.stats("plug")["state"]["relay_state"] == 0 and
          not any("set_relay_state" in json.dumps(q) for q in mock.stats("plug")["requests"]), r)
    mock.mode("switch", "no_mac")
    r = tool({"action": "on", "target": "Porch light"})
    check("app: the porch light's 'on' does not reach a device at its address that does not say which it is",
          isinstance(r, str) and r.startswith("ERROR") and mock.stats("switch")["state"]["relay_state"] == 0 and
          not any("set_relay_state" in json.dumps(q) for q in mock.stats("switch")["requests"]), r)
    mock.mode("switch", "normal")
    r = tool({"action": "on", "target": "Porch light"})
    check("app: the porch light switch goes on", r == {"now": "on", "target": "Porch light"} and
          mock.stats("switch")["state"]["relay_state"] == 1, r)
    r = tool({"action": "on", "target": "Socket C"})
    base = "C006C30000050000000000000000000000000000"
    check("app: one socket of the strip, by name", isinstance(r, dict) and mock.stats("strip")["state"]["children"][base + "02"] == 1 and
          mock.stats("strip")["requests"][-1].get("context") == {"child_ids": [base + "02"]}, r)
    r = tool({"action": "set", "target": "Hall dimmer", "brightness": 30})
    check("app: the dimmer to 30%", isinstance(r, dict) and mock.stats("dimmer")["state"]["brightness"] == 30 and
          mock.stats("dimmer")["state"]["relay_state"] == 1, r)
    r = tool({"action": "set", "target": "Reading lamp", "color": "blue"})
    ls = mock.stats("bulb")["state"]["light_state"]
    check("app: the bulb blue", isinstance(r, dict) and ls.get("on_off") == 1 and ls.get("color_temp") == 0 and
          200 <= ls.get("hue", 0) <= 260, (r, ls))
    r = tool({"action": "set", "target": "Reading lamp", "color": "warm white"})
    ls = mock.stats("bulb")["state"]["light_state"]
    check("app: the bulb warm white (2700 K)", isinstance(r, dict) and ls.get("color_temp") == 2700, (r, ls))
    r = tool({"action": "status", "target": "Socket C"})
    st = r.get("state", {}) if isinstance(r, dict) else {}
    check("app: status of a socket gives on and its power", st.get("on") is True and st.get("power_w") == 95.5, r)
    r = tool({"action": "off", "target": "all lights"})
    check("app: all lights off reaches the dimmer and the bulb", isinstance(r, dict) and
          mock.stats("dimmer")["state"]["relay_state"] == 0 and mock.stats("bulb")["state"]["light_state"]["on_off"] == 0, r)
    check("app: results carry no addresses or ids", "127.0.0.1" not in json.dumps(r) and base not in json.dumps(r), r)
    check("app: no device received anything it could not decode", all(not s["bad"] for s in mock.stats().values()))
    check("app: the moved device was never sent a request", mock.stats("moved")["requests"] == [] and mock.stats("moved")["probes"] == 0,
          mock.stats("moved"))


def main():
    if not os.access(CLI, os.X_OK):
        sys.exit("no kasa_cli at %s (run tests/unit/run_unit.sh first)" % CLI)
    mock = Mock()
    try:
        for group in GROUPS:
            globals()["test_" + group](mock)
    finally:
        mock.stop()
        shutil.rmtree(workdir, ignore_errors=True)
    print("%d checks, %d failed" % (checks, failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
