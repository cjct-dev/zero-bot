#!/usr/bin/env python3
"""Shelly end to end: the driver, and then the app, against simulated Shelly devices on 127.0.0.1.

usage: tests/shelly_e2e.py [path/to/shelly_cli [path/to/zero_bot]]
       (defaults: build/unit/shelly_cli, which tests/run_unit.sh builds, and build/cli/zero_bot;
        the app part is skipped when the app is not built)

Part 1 drives shelly_cli (the driver alone, its state in a file that stands in for the app's saved
devices, so a second run is "a later run") against tests/mocks/shelly/mock_shelly.py playing Gen1
and Gen2+ models, with and without a password. Part 2 runs the command-line build of the app on
the stand-in test network (ZB_TEST_NETWORK=1) with a saved home of simulated Shellies, through its
own smart_home tool. Every mock binds port 0; every check of what was sent is made from the mock's
side (GET /mock/log), never from what the driver says about itself. Nothing outside 127.0.0.1 is
contacted. Prints one line per check; exits non-zero when any fails.
"""
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(HERE, "..", "build", "unit", "shelly_cli")
APP = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.join(HERE, "..", "build", "cli", "zero_bot")
MOCK = os.path.join(HERE, "mocks", "shelly", "mock_shelly.py")
TMP = tempfile.mkdtemp(prefix="shelly-e2e-")
NO_PROXY = urllib.request.build_opener(urllib.request.ProxyHandler({}))
ENV = dict(os.environ, ZB_TEST_NETWORK="1", ZB_NO_AUDIO="1")
ENV.pop("ZB_DRY_RUN", None)
PASSWORDS = ["hunter2-Gen2", "wrong-Gen2", "changed-Gen2", "S3cret:Gen1", "wrong-Gen1", "changed-Gen1"]
# The state keys the app shows (src/smarthome/home.h).
APP_KEYS = {"on", "brightness", "kelvin", "hue", "saturation", "mode", "target_c", "current_c", "humidity", "position",
            "fan", "volume", "muted", "playing", "source", "title", "power_w", "energy_today_wh", "motion", "contact"}

run_count = fail_count = 0
outputs = []   # everything printed by the programs under test: no password may appear in it
mocks = []


def check(name, ok, detail=""):
    global run_count, fail_count
    run_count += 1
    fail_count += not ok
    print("%s  %s%s" % ("ok  " if ok else "FAIL", name, "" if ok or detail == "" else "  -> %s" % (detail,)), flush=True)


class Mock:
    def __init__(self, model, *extra):
        self.model = model
        self.proc = subprocess.Popen([sys.executable, "-I", MOCK, "--port", "0", "--model", model, *extra],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, start_new_session=True)
        mocks.append(self)
        line = self.proc.stdout.readline().strip()
        if not line.startswith("MOCK listening on 127.0.0.1:"):
            raise RuntimeError("mock did not start: %r" % line)
        self.port = int(line.rsplit(":", 1)[1])
        self.seen = []   # every request, kept across reset() for the final checks

    def _call(self, path, body=None):
        req = urllib.request.Request("http://127.0.0.1:%d%s" % (self.port, path),
                                     data=None if body is None else json.dumps(body).encode(),
                                     method="GET" if body is None else "POST")
        with NO_PROXY.open(req, timeout=10) as r:
            return json.load(r)

    def log(self):
        return self._call("/mock/log")["requests"]

    def reset(self):
        self.seen += self.log()
        self._call("/mock/reset", {})

    def config(self, **cfg):
        self._call("/mock/config", cfg)

    def state(self):
        return self._call("/mock/state")

    def stop(self):
        try:
            self.seen += self.log()
        except Exception:
            pass
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def paths(log):
    """What was asked, compactly: "/shelly", "Switch.Set", "/relay/0?turn=on"."""
    out = []
    for r in log:
        if r.get("rpc"):
            out.append(r["rpc"]["method"])
        else:
            q = "&".join("%s=%s" % kv for kv in r["query"].items())
            out.append(r["path"] + ("?" + q if q else ""))
    return out


def logins(log):
    return [r["auth"] for r in log if r["auth"] in ("ok", "bad", "stale")]


def new_state(name, mock, device_id, auth=None):
    path = os.path.join(TMP, name + ".json")
    dev = {"id": device_id, "kind": "shelly", "name": "", "ip": "127.0.0.1", "port": mock.port, "info": {}, "auth": auth or {}}
    with open(path, "w") as f:
        json.dump({"device": dev, "things": []}, f)
    return path


def load(path):
    with open(path) as f:
        return json.load(f)


def point_at(path, mock):
    s = load(path)
    s["device"]["port"] = mock.port
    with open(path, "w") as f:
        json.dump(s, f)


def cli(path, *args, deadline=None):
    """(ok, json or None, error kind, error text, wall ms)"""
    cmd = [CLI, path, *args] + (["--deadline", str(deadline)] if deadline else [])
    t0 = time.monotonic()
    p = subprocess.run(cmd, env=ENV, capture_output=True, text=True, timeout=60)
    ms = int((time.monotonic() - t0) * 1000)
    outputs.append(p.stdout + p.stderr)
    if p.returncode == 0:
        try:
            return True, json.loads(p.stdout), "", "", ms
        except ValueError:
            return False, None, "bad-output", p.stdout, ms
    err = p.stderr.strip()
    kind = err[len("error["):err.index("]")] if err.startswith("error[") else "?"
    return False, None, kind, err.split(": ", 1)[-1], ms


def things_of(result):
    return {t["id"]: t for t in (result or {}).get("things", [])}


# ================================================================ part 1: the driver
def gen2_open():
    m = Mock("plus1pm")
    st = new_state("plus1pm", m, "shelly:shellyplus1pm-a8032ab12345")
    ok, out, *_ = cli(st, "claims")
    check("gen2: the driver claims kind shelly", ok and out == {"claims": True})
    ok, out, *_ = cli(st, "step")
    check("gen2: no password, no setup", ok and out["kind"] == "none")
    ok, out, kind, err, ms = cli(st, "enumerate")
    t = things_of(out).get("switch:0", {})
    check("gen2: one switch, named as in the Shelly app, metering", ok and list(things_of(out)) == ["switch:0"] and
          t.get("name") == "Desk lamp" and t.get("type") == "switch" and set(t.get("cap_names", [])) == {"power", "sensor"}, out or err)
    check("gen2: its state", t.get("state") == {"on": False, "power_w": 12.5}, t.get("state"))
    check("gen2: the device is named from its model when the owner gave it none", ok and out["device"] == {"name": "Shelly Plus1PM", "model": "Plus1PM"}, out)
    check("gen2: enumerate asked /shelly, Shelly.GetConfig, Shelly.GetStatus, by GET, no password",
          paths(m.log()) == ["/shelly", "Shelly.GetConfig", "Shelly.GetStatus"] and all(r["method"] == "GET" for r in m.log())
          and logins(m.log()) == [], paths(m.log()))
    m.reset()
    ok, out, kind, err, ms = cli(st, "apply", "switch:0", '{"on":true}')
    log = m.log()
    check("gen2: on: /shelly first, the relay's settings again (review 2), then Switch.Set {id:0, on:true}",
          ok and paths(log) == ["/shelly", "Shelly.GetConfig", "Switch.Set"] and log[2]["rpc"]["params"] == {"id": 0, "on": True},
          (err, paths(log)))
    check("gen2: the device is on", m.state()["switch:0"]["output"] is True)
    m.reset()
    ok, out, *_ = cli(st, "read")
    check("gen2: read: /shelly (review 2), one Shelly.GetStatus, state on", ok and paths(m.log()) == ["/shelly", "Shelly.GetStatus"] and
          things_of(out)["switch:0"]["state"]["on"] is True, out)
    m.reset()
    for change, what in (('{"brightness":40}', "a brightness"), ('{"position":50}', "a position"), ('{"hs":[0,100]}', "a colour"),
                         ('{"volume":10}', "a volume")):
        ok, out, kind, err, ms = cli(st, "apply", "switch:0", change)
        check("gen2: %s for a switch is refused before anything is sent" % what, not ok and m.log() == [], (err, paths(m.log())))
    mode = stat.S_IMODE(os.stat(st).st_mode)
    check("the state file is kept private (0600)", mode == 0o600, oct(mode))
    m.stop()


def gen2_password():
    m = Mock("plus1pm", "--password", "hunter2-Gen2")
    st = new_state("plus1pm-pw", m, "shelly:shellyplus1pm-a8032ab12345")
    ok, out, kind, err, ms = cli(st, "enumerate")
    check("gen2 pw: enumerate says a password is needed", not ok and kind == "needs_password", (kind, err))
    check("gen2 pw: and asked nothing that needs one", paths(m.log()) == ["/shelly"] and logins(m.log()) == [], paths(m.log()))
    ok, out, *_ = cli(st, "step")
    check("gen2 pw: the next step asks for the password", ok and out["kind"] == "password" and out["field"] == "Shelly password", out)
    m.reset()
    ok, out, kind, err, ms = cli(st, "pair", "wrong-Gen2")
    check("gen2 pw: a wrong password is refused", not ok and "did not accept this password" in err, err)
    check("gen2 pw: it was tried once, as SHA-256 digest for admin in the device's realm",
          logins(m.log()) == ["bad"] and m.log()[-1].get("digest", {}).get("username") == "admin" and
          m.log()[-1]["digest"]["realm"] == "shellyplus1pm-a8032ab12345" and m.log()[-1]["digest"]["algorithm"].upper() == "SHA-256" and
          m.log()[-1]["digest"]["qop"] == "auth", m.log())
    check("gen2 pw: and is not kept", "password" not in load(st)["device"]["auth"], load(st)["device"]["auth"])
    m.reset()
    ok, out, kind, err, ms = cli(st, "enumerate")
    check("gen2 pw: so nothing is sent with a password afterwards", not ok and kind == "needs_password" and logins(m.log()) == [], paths(m.log()))
    m.reset()
    ok, out, kind, err, ms = cli(st, "pair", "hunter2-Gen2")
    check("gen2 pw: the right password is accepted, with one login", ok and logins(m.log()) == ["ok"], (err, m.log()))
    ok, out, *_ = cli(st, "step")
    check("gen2 pw: nothing more to set up", ok and out["kind"] == "none")
    m.reset()
    ok, out, kind, err, ms = cli(st, "enumerate")
    check("gen2 pw: enumerate with it", ok and "switch:0" in things_of(out) and logins(m.log()) == ["ok", "ok"], (err, m.log()))
    ok, out, kind, err, ms = cli(st, "apply", "switch:0", '{"on":true}')
    check("gen2 pw: switched on with it", ok and m.state()["switch:0"]["output"] is True, err)
    # The owner changes the password in the Shelly app.
    m.config(password="changed-Gen2")
    m.reset()
    ok, out, kind, err, ms = cli(st, "apply", "switch:0", '{"on":false}')
    check("gen2 pw: a changed password: refused, after one attempt", not ok and kind == "refused" and logins(m.log()) == ["bad"],
          (kind, err, m.log()))
    check("gen2 pw: the device was not switched", m.state()["switch:0"]["output"] is True)
    m.reset()
    for args in (("apply", "switch:0", '{"on":false}'), ("read",), ("apply", "switch:0", '{"on":true}')):
        ok, out, kind, err, ms = cli(st, *args)
        check("gen2 pw: a later run (%s) sends nothing at all with the refused password" % args[0],
              not ok and kind == "refused" and m.log() == [], (kind, paths(m.log())))
    ok, out, kind, err, ms = cli(st, "enumerate")
    check("gen2 pw: enumerate asks only /shelly, then stops", not ok and kind == "refused" and paths(m.log()) == ["/shelly"], paths(m.log()))
    ok, out, *_ = cli(st, "step")
    check("gen2 pw: the step says the password was not accepted", ok and out["kind"] == "password" and "did not accept" in out["prompt"], out)
    m.reset()
    ok, out, kind, err, ms = cli(st, "pair", "changed-Gen2")
    check("gen2 pw: typing the new one works", ok and logins(m.log()) == ["ok"], err)
    ok, out, kind, err, ms = cli(st, "apply", "switch:0", '{"on":false}')
    check("gen2 pw: and the device obeys again", ok and m.state()["switch:0"]["output"] is False, err)
    # The device's protection against guessing: "try later", not a refusal.
    m.config(throttle=True)
    m.reset()
    ok, out, kind, err, ms = cli(st, "apply", "switch:0", '{"on":true}')
    check("gen2 pw: a 429 is 'try later'", not ok and kind == "throttled", (kind, err))
    ok, out, *_ = cli(st, "step")
    check("gen2 pw: and not kept as a refusal", ok and out["kind"] == "none" and "refused" not in load(st).get("secrets", {}).get("shelly", {}), out)
    m.config(throttle=False)
    ok, out, kind, err, ms = cli(st, "apply", "switch:0", '{"on":true}')
    check("gen2 pw: it works once the device allows logins again", ok, err)
    # A stale nonce (firmware 2.0 expires them): a fresh challenge, no question to the user.
    m.config(stale_once=True)
    m.reset()
    ok, out, kind, err, ms = cli(st, "apply", "switch:0", '{"on":false}')
    check("gen2 pw: a stale nonce is answered with the new one", ok and logins(m.log()) == ["stale", "ok", "ok"], (err, m.log()))
    ok, out, *_ = cli(st, "step")
    check("gen2 pw: and is not a refusal", ok and out["kind"] == "none")
    # Unpair forgets the password.
    ok, out, *_ = cli(st, "unpair")
    check("gen2 pw: unpair forgets the password", ok and load(st)["device"]["auth"] == {} and "shelly" not in load(st).get("secrets", {}))
    # A check that the device answers with an error is not a success.
    ok, out, kind, err, ms = cli(st, "enumerate")
    check("gen2 pw: after unpairing, the password is asked for again", not ok and kind == "needs_password", (kind, err))
    m.config(hostile="error500")
    ok, out, kind, err, ms = cli(st, "pair", "changed-Gen2")
    check("gen2 pw: a password check the device answers with an error is not taken as accepted", not ok and "reported an error" in err, err)
    m.config(hostile="")
    m.stop()


def gen1_password():
    m = Mock("1pm", "--password", "S3cret:Gen1", "--user", "owner")
    st = new_state("1pm-pw", m, "shelly:shelly1pm-a4cf12f45378")
    ok, out, kind, err, ms = cli(st, "enumerate")
    check("gen1 pw: enumerate says a password is needed, after /shelly alone", not ok and kind == "needs_password" and
          paths(m.log()) == ["/shelly"], (kind, paths(m.log())))
    ok, out, *_ = cli(st, "step")
    check("gen1 pw: the user name is asked for first", ok and out["kind"] == "enter_code" and out["field"] == "User name", out)
    m.reset()
    ok, out, kind, err, ms = cli(st, "pair", "owner")
    check("gen1 pw: typing it contacts nothing", ok and m.log() == [], err)
    ok, out, *_ = cli(st, "step")
    check("gen1 pw: then the password", ok and out["kind"] == "password", out)
    ok, out, kind, err, ms = cli(st, "pair", "wrong-Gen1")
    check("gen1 pw: a wrong password: one attempt, refused", not ok and logins(m.log()) == ["bad"] and "did not accept" in err, (err, m.log()))
    ok, out, *_ = cli(st, "step")
    check("gen1 pw: user name and password are both asked again", ok and out["kind"] == "enter_code", out)
    cli(st, "pair", "owner")
    m.reset()
    ok, out, kind, err, ms = cli(st, "pair", "S3cret:Gen1")
    check("gen1 pw: the right login (a colon in the password is fine)", ok and logins(m.log()) == ["ok"], (err, m.log()))
    m.reset()
    ok, out, kind, err, ms = cli(st, "enumerate")
    t = things_of(out).get("relay:0", {})
    check("gen1 pw: a relay feeding lights is a light, metering", ok and t.get("type") == "light" and t.get("name") == "Porch light" and
          set(t.get("cap_names", [])) == {"power", "sensor"} and t.get("state") == {"on": False, "power_w": 7.5}, out or err)
    check("gen1 pw: enumerate asked /shelly, /settings, /status, logged in for the last two",
          paths(m.log()) == ["/shelly", "/settings", "/status"] and [r["auth"] for r in m.log()] == ["none", "ok", "ok"], m.log())
    m.reset()
    ok, out, kind, err, ms = cli(st, "apply", "relay:0", '{"on":true}')
    check("gen1 pw: on is /relay/0?turn=on, logged in (after /settings, review 2)",
          ok and paths(m.log()) == ["/shelly", "/settings", "/relay/0?turn=on"] and
          m.log()[2]["auth"] == "ok" and m.state()["relays"][0]["ison"] is True, (err, paths(m.log())))
    m.config(password="changed")
    m.reset()
    ok, out, kind, err, ms = cli(st, "read")
    check("gen1 pw: a changed password is refused after one attempt", not ok and kind == "refused" and logins(m.log()) == ["bad"], (kind, m.log()))
    m.reset()
    ok, out, kind, err, ms = cli(st, "apply", "relay:0", '{"on":false}')
    check("gen1 pw: and never sent again", not ok and kind == "refused" and m.log() == [], paths(m.log()))
    m.stop()


def covers():
    m = Mock("plus2pm-cover")
    st = new_state("cover", m, "shelly:shellyplus2pm-a8032ab12345")
    ok, out, kind, err, ms = cli(st, "enumerate")
    t = things_of(out).get("cover:0", {})
    check("cover: a 2PM in cover profile is one blind", ok and list(things_of(out)) == ["cover:0"] and t.get("type") == "cover" and
          t.get("cap_names") == ["position"] and t.get("state") == {"position": 40} and t["ref"].get("calibrated") is True, out or err)
    for change, method, params in (('{"position":100}', "Cover.Open", {"id": 0}), ('{"position":0}', "Cover.Close", {"id": 0}),
                                   ('{"position":30}', "Cover.GoToPosition", {"id": 0, "pos": 30}),
                                   ('{"transport":"stop"}', "Cover.Stop", {"id": 0}), ('{"on":true}', "Cover.Open", {"id": 0})):
        m.reset()
        ok, out, kind, err, ms = cli(st, "apply", "cover:0", change)
        log = m.log()
        check("cover: %s is %s" % (change, method), ok and paths(log) == ["/shelly", "Shelly.GetConfig", method] and
              log[2]["rpc"]["params"] == params, (err, paths(log)))
    check("cover: the position reached", m.state()["cover:0"]["current_pos"] == 100)
    m.stop()
    m = Mock("plus2pm-cover-uncal")
    st = new_state("cover-uncal", m, "shelly:shellyplus2pm-a8032ab12345")
    ok, out, kind, err, ms = cli(st, "enumerate")
    check("cover: an uncalibrated blind has no position", ok and things_of(out)["cover:0"]["state"] == {} and
          things_of(out)["cover:0"]["ref"]["calibrated"] is False, out)
    m.reset()
    ok, out, kind, err, ms = cli(st, "apply", "cover:0", '{"position":30}')
    check("cover: a position for it is refused before anything is sent", not ok and "calibrated" in err and m.log() == [], (err, paths(m.log())))
    ok, out, kind, err, ms = cli(st, "apply", "cover:0", '{"position":100}')
    check("cover: opening it fully works", ok and paths(m.log())[-1] == "Cover.Open", err)
    m.stop()
    m = Mock("25-roller")
    st = new_state("roller", m, "shelly:shelly25-a4cf12f45378")
    ok, out, kind, err, ms = cli(st, "enumerate")
    check("roller: a Gen1 2.5 in roller mode is one blind", ok and list(things_of(out)) == ["roller:0"] and
          things_of(out)["roller:0"]["state"] == {"position": 70} and things_of(out)["roller:0"]["name"] == "Landing", out or err)
    for change, path in (('{"position":35}', "/roller/0?go=to_pos&roller_pos=35"), ('{"transport":"stop"}', "/roller/0?go=stop"),
                         ('{"position":0}', "/roller/0?go=close")):
        m.reset()
        ok, out, kind, err, ms = cli(st, "apply", "roller:0", change)
        check("roller: %s is %s" % (change, path), ok and paths(m.log()) == ["/shelly", "/settings", path], (err, paths(m.log())))
    m.stop()
    m = Mock("25-roller-uncal")
    st = new_state("roller-uncal", m, "shelly:shelly25-a4cf12f45378")
    cli(st, "enumerate")
    m.reset()
    ok, out, kind, err, ms = cli(st, "apply", "roller:0", '{"position":35}')
    check("roller: uncalibrated, a position is refused before anything is sent", not ok and m.log() == [], paths(m.log()))
    m.stop()


def lights():
    cases = [
        ("rgbw", "shelly:shellyplusrgbwpm-a8032ab12345", "rgb:0", [('{"hs":[120,100]}', "RGB.Set", {"id": 0, "on": True, "rgb": [0, 255, 0]}),
                                                                  ('{"brightness":0}', "RGB.Set", {"id": 0, "on": False}),
                                                                  ('{"brightness":25}', "RGB.Set", {"id": 0, "on": True, "brightness": 25})]),
        ("duo", "shelly:shellyduobulbg3-a8032ab12345", "cct:0", [('{"kelvin":9000}', "CCT.Set", {"id": 0, "on": True, "ct": 6500}),
                                                                 ('{"kelvin":3000,"brightness":60}', "CCT.Set", {"id": 0, "on": True, "brightness": 60, "ct": 3000})]),
        ("dimmer", "shelly:shellyplusdimmer-a8032ab12345", "light:0", [('{"brightness":40}', "Light.Set", {"id": 0, "on": True, "brightness": 40}),
                                                                       ('{"on":false}', "Light.Set", {"id": 0, "on": False})]),
    ]
    for model, dev_id, thing, steps in cases:
        m = Mock(model)
        st = new_state(model, m, dev_id)
        ok, out, kind, err, ms = cli(st, "enumerate")
        check("light %s: listed as %s" % (model, thing), ok and thing in things_of(out) and things_of(out)[thing]["type"] == "light", out or err)
        for change, method, params in steps:
            m.reset()
            ok, out, kind, err, ms = cli(st, "apply", thing, change)
            log = m.log()
            check("light %s: %s is %s %s" % (model, change, method, json.dumps(params)),
                  ok and paths(log) == ["/shelly", method] and log[1]["rpc"]["params"] == params, (err, log[1:2]))
        m.stop()
    m = Mock("duo")
    st = new_state("duo-state", m, "shelly:shellyduobulbg3-a8032ab12345")
    ok, out, *_ = cli(st, "enumerate")
    check("light duo: its state in kelvin", ok and things_of(out)["cct:0"]["state"] == {"on": False, "brightness": 80, "kelvin": 4000}, out)
    m.stop()
    gen1 = [
        ("dimmer2", "light:0", [('{"brightness":40}', "/light/0?turn=on&brightness=40"), ('{"brightness":0}', "/light/0?turn=off")]),
        ("rgbw2-color", "light:0", [('{"hs":[0,100],"brightness":60}', "/color/0?turn=on&gain=60&red=255&green=0&blue=0")]),
        ("rgbw2-white", "light:3", [('{"brightness":55}', "/white/3?turn=on&brightness=55")]),
        ("duo1", "light:0", [('{"kelvin":3000}', "/light/0?turn=on&temp=3000")]),
    ]
    for model, thing, steps in gen1:
        m = Mock(model)
        st = new_state(model, m, "shelly:shelly%s-a4cf12f45378" % model.split("-")[0])
        ok, out, kind, err, ms = cli(st, "enumerate")
        check("gen1 light %s: listed with %s" % (model, thing), ok and thing in things_of(out), out or err)
        if model == "rgbw2-white":
            check("gen1 light rgbw2-white: four channels", len(things_of(out)) == 4, list(things_of(out)))
        for change, path in steps:
            m.reset()
            ok, out, kind, err, ms = cli(st, "apply", thing, change)
            check("gen1 light %s: %s is %s" % (model, change, path), ok and paths(m.log()) == ["/shelly", path], (err, paths(m.log())))
        m.stop()


def sensors_and_exclusions():
    m = Mock("plusht")
    st = new_state("plusht", m, "shelly:shellyplusht-a8032ab12345")
    ok, out, kind, err, ms = cli(st, "enumerate")
    t = things_of(out).get("temperature:0", {})
    check("sensor: a Plus H&T is one sensor with temperature and humidity", ok and list(things_of(out)) == ["temperature:0"] and
          t.get("state") == {"current_c": 21.4, "humidity": 49} and t.get("name") == "Nursery", out or err)
    m.reset()
    ok, out, kind, err, ms = cli(st, "apply", "temperature:0", '{"on":true}')
    check("sensor: a change is refused before anything is sent", not ok and m.log() == [], paths(m.log()))
    m.stop()
    m = Mock("ht1")
    st = new_state("ht1", m, "shelly:shellyht1-a4cf12f45378")
    ok, out, *_ = cli(st, "enumerate")
    check("sensor: a Gen1 H&T", ok and things_of(out).get("sensor", {}).get("state") == {"current_c": 12.4, "humidity": 81}, out)
    m.stop()
    for model, dev_id, why in (("plus1-garage", "shelly:shellyplus1pm-a8032ab12345", "a relay named Garage door"),
                               ("plus1-garage-dev", "shelly:shellyplus1pm-a8032ab12345", "a Plus 1 named Garage door, its relay unnamed"),
                               ("plus1-pulse", "shelly:shellyplus1pm-a8032ab12345", "a relay that switches itself off after a second"),
                               ("plus2pm-cover-garage", "shelly:shellyplus2pm-a8032ab12345", "an unnamed blind in a device named Garage"),
                               ("1", "shelly:shelly1-a4cf12f45378", "a Gen1 Shelly 1 named Garage door"),
                               ("1-garage-dev", "shelly:shelly1-a4cf12f45378", "a Gen1 Shelly 1 named Garage door, its relay unnamed"),
                               ("1-pulse", "shelly:shelly1-a4cf12f45378", "a Gen1 relay with auto-off after a second"),
                               ("25-roller-garage", "shelly:shelly25-a4cf12f45378", "a Gen1 2.5 blind (no name of its own) named Garage door"),
                               ("gas1", "shelly:shellygas1-a4cf12f45378", "a gas alarm")):
        m = Mock(model)
        st = new_state(model, m, dev_id)
        ok, out, kind, err, ms = cli(st, "enumerate")
        check("never offered: %s" % why, not ok and "nothing Zero Bot may use" in err, (out, err))
        m.stop()
    m = Mock("wall-thermostat")
    st = new_state("wall", m, "shelly:shellywalldisplay-a8032ab12345")
    ok, out, *_ = cli(st, "enumerate")
    check("never offered: a relay the device's own thermostat drives (its sensor is)", ok and list(things_of(out)) == ["temperature:0"], out)
    m.stop()
    m = Mock("plus2pm")
    st = new_state("plus2pm", m, "shelly:shellyplus2pm-a8032ab12345")
    ok, out, *_ = cli(st, "enumerate")
    ts = things_of(out)
    check("a 2PM in switch profile: two things, an unnamed one after the device, one feeding a light",
          ok and list(ts) == ["switch:0", "switch:1"] and ts["switch:0"]["name"] == "Kitchen 1" and ts["switch:1"]["type"] == "light" and
          ts["switch:1"]["name"] == "Cooker hood light", out)
    m.stop()


def identity_and_hostile():
    a = Mock("plus1pm")
    b = Mock("plus1pm", "--id", "shellyplus1pm-ffffffffffff", "--mac", "FFFFFFFFFFFF")
    st = new_state("ident", a, "shelly:shellyplus1pm-a8032ab12345")
    cli(st, "enumerate")
    point_at(st, b)   # the router gave the address to another Shelly
    ok, out, kind, err, ms = cli(st, "apply", "switch:0", '{"on":true}')
    check("identity: another Shelly at the address is not switched", not ok and kind == "other_device" and paths(b.log()) == ["/shelly"] and
          b.state()["switch:0"]["output"] is False, (kind, paths(b.log())))
    ok, out, kind, err, ms = cli(st, "read")
    check("identity: nor read as this one", not ok and kind == "other_device", (kind, err))
    ok, out, kind, err, ms = cli(st, "enumerate")
    check("identity: nor listed as this one", not ok and kind == "other_device" and load(st)["things"][0]["ref"]["dev"] == "shellyplus1pm-a8032ab12345", (kind, err))
    a.stop()
    b.stop()
    a = Mock("1pm")
    b = Mock("1pm", "--mac", "A4CF12000000")
    st = new_state("ident1", a, "shelly:shelly1pm-a4cf12f45378")
    cli(st, "enumerate")
    point_at(st, b)
    ok, out, kind, err, ms = cli(st, "apply", "relay:0", '{"on":true}')
    check("identity gen1: another device (by MAC) is not switched", not ok and kind == "other_device" and paths(b.log()) == ["/shelly"], (kind, paths(b.log())))
    a.stop()
    b.stop()
    # review: both have passwords of their own. Reading or typing the password again must not send
    # this device's login to the other (in the clear, for Gen1), nor keep its refusal against this one.
    for gen, model, dev_id, extra_b, user in (
            (1, "1pm", "shelly:shelly1pm-a4cf12f45378", ["--mac", "A4CF12000000"], "owner"),
            (2, "plus1pm", "shelly:shellyplus1pm-a8032ab12345", ["--id", "shellyplus1pm-ffffffffffff", "--mac", "FFFFFFFFFFFF"], None)):
        pw_a, pw_b = ("S3cret:Gen1", "wrong-Gen1") if gen == 1 else ("hunter2-Gen2", "wrong-Gen2")
        a = Mock(model, "--password", pw_a, *(["--user", user] if user else []))
        b = Mock(model, "--password", pw_b, *(["--user", user] if user else []), *extra_b)
        st = new_state("ident-pw%d" % gen, a, dev_id)
        cli(st, "enumerate")
        if user:
            cli(st, "pair", user)
        ok, *_ = cli(st, "pair", pw_a)
        ok2, *_ = cli(st, "enumerate")
        point_at(st, b)
        b.reset()
        ok3, out, kind, err, ms = cli(st, "read")
        refused = load(st).get("secrets", {}).get("shelly", {}).get("refused", {})
        check("identity gen%d pw: another Shelly at the address is not read with this one's password" % gen,
              ok and ok2 and not ok3 and kind == "other_device" and logins(b.log()) == [] and paths(b.log()) == ["/shelly"], (kind, err, b.log()))
        check("identity gen%d pw: and no refusal is kept against this one" % gen, refused == {}, refused)
        # The owner changes this device's password, it is refused once; then the router moves it and
        # the other Shelly gets its address before the new password is typed.
        new_pw = "changed-Gen%d" % gen
        point_at(st, a)
        a.config(password=new_pw)
        ok3, out, kind, err, ms = cli(st, "read")
        point_at(st, b)
        b.reset()
        if user:
            cli(st, "pair", user)
        ok4, out, kind4, err4, ms = cli(st, "pair", new_pw)
        check("identity gen%d pw: a password typed again goes only to its own device" % gen,
              kind == "refused" and not ok4 and "Another device" in err4 and logins(b.log()) == [] and
              load(st)["device"]["auth"].get("password") == new_pw, (kind, err4, b.log()))
        point_at(st, a)
        a.reset()
        ok3, out, kind, err, ms = cli(st, "read")
        check("identity gen%d pw: back at its own address it is read with one login" % gen, ok3 and logins(a.log()) == ["ok"], (err, a.log()))
        a.stop()
        b.stop()
    # Something that is not a Shelly at all (a printer's web page) now has the address.
    asked = []

    class Printer(BaseHTTPRequestHandler):
        def do_GET(self):
            asked.append(self.path)
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()

        def log_message(self, *a):
            pass
    printer = ThreadingHTTPServer(("127.0.0.1", 0), Printer)
    threading.Thread(target=printer.serve_forever, daemon=True).start()
    a = Mock("plus1pm")
    st = new_state("printer", a, "shelly:shellyplus1pm-a8032ab12345")
    cli(st, "enumerate")
    a.stop()
    s = load(st)
    s["device"]["port"] = printer.server_address[1]
    with open(st, "w") as f:
        json.dump(s, f)
    ok, out, kind, err, ms = cli(st, "apply", "switch:0", '{"on":true}')
    check("identity: a web server that is not a Shelly is asked /shelly and nothing else", not ok and kind == "other_device" and
          asked == ["/shelly"], (kind, err, asked))
    printer.shutdown()

    m = Mock("plus1pm")
    st = new_state("hostile", m, "shelly:shellyplus1pm-a8032ab12345")
    cli(st, "enumerate")
    m.config(hostile="names")
    ok, out, kind, err, ms = cli(st, "enumerate")
    name = things_of(out).get("switch:0", {}).get("name", "")
    check("hostile names: cleaned and capped", ok and 0 < len(name.encode()) <= 40 and not any(c in name for c in '"\x01\x1b<>{}“'), repr(name))
    check("hostile names: the device's own name too", ok and len(out["device"]["name"].encode()) <= 40 and '"' not in out["device"]["name"], out)
    m.config(hostile="types")
    ok, out, kind, err, ms = cli(st, "read")
    s = things_of(out).get("switch:0", {}).get("state")
    check("hostile types: nothing impossible is believed", ok and s == {}, s)
    for mode, want in (("long", "too long"), ("deep", "could not be understood"), ("garbage", "could not be understood"),
                       ("error500", "unavailable"), ("redirect", "reported an error")):
        m.config(hostile=mode)
        ok, out, kind, err, ms = cli(st, "read")
        check("hostile %s: read fails with \"%s\"" % (mode, want), not ok and want in err and ms < 5000, (err, ms))
    m.config(hostile="redirect")
    m.reset()
    ok, out, kind, err, ms = cli(st, "apply", "switch:0", '{"on":true}')
    check("hostile redirect: not followed (one request after /shelly, nothing more)", not ok and len(m.log()) == 2 and
          m.log()[1]["path"] == "/rpc/Shelly.GetConfig", (err, paths(m.log())))
    m.config(hostile="slow")
    ok, out, kind, err, ms = cli(st, "apply", "switch:0", '{"on":true}', deadline=2500)
    check("deadline: a device that stalls costs at most the 2.5 s given", not ok and kind == "no_answer" and ms < 3200, (kind, err, ms))
    ok, out, kind, err, ms = cli(st, "enumerate")
    check("deadline: enumerate gives up within its 4 s", not ok and ms < 4800, (err, ms))
    m.config(hostile="")
    ok, out, kind, err, ms = cli(st, "apply", "switch:0", '{"on":true}')
    check("after all that it still works", ok, err)
    m.stop()
    # A port nothing listens on: "did not answer" (the app then looks for the device elsewhere).
    st = new_state("closed", m, "shelly:shellyplus1pm-a8032ab12345")
    ok, out, kind, err, ms = cli(st, "enumerate")
    check("nothing at the port: did not answer", not ok and kind == "no_answer", (kind, err))


def review2():
    """Second review: what the first round left open."""
    # A relay or blind set up differently in the Shelly app after it was read (renamed "Garage
    # door", its device renamed, a one-second switch-off timer, a profile change): the same device
    # (same id or MAC), so the identity check passes; its settings are asked again and nothing is sent.
    for first, later, dev_id, thing, change, why in (
            ("plus1pm", "plus1-garage", "shelly:shellyplus1pm-a8032ab12345", "switch:0", '{"on":true}', "gen2 relay renamed Garage door"),
            ("plus1pm", "plus1-garage-dev", "shelly:shellyplus1pm-a8032ab12345", "switch:0", '{"on":false}', "gen2 device renamed Garage door (off too)"),
            ("plus1pm", "plus1-pulse", "shelly:shellyplus1pm-a8032ab12345", "switch:0", '{"on":true}', "gen2 relay given a 1 s auto-off"),
            ("plus2pm-cover", "plus2pm-cover-garage", "shelly:shellyplus2pm-a8032ab12345", "cover:0", '{"position":0}', "gen2 blind's device renamed Garage"),
            ("plus2pm", "plus2pm-cover", "shelly:shellyplus2pm-a8032ab12345", "switch:1", '{"on":true}', "gen2 2PM changed to the cover profile"),
            ("1pm", "1-garage-dev", "shelly:shelly1pm-a4cf12f45378", "relay:0", '{"on":true}', "gen1 device renamed Garage door"),
            ("1pm", "1-pulse", "shelly:shelly1pm-a4cf12f45378", "relay:0", '{"on":true}', "gen1 relay given a 1 s auto-off"),
            ("25-roller", "25-roller-garage", "shelly:shelly25-a4cf12f45378", "roller:0", '{"position":0}', "gen1 blind's device renamed Garage door"),
            ("25", "25-roller", "shelly:shelly25-a4cf12f45378", "relay:0", '{"on":true}', "gen1 2.5 changed to roller mode")):
        a = Mock(first)
        st = new_state("r2-stale-" + later, a, dev_id)
        ok, out, kind, err, ms = cli(st, "enumerate")
        a.stop()
        b = Mock(later)
        point_at(st, b)
        before = b.state()
        ok2, out2, kind2, err2, ms2 = cli(st, "apply", thing, change)
        check("review 2: settings asked again before a change: %s: not switched" % why,
              ok and thing in things_of(out) and not ok2 and kind2 != "other_device" and
              paths(b.log()) == ["/shelly", "Shelly.GetConfig" if b.model.startswith("plus") else "/settings"] and b.state() == before,
              (kind2, err2, paths(b.log())))
        b.stop()
    # A paired device that has never been read (it has nothing to offer: a garage door with a
    # password): its password is tied to the device that took it, so another Shelly at its address
    # later is sent nothing, its refusal is not kept, and typing the password again goes nowhere.
    for gen, garage, other, dev_id, user, pw_a, pw_b, extra in (
            (1, "1-garage-dev", "1pm", "shelly:shelly1-a4cf12f45378", "owner", "S3cret:Gen1", "wrong-Gen1", ["--mac", "A4CF12000000"]),
            (2, "plus1-garage-dev", "plus1pm", "shelly:shellyplus1pm-a8032ab12345", None, "hunter2-Gen2", "wrong-Gen2",
             ["--id", "shellyplus1pm-ffffffffffff", "--mac", "FFFFFFFFFFFF"])):
        a = Mock(garage, "--password", pw_a, *(["--user", user] if user else []))
        b = Mock(other, "--password", pw_b, *(["--user", user] if user else []), *extra)
        st = new_state("r2-anchor%d" % gen, a, dev_id)
        cli(st, "enumerate")
        if user:
            cli(st, "pair", user)
        okp, *_ = cli(st, "pair", pw_a)
        anchor = load(st)["device"]["auth"].get("dev")
        ok, out, kind, err, ms = cli(st, "enumerate")
        check("review 2 gen%d: pairing ties the password to the device that took it" % gen,
              okp and anchor == ("a4cf12f45378" if gen == 1 else "shellyplus1pm-a8032ab12345") and not ok and "nothing Zero Bot may use" in err,
              (anchor, err))
        point_at(st, b)
        b.reset()
        ok2, out2, kind2, err2, ms2 = cli(st, "enumerate")
        refused = load(st).get("secrets", {}).get("shelly", {}).get("refused", {})
        check("review 2 gen%d: never read, another Shelly at its address is sent no login" % gen,
              not ok2 and kind2 == "other_device" and logins(b.log()) == [] and paths(b.log()) == ["/shelly"], (kind2, err2, b.log()))
        check("review 2 gen%d: and no refusal is kept against it" % gen, refused == {}, refused)
        # Its owner changes its password; it is refused once, at its own address. Then the other
        # Shelly has the address when the new password is typed (Gen1: user name first).
        new_pw = "changed-Gen%d" % gen
        point_at(st, a)
        a.config(password=new_pw)
        ok3, out3, kind3, err3, ms3 = cli(st, "enumerate")
        point_at(st, b)
        b.reset()
        if user:
            cli(st, "pair", user)
        ok4, out4, kind4, err4, ms4 = cli(st, "pair", new_pw)
        check("review 2 gen%d: never read, the password typed again goes only to its own device" % gen,
              kind3 == "refused" and not ok4 and "Another device" in err4 and logins(b.log()) == [] and
              load(st)["device"]["auth"].get("dev") == anchor and load(st)["device"]["auth"].get("password") == new_pw,
              (kind3, err4, b.log(), load(st)["device"]["auth"].get("dev")))
        point_at(st, a)
        a.reset()
        ok4, out4, kind4, err4, ms4 = cli(st, "enumerate")
        check("review 2 gen%d: back at its address, it is asked with its own login" % gen,
              not ok4 and "nothing Zero Bot may use" in err4 and logins(a.log()) == ["ok", "ok"], (err4, a.log()))
        ok5, *_ = cli(st, "unpair")
        check("review 2 gen%d: unpair forgets which device it was" % gen, ok5 and load(st)["device"]["auth"] == {}, load(st)["device"]["auth"])
        a.stop()
        b.stop()
    # Read without a password while another Shelly, which has one, answers at the address: its 401
    # is not kept as "this one needs a password" (which would hide the device until a password it
    # does not have is typed); the app is told another device answers, and looks for it.
    a = Mock("plus1pm")
    b = Mock("plus1pm", "--password", "hunter2-Gen2", "--id", "shellyplus1pm-ffffffffffff", "--mac", "FFFFFFFFFFFF")
    st = new_state("r2-readlock", a, "shelly:shellyplus1pm-a8032ab12345")
    cli(st, "enumerate")
    point_at(st, b)
    ok, out, kind, err, ms = cli(st, "read")
    locked = load(st).get("secrets", {}).get("shelly", {}).get("locked", {})
    okst, step, *_ = cli(st, "step")
    check("review 2: read, no password, another (locked) Shelly at the address: 'another device', nothing kept",
          not ok and kind == "other_device" and locked == {} and step["kind"] == "none" and paths(b.log()) == ["/shelly"],
          (kind, err, locked, step, paths(b.log())))
    point_at(st, a)
    ok2, out2, kind2, err2, ms2 = cli(st, "read")
    check("review 2: back at its address it is read as before", ok2 and "switch:0" in things_of(out2), (kind2, err2))
    a.stop()
    b.stop()


# ================================================================ part 2: the app
class App:
    def __init__(self):
        self.state = tempfile.mkdtemp(prefix="zb-shelly-e2e-")
        self.env = dict(ENV, XDG_CONFIG_HOME=self.state + "/config", XDG_DATA_HOME=self.state + "/data",
                        XDG_CACHE_HOME=self.state + "/cache")
        self.file = self.state + "/data/zero-bot/smarthome.json"

    def run(self, *args):
        p = subprocess.run([APP, *args], env=self.env, capture_output=True, text=True, timeout=60)
        outputs.append(p.stdout + p.stderr)
        return p.stdout

    def tool(self, args):
        return self.run("--tool", "smart_home", json.dumps(args)).strip()

    def load(self):
        with open(self.file) as f:
            return json.load(f)

    def save(self, root):
        os.makedirs(os.path.dirname(self.file), exist_ok=True)
        with open(self.file, "w") as f:
            json.dump(root, f)


def app_part():
    if not os.access(APP, os.X_OK):
        print("info  %s not built: the app part was skipped" % APP)
        return
    lamp, blind, kettle = Mock("plus1pm"), Mock("plus2pm-cover"), Mock("plugs", "--password", "hunter2-Gen2")
    mocks_here = []
    app = App()
    try:
        out = app.run("--home-refresh")
        network = next((l.split(" id ")[1].strip() for l in out.splitlines() if l.startswith("network: Test network, id ")), "")
        check("app: runs on the stand-in test network", network.startswith("net-"), out)

        def dev(id_, mock, name):
            return {"id": id_, "kind": "shelly", "name": name, "last_ip": "127.0.0.1", "port": mock.port, "enabled": True, "things": [],
                    "found_by": ["mdns"]}
        app.save({"version": 2, "secrets": {}, "networks": {network: {
            "label": "Test network", "gateway_mac": "02:00:00:00:00:00", "allowed_at": 1, "last_scan": 1, "devices": [
                dev("shelly:shellyplus1pm-a8032ab12345", lamp, "shellyplus1pm-a8032ab12345"),
                dev("shelly:shellyplus2pm-a8032ab12345", blind, "shellyplus2pm-a8032ab12345"),
                dev("shelly:shellyplusplugs-a8032ab12345", kettle, "shellyplusplugs-a8032ab12345")]}}})
        out = app.run("--home-refresh")
        check("app: refresh reads the two open devices; the third wants its password", "2 ready, 1 not" in out and "password" in out, out)
        root = app.load()
        devs = {d["id"]: d for d in root["networks"][network]["devices"]}
        lamp_things = [t["id"] for t in devs["shelly:shellyplus1pm-a8032ab12345"]["things"]]
        check("app: the lamp's and the blind's things are saved", lamp_things == ["switch:0"] and
              [t["id"] for t in devs["shelly:shellyplus2pm-a8032ab12345"]["things"]] == ["cover:0"], devs)
        check("app: with no name from its owner, a device keeps the name it announced",
              devs["shelly:shellyplus1pm-a8032ab12345"]["name"] == "shellyplus1pm-a8032ab12345", devs["shelly:shellyplus1pm-a8032ab12345"]["name"])
        check("app: the blind is named as in the Shelly app",
              devs["shelly:shellyplus2pm-a8032ab12345"]["things"][0]["name"] == "Lounge blind", devs["shelly:shellyplus2pm-a8032ab12345"]["things"])
        check("app: the password-protected plug was asked only /shelly", paths(kettle.log()) == ["/shelly"] and logins(kettle.log()) == [], paths(kettle.log()))
        check("app: and is remembered as needing one", root.get("secrets", {}).get("shelly", {}).get("locked", {}).get("shelly:shellyplusplugs-a8032ab12345") == 2,
              root.get("secrets"))
        lamp.reset()
        r = app.tool({"action": "on", "target": "Desk lamp"})
        check("app: 'turn on the desk lamp' reaches the device as Switch.Set", not r.startswith("ERROR") and
              paths(lamp.log()) == ["/shelly", "Shelly.GetConfig", "Switch.Set"] and lamp.state()["switch:0"]["output"] is True,
              (r, paths(lamp.log())))
        blind.reset()
        r = app.tool({"action": "set", "target": "Lounge blind", "position": 30})
        check("app: 'set the lounge blind to 30' is Cover.GoToPosition 30", not r.startswith("ERROR") and paths(blind.log())[-1] == "Cover.GoToPosition" and
              blind.log()[-1]["rpc"]["params"] == {"id": 0, "pos": 30}, (r, paths(blind.log())))
        blind.reset()
        r = app.tool({"action": "off", "target": "Lounge blind"})
        check("app: 'close the lounge blind' (off) is Cover.Close", not r.startswith("ERROR") and paths(blind.log())[-1] == "Cover.Close", (r, paths(blind.log())))
        lamp.reset()
        r = app.tool({"action": "status", "target": "Desk lamp"})
        check("app: the status of the lamp comes from the device", '"on":true' in r.replace(" ", "") and "12.5" in r and
              paths(lamp.log()) == ["/shelly", "Shelly.GetStatus"], (r, paths(lamp.log())))
        check("app: results carry no address, id or MAC", "127.0.0.1" not in r and "a8032ab12345" not in r.lower(), r)
        # review 2: since it was read, the desk lamp's Shelly has been set up as a garage door
        # opener in the Shelly app (same device). Nothing has read it again; it is still offered
        # under its old name, but asked again, it is not switched.
        lamp.stop()
        garage = Mock("plus1-garage")
        mocks_here.append(garage)
        root = app.load()
        for d in root["networks"][network]["devices"]:
            if d["id"] == "shelly:shellyplus1pm-a8032ab12345":
                d["port"] = garage.port
        app.save(root)
        r = app.tool({"action": "on", "target": "Desk lamp"})
        check("app: a lamp since set up as a garage door is not switched (its settings asked again)",
              r.startswith("ERROR") and "does not control" in r and paths(garage.log()) == ["/shelly", "Shelly.GetConfig"] and
              garage.state()["switch:0"]["output"] is False, (r, paths(garage.log())))
    finally:
        for m in (lamp, blind, kettle, *mocks_here):
            m.stop()
        shutil.rmtree(app.state, ignore_errors=True)


def main():
    if not os.access(CLI, os.X_OK):
        sys.exit("no %s (run tests/run_unit.sh first)" % CLI)
    p = subprocess.run([sys.executable, "-I", MOCK, "--selftest"], capture_output=True, text=True, timeout=30)
    check("the mock's digest code reproduces RFC 7616's SHA-256 and MD5 examples", p.returncode == 0, p.stdout + p.stderr)
    try:
        gen2_open()
        gen2_password()
        gen1_password()
        covers()
        lights()
        sensors_and_exclusions()
        identity_and_hostile()
        review2()
        app_part()
    finally:
        for m in mocks:
            m.stop()
        setup = [(m.model, r["method"], r["path"], (r.get("rpc") or {}).get("method")) for m in mocks for r in m.seen if r.get("setup")]
        check("no device was ever sent a calibration, setup, firmware or reboot call", setup == [], setup)
        leaked = [p for p in PASSWORDS if any(p in o for o in outputs)]
        check("no password was ever printed", leaked == [], leaked)
        total = sum(len(m.seen) for m in mocks)
        print("info  %d requests reached the simulated devices" % total)
        shutil.rmtree(TMP, ignore_errors=True)
    print("shelly e2e: %d checks, %d failed" % (run_count, fail_count))
    return 1 if fail_count else 0


if __name__ == "__main__":
    sys.exit(main())
