#!/usr/bin/env python3
"""Hue end to end: the driver (through build/unit/hue_cli) and the app (build/cli/zero_bot) against
pretend bridges (tests/mocks/hue/mock_hue.py) on 127.0.0.1.

usage: tests/hue_e2e.py [path/to/hue_cli [path/to/zero_bot]]
       (defaults: build/unit/hue_cli, which tests/run_unit.sh builds, and build/cli/zero_bot; the
       app's part is skipped, and says so, when the command-line build is missing)
       HUE_E2E_ONLY=test_groups,test_app runs only the named groups.

Everything runs with ZB_TEST_NETWORK=1 (the stand-in network: only 127.x addresses are ever
contacted, nothing is scanned) and the app with its state in a fresh temporary folder. Every mock
binds port 0, is stopped when its test ends, and exits by itself when this script's end of its
stdin closes. What the driver sent is judged from the bridge's side: the mock logs every request
(method, path, body, and whether it carried the key it issued, never the key itself).

Prints one line per check and a summary; exits non-zero when any check failed.
"""
import importlib.util
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
MOCK = os.path.join(HERE, "mocks", "hue", "mock_hue.py")
CLI = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(HERE, "..", "build", "unit", "hue_cli")
APP = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.join(HERE, "..", "build", "cli", "zero_bot")
sys.dont_write_bytecode = True   # importing the mock for rid() leaves no __pycache__ in the repository
_spec = importlib.util.spec_from_file_location("mock_hue", MOCK)
mock_hue = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mock_hue)
rid = mock_hue.rid
BRIDGE_ID = "00005efffe005301"
DEVICE_ID = "hue:" + BRIDGE_ID
PINS = dict(line.split() for line in open(os.path.join(HERE, "mocks", "hue", "certs", "pins.txt")) if line.strip())
ENV = dict(os.environ, ZB_TEST_NETWORK="1", ZB_NO_AUDIO="1")
ENV.pop("ZB_DRY_RUN", None)

# The words the driver uses (src/smarthome/drivers/hue.cpp); checked exactly, since the README quotes the first.
PROMPT_PAIR = "Press the round button on the Hue Bridge, then confirm here. Zero Bot keeps trying to connect for 30 seconds."
PAIR_TIMEOUT = "The button on the Hue Bridge was not pressed in time. Press it, then try again."
PAIR_WRONG_CERT = ("This device's security certificate does not belong to the Hue Bridge that was found, so Zero Bot did "
                   "not connect to it.")
PAIR_WRONG_BRIDGE = "The device at this address is a different Hue Bridge from the one that was found. Search for devices again."
PAIR_OLD = "This Hue Bridge needs a software update before Zero Bot can use it. Update it with the Hue app, then try again."
PAIR_V1 = ("This is the first, round Hue Bridge, which Zero Bot cannot control. It works with the square Hue Bridge and "
           "the Hue Bridge Pro.")
PAIR_REFUSED = "The Hue Bridge refused to connect Zero Bot. Try again in a few minutes."
PAIR_NOT_HUE = "The device at this address does not answer like a Hue Bridge."
PAIR_NO_ANSWER = "The Hue Bridge did not answer. Check that it is switched on and connected to the router, then try again."
PAIR_CERT_MOVED = ("The Hue Bridge presented a different security certificate partway through pairing, so nothing was "
                   "sent. Try again.")
ERR_REFUSED = "the Hue Bridge no longer accepts Zero Bot's key; pair it again in Settings, Smart home"
ERR_CERT = ("the Hue Bridge's security certificate has changed, so nothing was sent to it; confirm the bridge in "
            "Settings, Smart home")
ERR_MOVED = "the Hue Bridge did not answer at its saved address"
ERR_NO_ANSWER = "the Hue Bridge did not answer"
ERR_TIMEOUT = "the Hue Bridge did not answer in time"
ERR_GARBLED = "the Hue Bridge sent a reply that could not be understood"
ERR_BUSY = "the Hue Bridge is busy; try again in a moment"
ERR_LOST = "the Hue Bridge has lost contact with this light; it may be switched off at the wall"
ERR_LOST_GROUP = "the Hue Bridge has lost contact with some of these lights; they may be switched off at the wall"
ERR_SOFT_OFF = "the light is off, so the change did not take effect; switch it on at the same time"

results = []
outputs = []   # everything the programs printed, to check that no key ever appears
keys = set()   # every application key the mocks issued (read from the state files)


def check(ok, what):
    results.append(bool(ok))
    print("%s %s" % ("ok  " if ok else "FAIL", what), flush=True)


class Mock:
    """One pretend bridge on 127.0.0.1, port 0, in its own temporary folder."""

    def __init__(self, cert=None, files=None):
        self.dir = tempfile.mkdtemp(prefix="hue_e2e_")
        for name, content in (files or {}).items():
            self.set(name, content)
        if cert:
            self.set("cert", cert)
        self.proc = subprocess.Popen([sys.executable, "-I", MOCK, "--state", self.dir, "--max-seconds", "300"],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        line = self.proc.stdout.readline()
        if not line.startswith("PORT "):
            self.stop()
            raise RuntimeError("the mock did not start")
        self.port = int(line.split()[1])

    def set(self, name, content=""):
        with open(os.path.join(self.dir, name), "w") as f:
            f.write(content)

    def clear(self, name):
        try:
            os.remove(os.path.join(self.dir, name))
        except OSError:
            pass

    def log(self):
        try:
            with open(os.path.join(self.dir, "requests.jsonl")) as f:
                return [json.loads(line) for line in f if line.strip()]
        except OSError:
            return []

    def reset_log(self):
        self.clear("requests.jsonl")

    def stop(self):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        shutil.rmtree(self.dir, ignore_errors=True)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()


class Run:
    def __init__(self, p, wall):
        self.rc = p.returncode
        self.out = [json.loads(line) for line in p.stdout.splitlines() if line.strip()]
        lines = p.stderr.splitlines()
        self.errors = [line[len("error: "):] for line in lines if line.startswith("error: ")]
        self.ms = [int(line.split()[1]) for line in lines if line.startswith("elapsed_ms: ")]
        self.wall = wall
        self.err = self.errors[0] if self.errors else ""


def cli(state, *args, timeout=90):
    t0 = time.monotonic()
    p = subprocess.run([CLI, "--state", state] + [str(a) for a in args], capture_output=True, text=True, timeout=timeout, env=ENV)
    outputs.append(p.stdout + p.stderr)
    return Run(p, time.monotonic() - t0)


def init(tmp, port, bridgeid=BRIDGE_ID, modelid="BSB002", name="state.json"):
    st = os.path.join(tmp, name)
    subprocess.run([CLI, "--state", st, "init", "127.0.0.1", str(port), bridgeid, modelid], check=True,
                   capture_output=True, env=ENV)
    return st


def load(st):
    with open(st) as f:
        return json.load(f)


def auth(st):
    a = load(st).get("auth", {})
    if a.get("key"):
        keys.add(a["key"])
    return a


def hue_secrets(st):
    return load(st).get("secrets", {}).get("hue_bridge", {})


def paired(mock, tmp, name="state.json"):
    st = init(tmp, mock.port, name=name)
    mock.set("button")
    r = cli(st, "pair", "--pair-window-ms", "3000", "--pair-interval-ms", "200")
    mock.clear("button")
    if r.rc != 0:
        raise RuntimeError("pairing failed: " + r.err)
    auth(st)
    return st


def enumerated(mock, tmp, name="state.json"):
    st = paired(mock, tmp, name)
    r = cli(st, "enumerate")
    if r.rc != 0:
        raise RuntimeError("enumerate failed: " + r.err)
    mock.reset_log()
    return st, {t["id"]: t for t in r.out[0]["things"]}


def keyed(entries):
    return [e for e in entries if e["key"] != "none"]


def puts(entries):
    return [e for e in entries if e["method"] == "PUT"]


def closed_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


# ================================================================ pairing
def test_pairing(tmp):
    with Mock() as m:
        st = init(tmp, m.port)
        r = cli(st, "step")
        check(r.rc == 0 and r.out[0] == {"kind": "press_button", "prompt": PROMPT_PAIR, "wait_s": 30, "field": ""},
              "an unpaired bridge asks for its button, 30 s")

        r = cli(st, "pair", "--pair-window-ms", "2500", "--pair-interval-ms", "500")
        log = m.log()
        posts = [e for e in log if e["method"] == "POST" and e["path"] == "/api"]
        check(r.rc == 1 and r.err == PAIR_TIMEOUT, "no button within the window: told so")
        check(4 <= len(posts) <= 6, "the key was asked for every 500 ms while waiting (%d times)" % len(posts))
        check(all(p.get("body") == {"devicetype": "zero_bot#cardputer"} for p in posts), "devicetype is zero_bot#cardputer")
        check(not keyed(log), "nothing carried a key")
        check([e["path"] for e in log if e["method"] == "GET"] == ["/api/0/config"], "one identity check first")
        check(r.ms and r.ms[0] < 4000, "the window ends on time (%s ms)" % (r.ms[0] if r.ms else "?"))
        check(auth(st) == {}, "a failed pairing stores nothing")

        m.reset_log()
        threading.Timer(1.2, lambda: m.set("button")).start()
        r = cli(st, "pair", "--pair-window-ms", "5000", "--pair-interval-ms", "300")
        a = auth(st)
        check(r.rc == 0 and r.out == [{"paired": True}], "the button pressed while waiting: paired")
        check(a.get("pin") == PINS["a"], "the stored pin is the certificate's key, as openssl computes it")
        check(len(a.get("key", "")) >= 32 and a.get("bridgeid") == BRIDGE_ID and set(a) == {"key", "pin", "bridgeid"},
              "the key and the bridge id are stored, nothing else")
        check(cli(st, "step").out[0]["kind"] == "none", "a paired bridge needs no step")
        m.clear("button")

    for cert in ("chain", "san", "rsa"):
        with Mock(cert=cert) as m:
            sub = tempfile.mkdtemp(dir=tmp)
            st = init(sub, m.port)
            m.set("button")
            r = cli(st, "pair", "--pair-window-ms", "2000")
            check(r.rc == 0 and auth(st).get("pin") == PINS[cert], "pairs with the %s certificate and pins its key" % cert)
            r = cli(st, "enumerate")
            check(r.rc == 0 and len(r.out[0]["things"]) == 12, "and then reads it through the pin (%s)" % cert)


def test_pairing_refusals(tmp):
    def attempt(mock_kw, init_kw=None, window="1500"):
        with Mock(**mock_kw) as m:
            sub = tempfile.mkdtemp(dir=tmp)
            st = init(sub, m.port, **(init_kw or {}))
            m.set("button")
            r = cli(st, "pair", "--pair-window-ms", window, "--pair-interval-ms", "200")
            return r, m.log(), auth(st)

    r, log, a = attempt({"cert": "other"})
    check(r.rc == 1 and r.err == PAIR_WRONG_CERT, "a certificate naming another bridge is refused")
    check(not [e for e in log if e["method"] == "POST"] and a == {}, "and no key is asked for")

    r, log, a = attempt({}, {"bridgeid": "00005efffe0053ff"})
    check(r.rc == 1 and r.err == PAIR_WRONG_BRIDGE and not [e for e in log if e["method"] == "POST"],
          "a bridge whose id differs from discovery's is refused")

    r, log, a = attempt({"files": {"config.json": '{"swversion": "1941132080"}'}})
    check(r.rc == 1 and r.err == PAIR_OLD, "firmware older than 1948086000 is told to update")

    r, log, a = attempt({"files": {"config.json": '{"modelid": "BSB001", "swversion": "01041302"}'}})
    check(r.rc == 1 and r.err == PAIR_V1, "the round first bridge is named as unsupported")
    r = subprocess.run([CLI, "claims", "hue_bridge", '{"modelid":"BSB001"}'], capture_output=True, text=True)
    check(json.loads(r.stdout) == {"claims": False}, "and not claimed when discovery reports BSB001")
    r = subprocess.run([CLI, "claims", "hue_bridge", '{"modelid":"BSB002","bridgeid":"' + BRIDGE_ID + '"}'],
                       capture_output=True, text=True)
    check(json.loads(r.stdout) == {"claims": True}, "a BSB002 is claimed")

    r, log, a = attempt({"files": {"fault": "link-refused /api"}})
    posts = [e for e in log if e["method"] == "POST"]
    check(r.rc == 1 and r.err == PAIR_REFUSED and len(posts) == 1, "a refusal other than 'button not pressed' is not retried")

    r, log, a = attempt({"files": {"fault": "not-hue"}})
    check(r.rc == 1 and r.err == PAIR_NOT_HUE, "something that is not a bridge is refused")

    r, log, a = attempt({"files": {"swap-after-config": "b"}})
    check(r.rc == 1 and r.err == PAIR_CERT_MOVED, "a certificate that changes between the check and the request stops pairing")
    check(not [e for e in log if e["method"] == "POST"] and a == {}, "and no request reached the other certificate's holder")

    r, log, a = attempt({"files": {"fault": "silent"}})
    check(r.rc == 1 and r.err == PAIR_NO_ANSWER and r.ms[0] < 7000,
          "a device that accepts and never speaks TLS: no answer within the limit (%d ms)" % r.ms[0])

    sub = tempfile.mkdtemp(dir=tmp)
    st = init(sub, closed_port())
    r = cli(st, "pair", "--pair-window-ms", "1000")
    check(r.rc == 1 and r.err == PAIR_NO_ANSWER and r.ms[0] < 1500, "nothing listening: no answer, at once")


# ================================================================ things
def test_things(tmp):
    with Mock() as m:
        st = paired(m, tmp)
        m.reset_log()
        r = cli(st, "enumerate")
        check(r.rc == 0 and r.out[0]["name"] == "Hue Bridge" and r.out[0]["model"] == "BSB002", "enumerate names the bridge")
        things = {t["id"]: t for t in r.out[0]["things"]}
        log = m.log()
        check([(e["method"], e["path"], e["key"]) for e in log] == [("GET", "/clip/v2/resource", "ok")],
              "enumerate makes one keyed GET of every resource")
        check(len(things) == 12, "12 things: 8 lights and plugs, 3 scenes, 1 sensor")
        expect = {
            "l-sofa": ("light", "Sofa lamp", "Living room", 15),
            "l-ceiling": ("light", "Ceiling", "Living room", 7),
            "l-plug": ("plug", "Fan heater", "Living room", 1),
            "l-strip": ("light", "Kitchen strip", "Kitchen", 11),
            "l-spots": ("light", "Kitchen spots", "Kitchen", 7),
            "l-hall": ("light", "Hall light", "Hallway", 3),
            "l-lantern": ("light", "Garden lantern", "Outside", 15),
            "sc-relax-living": ("scene", "Relax", "Living room", 16),
            "sc-relax-kitchen": ("scene", "Relax", "Kitchen", 16),
            "sc-bright-down": ("scene", "Bright", "Downstairs", 16),
            "d-sensor": ("sensor", "Hall sensor", "Hallway", 8192),
        }
        for name, (typ, label, room, caps) in expect.items():
            t = things.get(rid(name), {})
            check((t.get("type"), t.get("name"), t.get("room"), t.get("caps")) == (typ, label, room, caps),
                  "%s: %s '%s' in %s, caps %d" % (name, typ, label, room, caps))
        hostile = things.get(rid("l-hostile"), {})
        name = hostile.get("name", "")
        check(name == "Evil lamp " + "x" * 30 and len(name.encode()) <= 40,
              "a hostile name is cleaned: no control or bidi characters, 40 bytes")
        for gone in ("l-flood", "sc-alarm", "d-camera", "d-contact", "d-chime", "d-dimmer", "d-future", "bridge"):
            check(rid(gone) not in things, "%s is not exposed" % gone)
        sofa = things[rid("l-sofa")]
        check(sofa["state"].get("on") is True and sofa["state"].get("brightness") == 60 and
              abs(sofa["state"].get("hue", -99) - 39) <= 1 and abs(sofa["state"].get("saturation", -99) - 53) <= 1 and
              "kelvin" not in sofa["state"], "a colour light's state: on, 60 percent, hue 39 and saturation 53 (%s)" % sofa["state"])
        check(things[rid("l-ceiling")]["state"] == {"on": False, "brightness": 100, "kelvin": 2732}, "a white light's state")
        check(things[rid("d-sensor")]["state"] == {"current_c": 21.4, "motion": True}, "the sensor's temperature and motion")
        check([(g["rtype"], g["n"]) for g in sofa["ref"]["groups"]] == [("room", 3), ("zone", 4), ("bridge_home", 9)],
              "a light knows the groups that reach it and their sizes")
        check(all(len(t["name"].encode()) <= 40 and not any(ord(c) < 32 for c in t["name"]) for t in things.values()),
              "every name is clean")

        m.reset_log()
        r = cli(st, "read")
        log = m.log()
        check(r.rc == 0 and len(r.out[0]["things"]) == 12, "read with no ids refreshes every thing")
        check([e["path"] for e in log] == ["/clip/v2/resource/light", "/clip/v2/resource/temperature", "/clip/v2/resource/motion"],
              "with three GETs")
        m.reset_log()
        r = cli(st, "read", rid("d-sensor"))
        check(r.rc == 0 and r.out[0]["things"] == [{"id": rid("d-sensor"), "state": {"current_c": 21.4, "motion": True}}]
              and [e["path"] for e in m.log()] == ["/clip/v2/resource/temperature", "/clip/v2/resource/motion"],
              "reading the sensor asks only for sensors")
        m.reset_log()
        r = cli(st, "read", rid("sc-bright-down"))
        check(r.rc == 0 and m.log() == [], "reading a scene asks nothing (a scene has no state)")

        # The same bridge, its data in wrong types or bytes that are not UTF-8.
        m.set("fault", "wrong-types")
        r = cli(st, "enumerate")
        check(r.rc == 0, "resources with wrong JSON types do not break enumerate")
        check(r.rc == 0 and all(t["type"] != "light" for t in r.out[0]["things"]),
              "and lights whose fields have the wrong types are left out")
        m.set("fault", "nonutf8")
        r = cli(st, "enumerate")
        names = [t["name"] for t in r.out[0]["things"]] if r.rc == 0 else []
        check(r.rc == 0 and "Sofa ?? lamp" in names and "Kit??chen" in [t["room"] for t in r.out[0]["things"]],
              "names with bytes that are not UTF-8 are repaired, not rejected")
        m.clear("fault")


# ================================================================ changes
def test_apply(tmp):
    with Mock() as m:
        st, things = enumerated(m, tmp)
        sofa, ceiling, plug, hall, lantern = (rid(x) for x in ("l-sofa", "l-ceiling", "l-plug", "l-hall", "l-lantern"))

        def last_put():
            p = puts(m.log())
            return p[-1] if p else None

        r = cli(st, "apply", sofa, '{"on":true,"brightness":40}')
        p = last_put()
        check(r.rc == 0 and p and p["path"] == "/clip/v2/resource/light/" + sofa and p["key"] == "ok" and
              p["body"] == {"on": {"on": True}, "dimming": {"brightness": 40}}, "on at 40 percent is one PUT")
        check(r.ms[0] < 2500, "within the deadline (%d ms)" % r.ms[0])
        r = cli(st, "apply", ceiling, '{"on":true,"kelvin":2700}')
        check(r.rc == 0 and last_put()["body"] == {"on": {"on": True}, "color_temperature": {"mirek": 370}}, "2700 K is 370 mirek")
        r = cli(st, "apply", ceiling, '{"on":true,"kelvin":1500}')
        check(r.rc == 0 and last_put()["body"]["color_temperature"] == {"mirek": 454}, "1500 K is kept within the light's 454")
        r = cli(st, "apply", lantern, '{"on":true,"kelvin":1500}')
        check(r.rc == 0 and last_put()["body"]["color_temperature"] == {"mirek": 667}, "a 50..1000 light takes 667 mirek")
        r = cli(st, "apply", sofa, '{"on":true,"hs":[240,100]}')
        check(r.rc == 0 and last_put()["body"]["color"] == {"xy": {"x": 0.1355, "y": 0.0399}}, "blue is sent as its xy")
        r = cli(st, "read", sofa)
        check(r.rc == 0 and r.out[0]["things"][0]["state"]["hue"] == 240 and r.out[0]["things"][0]["state"]["saturation"] == 100,
              "and reads back as hue 240")
        r = cli(st, "apply", sofa, '{"brightness":0}')
        check(r.rc == 0 and last_put()["body"] == {"on": {"on": False}}, "brightness 0 switches off")

        m.reset_log()
        for thing, change, err, what in [
            (hall, '{"on":true,"hs":[0,100]}', "its colour cannot be changed", "a white light refuses a colour"),
            (plug, '{"brightness":50}', "this plug can only be switched on and off", "a plug refuses brightness"),
            (rid("sc-relax-living"), '{"on":false}', "a scene can only be started", "a scene cannot be switched off"),
            (rid("d-sensor"), '{"on":true}', "this is a sensor; it can only be read", "a sensor is never commanded"),
            (sofa, '{"volume":5}', "a Hue light can only be switched, dimmed or coloured", "a light has no volume"),
            (sofa, '{"key":"ok"}', "a Hue light can only be switched, dimmed or coloured", "a light takes no remote keys"),
        ]:
            r = cli(st, "apply", thing, change)
            check(r.rc == 1 and r.err == err, what)
        check(m.log() == [], "and none of those sent anything")

        r = cli(st, "apply", plug, '{"on":true}')
        check(r.rc == 0 and last_put()["body"] == {"on": {"on": True}}, "a plug switches on")
        r = cli(st, "apply", rid("sc-relax-living"), '{"on":true}')
        p = last_put()
        check(r.rc == 0 and p["path"] == "/clip/v2/resource/scene/" + rid("sc-relax-living") and
              p["body"] == {"recall": {"action": "active"}}, "a scene is recalled")
        r = cli(st, "apply", hall, '{"brightness":30}')
        check(r.rc == 1 and r.err == ERR_SOFT_OFF, "dimming a light that is off without switching it on is reported (207)")

        m.set("unreachable")
        r = cli(st, "apply", sofa, '{"on":true}')
        check(r.rc == 1 and r.err == ERR_LOST, "a light the bridge cannot reach is reported (207)")
        m.clear("unreachable")

        # Spacing: three commands in one process go at least 100 ms apart.
        m.reset_log()
        r = cli(st, "apply", sofa, '{"on":true}', "--repeat", "3")
        t = [e["t"] for e in puts(m.log())]
        gaps = [b - a for a, b in zip(t, t[1:])]
        check(r.rc == 0 and len(t) == 3 and all(g >= 0.09 for g in gaps),
              "successive commands are spaced 100 ms apart (%s)" % ", ".join("%.0f ms" % (g * 1000) for g in gaps))

        # Busy bridge: one 429 is retried once; a bridge that stays busy is reported, within the deadline.
        m.reset_log()
        m.set("busy", "1")
        r = cli(st, "apply", sofa, '{"on":false}')
        check(r.rc == 0 and len(puts(m.log())) == 2, "a 429 is retried once")
        m.reset_log()
        m.set("busy", "10")
        r = cli(st, "apply", sofa, '{"on":false}')
        check(r.rc == 1 and r.err == ERR_BUSY and len(puts(m.log())) == 2 and r.ms[0] < 2600,
              "a bridge that stays busy: two attempts, then told, in %d ms" % r.ms[0])
        m.clear("busy")

        r = cli(st, "apply", rid("nothing-here"), '{"on":true}')
        check(r.rc == 2, "the CLI refuses an id it does not have")


def test_groups(tmp):
    with Mock() as m:
        st, things = enumerated(m, tmp)
        strip, spots, sofa, ceiling, plug = (rid(x) for x in ("l-strip", "l-spots", "l-sofa", "l-ceiling", "l-plug"))

        r = cli(st, "apply-many", '{"on":false}', strip, spots)
        log = m.log()
        check(r.rc == 0 and r.out[0] == {"ok": True, "handled": True}, "the Kitchen's two lights: one group call")
        check([(e["method"], e["path"]) for e in log] ==
              [("GET", "/clip/v2/resource/room/" + rid("room-kitchen")),
               ("PUT", "/clip/v2/resource/grouped_light/" + rid("gl-room-kitchen"))] and log[1]["body"] == {"on": {"on": False}},
              "it checks the room's members, then sends one grouped_light PUT")
        r = cli(st, "read", strip, spots)
        check(r.rc == 0 and all(t["state"]["on"] is False for t in r.out[0]["things"]), "both lights went off")

        m.reset_log()
        r = cli(st, "apply-many", '{"on":true}', sofa, ceiling)
        check(r.rc == 0 and r.out[0] == {"ok": False, "handled": False} and m.log() == [],
              "the Living room's lights without its plug: not a group call, and nothing sent")
        r = cli(st, "apply-many", '{"on":true}', sofa, ceiling, plug)
        check(r.rc == 0 and r.out[0]["handled"] and puts(m.log())[-1]["path"] == "/clip/v2/resource/grouped_light/" +
              rid("gl-room-living"), "with the plug the whole room is one call")
        m.reset_log()
        r = cli(st, "apply-many", '{"on":true,"kelvin":2700}', strip, spots)
        check(r.rc == 0 and not r.out[0]["handled"] and m.log() == [], "a white the strip cannot show is not sent to the room")
        r = cli(st, "apply-many", '{"on":true,"brightness":30}', sofa, ceiling, strip, spots)
        p = puts(m.log())
        check(r.rc == 0 and r.out[0]["handled"] and p and p[-1]["path"] == "/clip/v2/resource/grouped_light/" + rid("gl-zone-down")
              and p[-1]["body"] == {"on": {"on": True}, "dimming": {"brightness": 30}}, "a zone's lights are one call")
        m.reset_log()
        lights = [t for t, v in things.items() if v["type"] in ("light", "plug")]
        r = cli(st, "apply-many", '{"on":false}', *lights)
        check(r.rc == 0 and not r.out[0]["handled"] and m.log() == [],
              "every exposed light is not the whole home while the camera's floodlight is on the bridge")

        m.set("unreachable")
        r = cli(st, "apply-many", '{"on":true}', strip, spots)
        check(r.rc == 1 and r.err == ERR_LOST_GROUP, "a group with lights the bridge cannot reach is reported")
        m.clear("unreachable")

        # Membership changed since the list was read: the Hall light moved to the Kitchen.
        m.set("extra-room")
        m.reset_log()
        r = cli(st, "apply-many", '{"on":false}', strip, spots)
        log = m.log()
        check(r.rc == 0 and not r.out[0]["handled"] and [e["method"] for e in log] == ["GET"],
              "a room that gained a light since enumerate is not switched as a group")

    with Mock(files={"no-camera": ""}) as m:
        st, things = enumerated(m, tmp, "nocam.json")
        lights = [t for t, v in things.items() if v["type"] in ("light", "plug")]
        r = cli(st, "apply-many", '{"on":false}', *lights)
        log = m.log()
        check(r.rc == 0 and r.out[0]["handled"] and [(e["method"], e["path"]) for e in log] ==
              [("GET", "/clip/v2/resource/light"), ("PUT", "/clip/v2/resource/grouped_light/" + rid("gl-home"))],
              "every light of the bridge is one bridge_home call")


# ================================================================ failures
def test_refused_key(tmp):
    with Mock() as m:
        st, things = enumerated(m, tmp)
        sofa = rid("l-sofa")
        m.set("revoke")
        m.reset_log()
        r = cli(st, "apply", sofa, '{"on":true}', "--repeat", "3")
        check(r.rc == 1 and r.errors == [ERR_REFUSED] * 3, "a revoked key is reported")
        check([e["key"] for e in m.log()] == ["wrong"], "and sent once only: later commands are refused without contacting the bridge")
        saved = hue_secrets(st).get("refused", {})
        check(list(saved) == [DEVICE_ID] and len(saved[DEVICE_ID]) == 64 and saved[DEVICE_ID] != auth(st)["key"],
              "the refusal is kept in the family's secrets, as a hash of the key")
        m.reset_log()
        r = cli(st, "apply", sofa, '{"on":true}')
        r2 = cli(st, "enumerate")
        r3 = cli(st, "read")
        check(r.err == ERR_REFUSED and r2.err == ERR_REFUSED and r3.err == ERR_REFUSED and m.log() == [],
              "a new run does not send the refused key either (apply, enumerate, read)")
        r = cli(st, "step")
        check(r.out[0]["kind"] == "press_button" and "no longer accepts" in r.out[0]["prompt"],
              "and asks for the button again")
        m.clear("revoke")
        m.reset_log()
        r = cli(st, "apply", sofa, '{"on":true}')
        check(r.rc == 1 and r.err == ERR_REFUSED and m.log() == [], "nothing is sent until it is paired again, even once the bridge would take it")
        m.set("button")
        r = cli(st, "pair", "--pair-window-ms", "2000")
        m.clear("button")
        check(r.rc == 0 and "refused" not in hue_secrets(st), "pairing again clears the refusal")
        auth(st)
        r = cli(st, "apply", sofa, '{"on":true}')
        check(r.rc == 0, "and commands work again")


def test_certificate_change(tmp):
    with Mock() as m:
        st, things = enumerated(m, tmp)
        sofa = rid("l-sofa")
        m.set("cert", "b")
        m.reset_log()
        r = cli(st, "apply", sofa, '{"on":true}', "--repeat", "3")
        log = m.log()
        check(r.rc == 1 and r.errors == [ERR_CERT] * 3, "a changed certificate is reported, and stops the driver")
        check(not keyed(log), "the key was never sent to the new certificate")
        check([e["path"] for e in log] == ["/api/0/config"], "one keyless identity check, then nothing more")
        check(hue_secrets(st).get("cert_changed") == {DEVICE_ID: PINS["a"]}, "the change is kept in the family's secrets")
        m.reset_log()
        r = cli(st, "enumerate")
        check(r.rc == 1 and r.err == ERR_CERT and m.log() == [], "a new run sends nothing at all while it stands")
        r = cli(st, "step")
        check(r.out[0]["kind"] == "press_button" and "certificate has changed" in r.out[0]["prompt"],
              "which asks for the button to confirm")
        m.set("button")
        r = cli(st, "pair", "--pair-window-ms", "2000")
        m.clear("button")
        a = auth(st)
        check(r.rc == 0 and a.get("pin") == PINS["b"] and "cert_changed" not in hue_secrets(st), "confirming pins the new certificate")
        r = cli(st, "apply", sofa, '{"on":false}')
        check(r.rc == 0, "and commands work again")

        m.set("cert", "other")
        m.reset_log()
        r = cli(st, "apply", sofa, '{"on":true}')
        check(r.rc == 1 and r.err == ERR_MOVED and not keyed(m.log()) and "cert_changed" not in hue_secrets(st),
              "another device at the address reads as 'not at its saved address' (the cue to look elsewhere), nothing kept")


def test_unreachable(tmp):
    sub = tempfile.mkdtemp(dir=tmp)
    with Mock() as m:
        st, things = enumerated(m, sub)
    sofa = rid("l-sofa")
    # The mock is gone: nothing listens on that port now.
    r = cli(st, "apply", sofa, '{"on":true}')
    check(r.rc == 1 and r.err == ERR_NO_ANSWER and r.ms[0] < 1000, "nothing listening: 'did not answer' at once (%d ms)" % r.ms[0])

    for fault, label in (("silent", "accepts and never speaks TLS"), ("silent-http", "speaks TLS and never answers")):
        with Mock() as m:
            st, things = enumerated(m, tempfile.mkdtemp(dir=tmp))
            m.set("fault", fault)
            r = cli(st, "apply", sofa, '{"on":true}', "--deadline-ms", "2500")
            check(r.rc == 1 and r.err in (ERR_TIMEOUT, ERR_NO_ANSWER) and r.ms[0] <= 2900,
                  "a bridge that %s: apply gives up within 2.5 s (%d ms)" % (label, r.ms[0]))
            r = cli(st, "enumerate")
            check(r.rc == 1 and r.err in (ERR_TIMEOUT, ERR_NO_ANSWER) and r.ms[0] <= 4400,
                  "and enumerate within 4 s (%d ms)" % r.ms[0])
            r = cli(st, "read")
            check(r.rc == 1 and r.ms[0] <= 4400, "and read within 4 s (%d ms)" % r.ms[0])


def test_malformed(tmp):
    with Mock() as m:
        st, things = enumerated(m, tmp)
        sofa = rid("l-sofa")
        for fault, what in (("garbage", "a body that is not JSON"), ("deep", "JSON nested 100000 deep"),
                            ("huge", "a 10 MB body"), ("huge-length", "a 100 GB Content-Length"),
                            ("truncated", "a body cut short"), ("data-object", "'data' that is not an array")):
            m.set("fault", fault + " /clip/v2/resource")
            r = cli(st, "enumerate")
            check(r.rc == 1 and r.err == ERR_GARBLED and r.ms[0] < 4200, "enumerate given %s: told so (%d ms)" % (what, r.ms[0]))
            r = cli(st, "read", sofa)
            check(r.rc == 1 and r.err == ERR_GARBLED, "read given %s: told so" % what)
            r = cli(st, "apply", sofa, '{"on":true}')
            check(r.rc == 1 and r.err == ERR_GARBLED, "apply given %s: told so" % what)
        # A well-formed reply that is simply too big (over the driver's 4 MB limit) is refused too.
        m.set("fault", "huge-valid /clip/v2/resource")
        r = cli(st, "enumerate")
        r2 = cli(st, "read", sofa)
        check(r.rc == 1 and r.err == ERR_GARBLED and r2.rc == 1 and r2.err == ERR_GARBLED,
              "a well-formed 5 MB reply is refused, not parsed (enumerate and read)")
        m.set("fault", "status-500 /clip/v2/resource/light")
        r = cli(st, "apply", sofa, '{"on":true}')
        check(r.rc == 1 and r.err == ERR_BUSY, "a 500 reads as busy")
        m.clear("fault")
        r = cli(st, "apply", sofa, '{"on":true}')
        check(r.rc == 0, "and once the bridge behaves, it works")


def test_unpair(tmp):
    with Mock() as m:
        st, things = enumerated(m, tmp)
        m.reset_log()
        r = cli(st, "unpair")
        check(r.rc == 0 and auth(st) == {} and m.log() == [], "unpair forgets the key and the pin, sending nothing")
        check(cli(st, "step").out[0]["kind"] == "press_button", "and the bridge asks for its button again")
        r = cli(st, "apply", rid("l-sofa"), '{"on":true}')
        check(r.rc == 1 and r.err == "the Hue Bridge is not paired yet; pair it in Settings, Smart home" and m.log() == [],
              "an unpaired bridge is not contacted")


# ================================================================ the app, on the stand-in network
class App:
    def __init__(self, tmp):
        self.state = tempfile.mkdtemp(dir=tmp, prefix="app-")
        self.env = dict(ENV, XDG_CONFIG_HOME=self.state + "/config", XDG_DATA_HOME=self.state + "/data",
                        XDG_CACHE_HOME=self.state + "/cache", HOME=self.state)
        self.file = self.state + "/data/zero-bot/smarthome.json"

    def run(self, *args):
        p = subprocess.run([APP, *args], env=self.env, capture_output=True, text=True, timeout=60)
        outputs.append(p.stdout + p.stderr)
        return p.stdout

    def tool(self, args):
        out = self.run("--tool", "smart_home", json.dumps(args)).strip()
        try:
            return json.loads(out)
        except ValueError:
            return out

    def load(self):
        with open(self.file) as f:
            return json.load(f)

    def save(self, root):
        os.makedirs(os.path.dirname(self.file), exist_ok=True)
        with open(self.file, "w") as f:
            json.dump(root, f)
        os.chmod(self.file, 0o600)


def test_app(tmp):
    if not os.access(APP, os.X_OK):
        print("info  %s is not built (repobuild cli): the app's part was skipped" % APP, flush=True)
        return
    with Mock() as m:
        st = paired(m, tmp)
        app = App(tmp)
        out = app.run("--home-refresh")
        network = next((l.split(" id ")[1].strip() for l in out.splitlines() if l.startswith("network: Test network, id ")), "")
        check(network.startswith("net-"), "the app runs on the stand-in test network")
        device = {"id": DEVICE_ID, "kind": "hue_bridge", "name": "Hue Bridge", "model": "BSB002", "mac": "", "last_ip": "127.0.0.1",
                  "port": m.port, "info": {"bridgeid": BRIDGE_ID, "modelid": "BSB002"}, "found_by": ["mdns"], "enabled": True,
                  "needs_setup": False, "last_seen": 1, "problem": "", "auth": auth(st), "things": []}
        app.save({"version": 2, "secrets": {},
                  "networks": {network: {"label": "Test network", "gateway_mac": "02:00:00:00:00:00", "allowed_at": 1,
                                         "last_scan": 1, "devices": [device]}}})
        m.reset_log()
        out = app.run("--home-refresh")
        check("1 device ready." in out, "refresh reads the bridge")
        check([(e["method"], e["path"], e["key"]) for e in m.log()] == [("GET", "/clip/v2/resource", "ok")],
              "with one keyed GET, seen by the bridge")
        saved = app.load()["networks"][network]["devices"][0]
        names = {t["name"]: t for t in saved["things"]}
        check(len(saved["things"]) == 12 and {"Sofa lamp", "Fan heater", "Bright", "Hall sensor"} <= set(names),
              "the app saved the bridge's 12 things")
        check(names.get("Fan heater", {}).get("ask_first") is True, "the app asks before switching on a plug called a heater")

        m.reset_log()
        r = app.tool({"action": "on", "target": "Sofa lamp"})
        log = m.log()
        check(isinstance(r, dict) and r.get("now") == "on" and [(e["method"], e["path"], e["key"], e.get("body")) for e in log] ==
              [("PUT", "/clip/v2/resource/light/" + rid("l-sofa"), "ok", {"on": {"on": True}})],
              "on by name: one PUT to that light, with the key (%s)" % (r,))
        m.reset_log()
        r = app.tool({"action": "off", "target": "Kitchen"})
        log = m.log()
        check(isinstance(r, dict) and [(e["method"], e["path"]) for e in log] ==
              [("GET", "/clip/v2/resource/room/" + rid("room-kitchen")),
               ("PUT", "/clip/v2/resource/grouped_light/" + rid("gl-room-kitchen"))] and log[-1]["body"] == {"on": {"on": False}},
              "a room by name: one grouped_light call after checking its members (%s)" % (r,))
        m.reset_log()
        r = app.tool({"action": "off", "target": "Hallway"})
        log = m.log()
        check(isinstance(r, dict) and sorted(e["path"] for e in puts(log)) ==
              sorted("/clip/v2/resource/light/" + rid(x) for x in ("l-hall", "l-hostile")) and
              not any("grouped_light" in e["path"] or rid("l-flood") in e["path"] for e in log),
              "the Hallway, whose bridge group also holds the camera's floodlight, goes light by light (%s)" % (r,))
        m.reset_log()
        r = app.tool({"action": "set", "target": "Kitchen spots", "color": "warm white"})
        log = puts(m.log())
        check(isinstance(r, dict) and len(log) == 1 and log[0]["body"] == {"on": {"on": True}, "color_temperature": {"mirek": 370}},
              "warm white is 370 mirek, with on (%s)" % (r,))
        m.reset_log()
        r = app.tool({"action": "scene", "target": "Bright"})
        log = puts(m.log())
        check(isinstance(r, dict) and len(log) == 1 and log[0]["path"] == "/clip/v2/resource/scene/" + rid("sc-bright-down") and
              log[0]["body"] == {"recall": {"action": "active"}}, "a scene by name is recalled (%s)" % (r,))
        m.reset_log()
        r = app.tool({"action": "on", "target": "Fan heater"})
        check(isinstance(r, str) and r.startswith("CONFIRM:") and m.log() == [], "the heater plug is asked about first, nothing sent")
        m.reset_log()
        r = app.tool({"action": "status", "target": "Hall sensor"})
        state = r.get("state", {}) if isinstance(r, dict) else {}
        check(state.get("current_c") == 21.4 and state.get("motion") is True and
              [e["path"] for e in m.log()] == ["/clip/v2/resource/temperature", "/clip/v2/resource/motion"],
              "status of the sensor reads its temperature and motion (%s)" % (r,))
        check(BRIDGE_ID not in json.dumps(r) and "127.0.0.1" not in json.dumps(r) and rid("d-sensor") not in json.dumps(r),
              "status results carry no address or id")

        # A key the bridge stops accepting: sent once, never again, not even by a new run.
        m.set("revoke")
        m.reset_log()
        r = app.tool({"action": "on", "target": "Sofa lamp"})
        check(isinstance(r, str) and "no longer accepts" in r and [e["key"] for e in m.log()] == ["wrong"],
              "a revoked key: the command fails, the bridge saw it once (%s)" % (r,))
        check(DEVICE_ID in app.load()["secrets"].get("hue_bridge", {}).get("refused", {}), "the refusal is kept in the app's file")
        m.reset_log()
        r = app.tool({"action": "off", "target": "Sofa lamp"})
        out = app.run("--home-refresh")
        check("no longer accepts" in (r if isinstance(r, str) else json.dumps(r)) and "no longer accepts" in out and m.log() == [],
              "new runs send nothing to the bridge, and the settings say why")
        m.clear("revoke")


def test_secrets_never_printed():
    leaked = [k for k in keys if any(k in o for o in outputs)]
    check(keys and not leaked, "no application key appears in anything the programs printed (%d keys watched)" % len(keys))


def main():
    if not os.access(CLI, os.X_OK):
        sys.exit("no hue_cli at %s (run tests/run_unit.sh first)" % CLI)
    tmp = tempfile.mkdtemp(prefix="hue_e2e_run_")
    only = [t for t in os.environ.get("HUE_E2E_ONLY", "").split(",") if t]
    try:
        for test in (test_pairing, test_pairing_refusals, test_things, test_apply, test_groups, test_refused_key,
                     test_certificate_change, test_unreachable, test_malformed, test_unpair, test_app):
            if only and test.__name__ not in only:
                continue
            print("--- " + test.__name__, flush=True)
            try:
                test(tempfile.mkdtemp(dir=tmp))
            except Exception as e:   # a crash is a failed check, and the remaining tests still run
                check(False, "%s raised %s: %s" % (test.__name__, type(e).__name__, e))
        if keys or not only:   # a run of groups that pair nothing has no key to watch
            test_secrets_never_printed()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    failed = results.count(False)
    print("hue e2e: %d checks, %d failed" % (len(results), failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
