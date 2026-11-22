#!/usr/bin/env python3
"""WiZ end to end: the driver (through wiz_cli) and the app (zero_bot) against simulated WiZ devices.

usage: tests/wiz_e2e.py [path/to/wiz_cli [path/to/zero_bot]]
       (defaults: build/unit/wiz_cli, which tests/run_unit.sh builds, and build/cli/zero_bot)

Every device is tests/mocks/wiz/mock_wiz.py on 127.0.0.1, on ports the system picks; its counters
(GET /mock/stats) show what reached it, so "sent once", "not sent again" and "signed with the
right key" are judged from the device's side, not from what the driver says about itself. The
second half runs the command-line build of the app on the stand-in test network
(ZB_TEST_NETWORK=1, where only 127.x addresses are ever contacted) with its state in a fresh
temporary folder. Nothing outside 127.0.0.1 is contacted and nothing is scanned. Prints one line
per check and exits non-zero when any fails.
"""
import atexit
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import netguard   # noqa: E402
netguard.sanitize()   # no key or proxy of the caller's; the app pinned (ZB_EXP_NET) and on the stand-in network

CLI = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(HERE, "..", "build", "unit", "wiz_cli")
APP = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.join(HERE, "..", "build", "cli", "zero_bot")
KEY = "00112233445566778899aabbccddeeff"
WRONG = "ffeeddccbbaa99887766554433221100"
TMP = tempfile.mkdtemp(prefix="wiz-e2e-")
NO_PROXY = urllib.request.build_opener(urllib.request.ProxyHandler({}))
# What the app counts as "it did not answer" (control.cpp unreachable()).
UNREACHABLE_WORDS = ("connect", "reach", "answer", "respond", "timed out", "timeout", "no route", "unreachable")
# The MAC each simulated model reports (mock_wiz.py MODELS): what discovery would have saved it under.
MACS = {"rgb": "a8bb5006033d", "rgb_lowdim": "a8bb50aa0001", "tw": "6c29905a067c", "dw": "a8bb50060001", "old": "a8bb50bdf8d7",
        "typeid": "a8bb502054e3", "socket": "d8a0119906b7", "socket_plain": "a8bb50df7693", "fan": "a8bb50fa0001",
        "signed": "a8bb50510001", "camera": "a8bb50ca0001"}

checks = failed = 0
outputs = []   # everything printed by the CLI and the app, to check that no key ever appears
mocks = []


def check(name, ok, detail=""):
    global checks, failed
    checks += 1
    failed += not ok
    print("%s  %s%s" % ("ok  " if ok else "FAIL", name, "" if ok else "  -> %s" % (detail,)), flush=True)


def first_state(out):
    """The state of the first thing in a wiz_cli listing, {} when there is none."""
    things = out.get("things") or [{}]
    return things[0].get("state", {})


def params_of(sent, i=0):
    """The params of the i-th request in `sent`, None when there is no such request."""
    return sent[i]["params"] if -len(sent) <= i < len(sent) else None


def unreachable(text):
    return any(w in text.lower() for w in UNREACHABLE_WORDS)


class Mock:
    def __init__(self, model, key=""):
        args = [sys.executable, "-I", os.path.join(HERE, "mocks", "wiz", "mock_wiz.py"), "--port", "0", "--model", model,
                "--stdin-lifeline"] + (["--key", key] if key else [])
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, start_new_session=True,
                                     env=netguard.mock_env())
        mocks.append(self)
        words = self.proc.stdout.readline().split()
        if words[:3] != ["MOCK", "listening", "udp"]:
            raise RuntimeError("the mock did not start: %r" % words)
        self.port, self.http = int(words[3]), int(words[5])
        self.model = model

    def _call(self, path, body=None):
        req = urllib.request.Request("http://127.0.0.1:%d%s" % (self.http, path),
                                     data=None if body is None else json.dumps(body).encode(),
                                     method="GET" if body is None else "POST")
        with NO_PROXY.open(req, timeout=10) as r:
            return json.load(r)

    def stats(self):
        return self._call("/mock/stats")

    def config(self, **cfg):
        self._call("/mock/config", cfg)

    def reset(self):
        self._call("/mock/reset", {})

    def requests(self, method=None):
        return [r for r in self.stats()["requests"] if method is None or r["method"] == method]

    def stop(self):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        self.proc.kill()
        self.proc.wait()


def cleanup():
    for m in mocks:
        if m.proc.poll() is None:
            m.stop()
    shutil.rmtree(TMP, ignore_errors=True)


atexit.register(cleanup)


class State:
    """One saved device and the family's secrets, as the app's store would hold them."""
    count = 0

    def __init__(self, mock, secrets=None):
        State.count += 1
        self.path = os.path.join(TMP, "state%d.json" % State.count)
        self.mock = mock
        with open(self.path, "w") as f:
            json.dump({"device": {"id": "wiz:" + mock.model, "kind": "wiz", "name": "WiZ", "mac": MACS[mock.model],
                                  "ip": "127.0.0.1", "port": mock.port, "info": {}, "auth": {}},
                       "things": [], "secrets": secrets or {}}, f)

    def run(self, *args, timeout=30):
        """(exit status, the JSON printed or {} when none, stderr)."""
        t0 = time.monotonic()
        p = subprocess.run([CLI, self.path, *args], capture_output=True, text=True, timeout=timeout)
        self.seconds = time.monotonic() - t0
        outputs.append(p.stdout + p.stderr)
        try:
            out = json.loads(p.stdout) if p.stdout.strip() else {}
        except ValueError:
            out = {"unparsed": p.stdout}
        return p.returncode, out if isinstance(out, dict) else {"unparsed": out}, p.stderr.strip()

    def load(self):
        with open(self.path) as f:
            return json.load(f)

    def things(self):
        return {t["id"]: t for t in self.load()["things"]}

    def ref(self, thing_id):
        """The driver's private addressing of one thing, {} when there is no such thing."""
        return self.things().get(thing_id, {}).get("ref", {})

    def secrets(self):
        return self.load()["secrets"]


def enumerate_case(model, want):
    """Enumerates one model and compares its things with `want`: {id: (type, cap_names, state)}."""
    mock = Mock(model)
    st = State(mock)
    rc, out, err = st.run("enumerate")
    got = {t["id"]: (t["type"], t["cap_names"], t["state"]) for t in (out or {}).get("things", [])} if rc == 0 else err
    check("%s: things, capabilities and state" % model, got == want, got)
    sent = [r["method"] for r in mock.requests()]
    check("%s: only reads were sent (%s)" % (model, ", ".join(sorted(set(sent)))),
          "setPilot" not in sent and set(sent) <= {"getSystemConfig", "getModelConfig", "getUserConfig", "getPower", "getPilot"}, sent)
    return mock, st


def driver_tests():
    if not os.access(CLI, os.X_OK):
        check("wiz_cli is built (tests/run_unit.sh builds it)", False, CLI)
        return

    # ---- what each kind of device yields
    rgb, rgb_st = enumerate_case("rgb", {"light": ("light", ["power", "brightness", "color_temp", "color"],
                                                   {"on": False, "brightness": 13, "hue": 30, "saturation": 100})})
    check("rgb: the white range from getModelConfig", rgb_st.ref("light").get("kmin") == 2200 and
          rgb_st.ref("light").get("kmax") == 6500, rgb_st.things())
    check("rgb: named after the end of its MAC", rgb_st.things().get("light", {}).get("name") == "WiZ light 033d", rgb_st.things())
    enumerate_case("tw", {"light": ("light", ["power", "brightness", "color_temp"], {"on": False, "brightness": 60, "kelvin": 4000})})
    enumerate_case("dw", {"light": ("light", ["power", "brightness"], {"on": False, "brightness": 40})})
    old, old_st = enumerate_case("old", {"light": ("light", ["power", "brightness", "color_temp", "color"],
                                                   {"on": False, "brightness": 13, "hue": 30, "saturation": 100})})
    check("old firmware: getModelConfig refused, getUserConfig asked, its extRange used",
          [r["method"] for r in old.requests()].count("getUserConfig") == 1 and old_st.ref("light").get("kmin") == 2200,
          old_st.things())
    enumerate_case("typeid", {"light": ("light", ["power", "brightness"], {"on": False, "brightness": 40})})
    sock, sock_st = enumerate_case("socket", {"plug": ("plug", ["power", "sensor"], {"on": False, "power_w": 23.5})})
    enumerate_case("socket_plain", {"plug": ("plug", ["power"], {"on": False})})
    fan, fan_st = enumerate_case("fan", {"light": ("light", ["power", "brightness"], {"on": False, "brightness": 40}),
                                         "fan": ("fan", ["power", "fan_speed"], {"on": False, "fan": 33})})
    camera = Mock("camera")
    rc, out, err = State(camera).run("enumerate")
    check("a module of no known kind yields nothing, with a sentence", rc == 1 and "cannot control this kind" in err, (rc, out, err))

    # ---- changes, checked by what the device was sent and what it now is
    def apply(st, thing, change, deadline=2500):
        before = len(st.mock.requests("setPilot"))
        rc, out, err = st.run("apply", thing, json.dumps(change), "--deadline", str(deadline))
        sent = st.mock.requests("setPilot")[before:]
        return rc, err, sent

    def last_params(mock):
        sent = mock.requests("setPilot")
        return sent[-1]["params"] if sent else None

    cases = [
        ("on", {"on": True}, {"state": True}),
        ("brightness 40", {"on": True, "brightness": 40}, {"state": True, "dimming": 40}),
        ("brightness 1 is raised to the lowest dimming, 10", {"brightness": 1}, {"state": True, "dimming": 10}),
        ("warm white", {"on": True, "kelvin": 2700}, {"state": True, "temp": 2700}),
        ("a white beyond the range is clamped", {"kelvin": 9000}, {"state": True, "temp": 6500}),
        ("red", {"on": True, "hs": [0, 100]}, {"state": True, "r": 255, "g": 0, "b": 0, "w": 0}),
        ("pale green: white mixed in", {"hs": [120, 30]}, {"state": True, "r": 0, "g": 153, "b": 0, "w": 128}),
        ("off", {"on": False}, {"state": False}),
    ]
    for name, change, want in cases:
        rc, err, sent = apply(rgb_st, "light", change)
        check("rgb %s: sent once, exactly %s" % (name, json.dumps(want)), rc == 0 and len(sent) == 1 and params_of(sent) == want,
              (rc, err, [s["raw"] for s in sent]))
    p = rgb.stats()["pilot"]
    check("the device is now off, in the colour last set", p["state"] is False and p["g"] == 153 and p["temp"] == 0, p)
    check("requests are unsigned on firmware that does not sign", all(not r["signed"] for r in rgb.requests()))

    lowdim = Mock("rgb_lowdim")
    low_st = State(lowdim)
    low_st.run("enumerate")
    rc, err, sent = apply(low_st, "light", {"brightness": 1})
    check("minDimLevel 1: brightness 1 is sent as 1", rc == 0 and last_params(lowdim) == {"state": True, "dimming": 1}, (rc, err))

    tw_mock = Mock("tw")
    tw_st = State(tw_mock)
    tw_st.run("enumerate")
    rc, err, sent = apply(tw_st, "light", {"kelvin": 2200})
    check("tunable white: a white below its range (2700) is clamped", rc == 0 and last_params(tw_mock) == {"state": True, "temp": 2700},
          (rc, err))
    rc, err, sent = apply(tw_st, "light", {"hs": [0, 100]})
    check("tunable white: a colour is refused and nothing is sent", rc == 1 and not sent and "cannot change colour" in err, (rc, err))

    rc, err, sent = apply(rgb_st, "light", {"kelvin": 2700, "hs": [0, 100]})
    check("a white and a colour together are refused before anything is sent", rc == 1 and not sent and not unreachable(err), (rc, err))
    rc, err, sent = apply(sock_st, "plug", {"on": True, "brightness": 50})
    check("a plug is not dimmed: refused, nothing sent", rc == 1 and not sent and not unreachable(err), (rc, err))
    rc, err, sent = apply(sock_st, "plug", {"on": True})
    check("plug on", rc == 0 and params_of(sent) == {"state": True} and len(sent) == 1 and sock.stats()["pilot"]["state"] is True,
          (rc, err))
    sock.reset()
    rc, out, err = sock_st.run("read")
    check("reading the plug by name: on, and its power", rc == 0 and first_state(out) == {"on": True, "power_w": 23.5}, (rc, out, err))
    check("which asked for the pilot and the power", sorted(r["method"] for r in sock.requests()) == ["getPilot", "getPower"],
          sock.requests())
    sock.reset()
    rc, out, err = sock_st.run("read", "--brief")
    check("reading everything briefly: the pilot only", rc == 0 and [r["method"] for r in sock.requests()] == ["getPilot"] and
          first_state(out) == {"on": True}, (rc, out, err, sock.requests()))

    rc, err, sent = apply(fan_st, "fan", {"on": True})
    check("fan on: fanState 1, its light untouched", rc == 0 and params_of(sent) == {"fanState": 1} and
          fan.stats()["pilot"]["state"] is False, (rc, err))
    rc, err, sent = apply(fan_st, "fan", {"fan": 50})
    check("fan at 50 %: speed 3 of 6", rc == 0 and params_of(sent) == {"fanState": 1, "fanSpeed": 3}, (rc, err))
    rc, err, sent = apply(fan_st, "light", {"on": True, "brightness": 70})
    check("the fan's light on at 70 %", rc == 0 and params_of(sent) == {"state": True, "dimming": 70}, (rc, err))
    rc, err, sent = apply(fan_st, "fan", {"on": False})
    p = fan.stats()["pilot"]
    check("fan off; its light stays on", rc == 0 and p["fanState"] == 0 and p["state"] is True, p)
    rc, out, err = fan_st.run("read")
    st = {t["id"]: t["state"] for t in (out or {}).get("things", [])}
    check("read back: the light on at 70 %, the fan off at speed 3", st == {"light": {"on": True, "brightness": 70},
                                                                           "fan": {"on": False, "fan": 50}}, (rc, st, err))

    # ---- datagrams lost, and answers that are not answers
    rgb.config(drop="alternate")
    rgb.reset()
    rc, out, err = rgb_st.run("enumerate")
    check("every other datagram lost: enumerate still works", rc == 0 and len(out.get("things", [])) == 1, (rc, err))
    sent = [r for r in rgb.requests() if r["method"] == "getSystemConfig"]
    check("each lost request was sent again", len(sent) == 2 and sent[0]["dropped"] and not sent[1]["dropped"], sent)
    check("from one socket per request (the resend comes from the same port)", len({r["port"] for r in sent}) == 1, sent)
    rc, err, sent = apply(rgb_st, "light", {"on": True})
    check("every other datagram lost: a change is sent twice and done", rc == 0 and len(sent) == 2 and rgb.stats()["pilot"]["state"] is True,
          (rc, err, len(sent)))
    rgb.config(drop="none", garbage_first=True, late_other=True)
    rc, err, sent = apply(rgb_st, "light", {"on": False})
    check("garbage and stray answers before the real one are skipped", rc == 0 and len(sent) == 1 and rgb.stats()["pilot"]["state"] is False,
          (rc, err))
    rgb.config(garbage_first=False, late_other=False, drop="all")
    rgb.reset()
    rc, out, err = rgb_st.run("enumerate")
    check("nothing answers (local communication off): enumerate says what to check",
          rc == 1 and "did not answer" in err and "Allow local communication" in err and unreachable(err), err)
    # A change starts by asking who is at the address (getPilot), with at most half its time.
    rgb.reset()
    rc, err, sent = apply(rgb_st, "light", {"on": True}, deadline=2500)
    asked = rgb.requests()
    check("nothing answers: a change gives up by its deadline", rc == 1 and unreachable(err) and rgb_st.seconds < 3.2,
          (err, rgb_st.seconds))
    check("after a few resends of the question who it is, not a flood (at most 6 datagrams), and no change sent",
          2 <= len(asked) <= 6 and {r["method"] for r in asked} == {"getPilot"} and not sent, [r["method"] for r in asked])
    rgb.reset()
    rc, err, sent = apply(rgb_st, "light", {"on": True}, deadline=6000)
    check("nothing answers for 6 s: still at most 6 datagrams, given up within the question's share of the time (1.25 s)",
          rc == 1 and len(rgb.requests()) <= 6 and not sent and rgb_st.seconds < 2.0, (len(rgb.requests()), rgb_st.seconds))
    rgb.config(drop="none")


def signed_tests():
    if not os.access(CLI, os.X_OK):
        return
    dev = Mock("signed", key=KEY)
    st = State(dev)
    rc, out, err = st.run("enumerate")
    check("signed firmware: enumerated without a key (reads are open)", rc == 0 and len(out.get("things", [])) == 1, (rc, err))
    check("its thing is marked signed", st.ref("light").get("signed") is True, st.things())
    rc, step, err = st.run("step")
    check("the signing key is asked for (a token step)", step.get("kind") == "token" and step.get("field") == "WiZ signing key" and
          "udp_signing_key" in step.get("prompt", ""), step)
    rc, out, err = st.run("apply", "light", '{"on":true}')
    check("without a key nothing is sent", rc == 1 and "only accepts signed commands" in err and not dev.requests("setPilot"), err)
    rc, out, err = st.run("pair", "not a key")
    check("something that is not a key is refused, nothing saved", rc == 1 and "32 characters" in err and st.secrets() == {}, err)

    rc, out, err = st.run("pair", WRONG)
    check("a wrong key is saved; nothing is sent when it is typed", rc == 0 and st.secrets().get("wiz") == {"signing_key": WRONG} and
          not dev.requests("setPilot"), (rc, err, st.secrets()))
    rc, step, err = st.run("step")
    check("then no step is asked", step.get("kind") == "none", step)
    rc, out, err = st.run("apply", "light", '{"on":true,"brightness":50}')
    sent = dev.requests("setPilot")
    check("the device refuses the wrong key", rc == 1 and "did not accept the signing key" in err and not unreachable(err), err)
    check("from the device's side: one signed request, its signature wrong", len(sent) == 1 and sent[0]["signed"] and
          sent[0]["sig_ok"] is False, sent)
    check("the refusal is kept in the file", st.secrets().get("wiz", {}).get("refused") is True, st.secrets())
    rc, step, err = st.run("step")
    check("the key is asked for again, saying why", step.get("kind") == "token" and "did not accept" in step.get("prompt", ""), step)
    for attempt in range(2):
        rc, out, err = st.run("apply", "light", '{"on":true}')
        check("a later run does not send the refused key (run %d)" % (attempt + 1),
              rc == 1 and "did not accept the signing key" in err and len(dev.requests("setPilot")) == 1, (err, len(dev.requests("setPilot"))))
    p = dev.stats()["pilot"]
    check("the device was not changed", p["state"] is False, p)
    rc, out, err = st.run("read")
    rc, out, err = st.run("enumerate")
    check("nor is the refused key used for reads: one signed request in all, of any kind",
          [r["method"] for r in dev.requests() if r["signed"]] == ["setPilot"], [r["method"] for r in dev.requests() if r["signed"]])

    export = json.dumps({"homes": [{"name": "Home", "udp_signing_key": KEY.upper()}]})
    rc, out, err = st.run("pair", export)
    check("the right key, pasted as the exported file", rc == 0 and st.secrets().get("wiz") == {"signing_key": KEY}, (rc, err, st.secrets()))
    rc, out, err = st.run("apply", "light", '{"on":true,"brightness":50}')
    sent = dev.requests("setPilot")
    check("a signed change goes through", rc == 0, err)
    check("from the device's side: signed with the right key, the time fresh, the params as asked",
          len(sent) == 2 and sent[-1]["sig_ok"] is True and {k: v for k, v in sent[-1]["params"].items() if k != "sigTs"} ==
          {"state": True, "dimming": 50} and json.loads(sent[-1]["raw"]).get("env") == "pro", sent[-1:])
    check("and it did it", dev.stats()["pilot"]["state"] is True and dev.stats()["pilot"]["dimming"] == 50, dev.stats()["pilot"])
    rc, out, err = st.run("read")
    check("read after signing", rc == 0 and first_state(out).get("on") is True, (rc, err))

    # A device not yet known to sign (never read since its firmware changed): unsigned, then the key once.
    fresh = State(dev, secrets={"wiz": {"signing_key": KEY}})
    with open(fresh.path) as f:
        root = json.load(f)
    root["device"]["id"] = "wiz:fresh"
    root["things"] = [{"id": "light", "name": "WiZ light", "type": "light", "caps": 15,
                       "ref": {"k": "light", "kmin": 2700, "kmax": 5000, "mind": 10}, "state": {}}]
    with open(fresh.path, "w") as f:
        json.dump(root, f)
    before = len(dev.requests("setPilot"))
    rc, out, err = fresh.run("apply", "light", '{"on":false}')
    sent = dev.requests("setPilot")[before:]
    check("firmware that started signing: one unsigned request turned down, then one signed with the saved key",
          rc == 0 and [s["signed"] for s in sent] == [False, True] and sent[1]["sig_ok"] is True, (rc, err, sent))

    leaked = [k for k in (KEY, WRONG, KEY.upper()) if any(k in o for o in outputs)]
    check("no key appears in anything wiz_cli printed", not leaked, leaked)


# ---------------------------------------------------------------------------------------------
class App:
    def __init__(self):
        self.state = tempfile.mkdtemp(prefix="app-", dir=TMP)
        self.env = dict(os.environ, XDG_CONFIG_HOME=self.state + "/config", XDG_DATA_HOME=self.state + "/data",
                        XDG_CACHE_HOME=self.state + "/cache", ZB_TEST_NETWORK="1", ZB_NO_AUDIO="1")
        self.env.pop("ZB_DRY_RUN", None)
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


def saved_device(mock, name):
    return {"id": "wiz:" + mock.model, "kind": "wiz", "name": name, "model": "", "mac": MACS[mock.model], "last_ip": "127.0.0.1",
            "port": mock.port, "info": {}, "found_by": ["wiz"], "enabled": True, "last_seen": 1, "problem": "", "things": []}


def app_tests():
    if not os.access(APP, os.X_OK):
        check("the command-line build exists for the app checks", False, APP)
        return
    app = App()
    out = app.run("--home-refresh")
    network = next((l.split(" id ")[1].strip() for l in out.splitlines() if l.startswith("network: Test network, id ")), "")
    check("app: runs on the stand-in test network", network.startswith("net-"), out)

    light, plug, signed = Mock("rgb"), Mock("socket"), Mock("signed", key=KEY)
    app.save({"version": 2, "secrets": {},
              "networks": {network: {"label": "Test network", "gateway_mac": "02:00:00:00:00:00", "allowed_at": 1, "last_scan": 1,
                                     "devices": [saved_device(light, "WiZ 033d"), saved_device(plug, "WiZ 06b7"),
                                                 saved_device(signed, "WiZ 0001")]}}})
    out = app.run("--home-refresh")
    check("app: refresh reads all three", "3 devices ready." in out, out)
    names = {t["name"]: t for d in app.load()["networks"][network]["devices"] for t in d["things"]}
    check("app: the things are saved under the driver's names", set(names) == {"WiZ light 033d", "WiZ plug 06b7", "WiZ light 0001"}, names)

    r = app.tool({"action": "on", "target": "WiZ light 033d"})
    check("app: on by name switches the light on", r == {"now": "on", "target": "WiZ light 033d"} and light.stats()["pilot"]["state"] is True, r)
    r = app.tool({"action": "set", "target": "WiZ light 033d", "brightness": 30})
    check("app: brightness 30 reaches the light as dimming 30", isinstance(r, dict) and light.stats()["pilot"]["dimming"] == 30, r)
    r = app.tool({"action": "set", "target": "WiZ light 033d", "color": "red"})
    p = light.stats()["pilot"]
    check("app: red reaches the light as its red LED", isinstance(r, dict) and p["r"] == 255 and p["g"] == 0 and p["temp"] == 0, (r, p))
    r = app.tool({"action": "set", "target": "WiZ light 033d", "color": "warm white"})
    p = light.stats()["pilot"]
    check("app: warm white reaches the light as a white", isinstance(r, dict) and p["temp"] == 2700, (r, p))
    r = app.tool({"action": "on", "target": "WiZ plug 06b7"})
    check("app: the plug on", r == {"now": "on", "target": "WiZ plug 06b7"} and plug.stats()["pilot"]["state"] is True, r)
    r = app.tool({"action": "status", "target": "WiZ plug 06b7"})
    check("app: the plug's status with its power", isinstance(r, dict) and r.get("state") == {"on": True, "power_w": 23.5}, r)
    r = app.tool({"action": "status"})
    check("app: status carries no address or id", isinstance(r, dict) and "127.0.0.1" not in json.dumps(r) and
          "a8bb" not in json.dumps(r), r)

    # The signed light in the app: listed, but a change needs the key, which is never sent once refused.
    r = app.tool({"action": "on", "target": "WiZ light 0001"})
    check("app: the signed light without a key says so; nothing sent", isinstance(r, str) and "signed commands" in r and
          not signed.requests("setPilot"), r)
    out = app.run("--home-refresh")
    dev = next(d for d in app.load()["networks"][network]["devices"] if d["id"] == "wiz:signed")
    check("app: after a refresh it needs setting up, and says how", dev["needs_setup"] is True and "udp_signing_key" in dev["problem"],
          (dev["needs_setup"], dev["problem"]))
    root = app.load()
    root["secrets"]["wiz"] = {"signing_key": WRONG}   # as the settings page saves a typed key
    for d in root["networks"][network]["devices"]:
        d["needs_setup"] = False
    app.save(root)
    r = app.tool({"action": "on", "target": "WiZ light 0001"})
    check("app: a wrong key is refused by the device", isinstance(r, str) and r.startswith("ERROR") and len(signed.requests("setPilot")) == 1,
          (r, len(signed.requests("setPilot"))))
    check("app: the refusal is kept in the file", app.load()["secrets"]["wiz"].get("refused") is True, app.load()["secrets"])
    app.tool({"action": "on", "target": "WiZ light 0001"})
    app.run("--home-refresh")
    check("app: a later run sends the refused key neither for a change nor for a refresh", len(signed.requests("setPilot")) == 1 and
          [r["method"] for r in signed.requests() if r["signed"]] == ["setPilot"], [r["method"] for r in signed.requests() if r["signed"]])
    root = app.load()
    root["secrets"]["wiz"] = {"signing_key": KEY}
    app.save(root)
    out = app.run("--home-refresh")
    r = app.tool({"action": "on", "target": "WiZ light 0001"})
    sent = signed.requests("setPilot")
    check("app: with the right key typed, the signed light switches", r == {"now": "on", "target": "WiZ light 0001"} and
          bool(sent) and sent[-1]["sig_ok"] is True and signed.stats()["pilot"]["state"] is True, (r, sent[-1:]))

    r = app.tool({"action": "off", "target": "all"})
    check("app: everything off", isinstance(r, dict) and r.get("changed") == 3 and not light.stats()["pilot"]["state"] and
          not plug.stats()["pilot"]["state"] and not signed.stats()["pilot"]["state"], r)
    leaked = [k for k in (KEY, WRONG) if any(k in o for o in outputs)]
    check("no key appears in anything the app printed", not leaked, leaked)


def moved_tests():
    """The router gives a saved device's address to another WiZ device (it lost its leases when it
    restarted): a command for the light must not switch the plug that now answers there."""
    if not os.access(CLI, os.X_OK) or not os.access(APP, os.X_OK):
        return
    light, plug = Mock("rgb"), Mock("socket")
    st = State(light)
    root = st.load()
    root["device"]["id"] = "wiz:a8bb5006033d"   # as discovery names it: by the MAC it reported
    with open(st.path, "w") as f:
        json.dump(root, f)
    rc, out, err = st.run("enumerate")
    check("moved: the light listed at its own address", rc == 0 and "light" in st.things(), (rc, err))
    root = st.load()
    root["device"]["port"] = plug.port
    with open(st.path, "w") as f:
        json.dump(root, f)
    rc, out, err = st.run("apply", "light", '{"on":true}')
    check("moved: a change for the light, at an address the plug now has, is not made", rc == 1 and unreachable(err), (rc, err))
    check("moved: from the plug's side: asked who it is, no change sent, still off",
          [r["method"] for r in plug.requests()] == ["getPilot"] and plug.stats()["pilot"]["state"] is False, plug.requests())
    rc, out, err = st.run("enumerate")
    check("moved: the plug's things are not listed as the light's", rc == 1 and set(st.things()) == {"light"}, (rc, err, st.things()))

    # The same through the app, with devices saved under their MACs as discovery saves them.
    app = App()
    out = app.run("--home-refresh")
    network = next((l.split(" id ")[1].strip() for l in out.splitlines() if l.startswith("network: Test network, id ")), "")
    lamp, heater = Mock("rgb"), Mock("socket")
    devices = [saved_device(lamp, "WiZ 033d"), saved_device(heater, "WiZ 06b7")]
    devices[0]["id"], devices[1]["id"] = "wiz:a8bb5006033d", "wiz:d8a0119906b7"
    app.save({"version": 2, "secrets": {},
              "networks": {network: {"label": "Test network", "gateway_mac": "02:00:00:00:00:00", "allowed_at": 1, "last_scan": 1,
                                     "devices": devices}}})
    out = app.run("--home-refresh")
    check("app moved: both read at their own addresses", "2 devices ready." in out, out)
    root = app.load()
    saved = root["networks"][network]["devices"]
    saved[0]["port"], saved[1]["port"] = heater.port, lamp.port   # the two addresses swapped
    app.save(root)
    r = app.tool({"action": "on", "target": "WiZ light 033d"})
    check("app moved: turning on the light does not switch the plug now at its address",
          isinstance(r, str) and r.startswith("ERROR") and not heater.requests("setPilot") and heater.stats()["pilot"]["state"] is False,
          (r, heater.requests("setPilot")))
    r = app.tool({"action": "on", "target": "WiZ plug 06b7"})
    check("app moved: nor does turning on the plug switch the light", isinstance(r, str) and r.startswith("ERROR") and
          not lamp.requests("setPilot") and lamp.stats()["pilot"]["state"] is False, (r, lamp.requests("setPilot")))
    out = app.run("--home-refresh")
    things = {t["name"] for d in app.load()["networks"][network]["devices"] for t in d["things"]}
    check("app moved: a refresh keeps each device's own things", things == {"WiZ light 033d", "WiZ plug 06b7"} and "0 ready" in out,
          (out, things))


def identity_tests():
    """A device must say it is the one saved before it is changed: its MAC, in getPilot or, for
    firmware whose getPilot leaves it out, in getSystemConfig. A device at the saved address that names
    none (as another device could) is never changed. Checked from the device's side."""
    if not os.access(CLI, os.X_OK):
        return
    lamp = Mock("rgb")
    st = State(lamp)
    rc, out, err = st.run("enumerate")
    check("identity: the light listed while it names its MAC", rc == 0 and "light" in st.things(), (rc, err))

    lamp.config(mac="system_only")
    lamp.reset()
    rc, out, err = st.run("apply", "light", '{"on":true}')
    check("identity: firmware whose getPilot names no MAC is asked getSystemConfig, then changed",
          rc == 0 and [r["method"] for r in lamp.requests()] == ["getPilot", "getSystemConfig", "setPilot"] and
          lamp.stats()["pilot"]["state"] is True, (rc, err, [r["method"] for r in lamp.requests()]))

    for mode in ("none", "empty"):
        lamp.config(mac=mode)
        lamp.reset()
        before = lamp.stats()["pilot"]["state"]
        rc, out, err = st.run("apply", "light", json.dumps({"on": not before}))
        check("identity (%s MAC): a device that does not say who it is gets no change" % mode,
              rc == 1 and unreachable(err) and not lamp.requests("setPilot") and lamp.stats()["pilot"]["state"] is before and
              [r["method"] for r in lamp.requests()] == ["getPilot", "getSystemConfig"], (rc, err, [r["method"] for r in lamp.requests()]))
        rc, out, err = st.run("read")
        check("identity (%s MAC): nor is it read as the light" % mode, rc == 1 and "did not say which device" in err, (rc, err))
        rc, out, err = st.run("enumerate")
        check("identity (%s MAC): nor listed as it, the light's things kept" % mode, rc == 1 and set(st.things()) == {"light"},
              (rc, err, st.things()))
    lamp.config(mac="all")

    # A record saved without a MAC: nothing is sent to change it.
    bare = State(lamp)
    root = bare.load()
    root["device"]["mac"] = ""
    with open(bare.path, "w") as f:
        json.dump(root, f)
    bare.run("enumerate")
    lamp.reset()
    rc, out, err = bare.run("apply", "light", '{"on":false}')
    check("identity: a device saved without a MAC is not changed, and nothing is sent",
          rc == 1 and "No hardware address" in err and not unreachable(err) and lamp.requests() == [], (rc, err, lamp.requests()))

    if not os.access(APP, os.X_OK):
        return
    # Through the app: the saved light's address now answers without naming a MAC.
    app = App()
    out = app.run("--home-refresh")
    network = next((l.split(" id ")[1].strip() for l in out.splitlines() if l.startswith("network: Test network, id ")), "")
    light = Mock("rgb")
    dev = saved_device(light, "WiZ 033d")
    dev["id"] = "wiz:" + MACS["rgb"]
    app.save({"version": 2, "secrets": {},
              "networks": {network: {"label": "Test network", "gateway_mac": "02:00:00:00:00:00", "allowed_at": 1, "last_scan": 1,
                                     "devices": [dev]}}})
    out = app.run("--home-refresh")
    check("app identity: the light read while it names its MAC", "1 device ready." in out, out)
    light.config(mac="none")
    r = app.tool({"action": "on", "target": "WiZ light 033d"})
    check("app identity: 'on' for the light is not sent to a device that does not say who it is",
          isinstance(r, str) and r.startswith("ERROR") and not light.requests("setPilot") and light.stats()["pilot"]["state"] is False,
          (r, light.requests("setPilot")))


def main():
    driver_tests()
    signed_tests()
    app_tests()
    moved_tests()
    identity_tests()
    print("%d checks, %d failed" % (checks, failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
