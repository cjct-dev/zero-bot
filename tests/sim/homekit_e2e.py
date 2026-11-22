#!/usr/bin/env python3
"""Runs homekit_cli against mock_homekit.py on 127.0.0.1 and checks what the mock saw.

usage: tests/sim/homekit_e2e.py [path/to/homekit_cli]   (default: build/unit/homekit_cli, which tests/unit/run_unit.sh builds)

Prints one line per check and exits non-zero when any fails. Nothing outside 127.0.0.1 is
contacted. Every mock binds port 0 and says which port it got; every mock is killed when this
script ends (and exits by itself when this script's end of its stdin closes). The counters come
from the mock (GET /mock/stats), so "the code was sent once" and "nothing was sent" are judged
from the accessory's side, not from what the driver says about itself.
"""
import atexit
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

TESTS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # tests/: netguard.py, mocks/
sys.path.insert(0, TESTS)
import netguard   # noqa: E402
netguard.sanitize()   # no key or proxy of the caller's; the app pinned (ZB_EXP_NET) and on the stand-in network

CLI = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(TESTS, "..", "build", "unit", "homekit_cli")
CODE, WRONG, WRONG2 = "123-45-678", "123-45-679", "111-22-333"
TMP = tempfile.mkdtemp(prefix="homekit-e2e-")
NO_PROXY = urllib.request.build_opener(urllib.request.ProxyHandler({}))

# The state keys the app shows (src/smarthome/home.h; control.cpp shown_state() drops every other
# key before the model sees it), and the driver's own keys, documented in homekit.h.
APP_KEYS = {"on", "brightness", "kelvin", "hue", "saturation", "mode", "target_c", "current_c", "humidity", "position",
            "fan", "volume", "muted", "playing", "source", "title", "power_w", "energy_today_wh", "motion", "contact"}
OWN_KEYS = {"in_use", "heating", "target_position", "reachable"}
# What the app counts as "it did not answer" (control.cpp unreachable()): such an error costs a
# 2.5 s rescan of the network.
UNREACHABLE_WORDS = ("connect", "reach", "answer", "respond", "timed out", "timeout", "no route", "unreachable")


def shown(state):
    """What the app lets the model see of a thing's state."""
    return {k: v for k, v in state.items() if k in APP_KEYS}

run_count = fail_count = 0
outputs = []   # everything the CLI printed, to check that no secret ever appears
mocks = []


def check(name, ok, detail=""):
    global run_count, fail_count
    run_count += 1
    fail_count += not ok
    print("%s  %s%s" % ("ok  " if ok else "FAIL", name, "" if ok or detail == "" else "  -> %s" % (detail,)), flush=True)


def info(text):
    print("info  %s" % text, flush=True)


class Mock:
    def __init__(self, *extra):
        self.proc = subprocess.Popen([sys.executable, "-I", os.path.join(TESTS, "mocks", "homekit", "mock_homekit.py"), "--port", "0", *extra],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, start_new_session=True, env=netguard.mock_env())
        mocks.append(self)
        line = self.proc.stdout.readline().strip()
        if not line.startswith("MOCK listening on 127.0.0.1:"):
            raise RuntimeError("mock did not start: %r" % line)
        self.port = int(line.rsplit(":", 1)[1])

    def _call(self, path, body=None):
        req = urllib.request.Request("http://127.0.0.1:%d%s" % (self.port, path),
                                     data=None if body is None else json.dumps(body).encode(),
                                     method="GET" if body is None else "POST")
        with NO_PROXY.open(req, timeout=10) as r:
            return json.load(r)

    def stats(self):
        return self._call("/mock/stats")

    def reset(self, **cfg):
        self._call("/mock/reset", cfg)

    def config(self, **cfg):
        self._call("/mock/config", cfg)

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
    """One saved device and the secrets, as the app's store would hold them."""
    count = 0

    def __init__(self, port, **discovery):
        State.count += 1
        self.path = os.path.join(TMP, "state%d.json" % State.count)
        info_kv = {"sf": "1", "ci": "2", "id": "AA:BB:CC:DD:EE:01"}
        info_kv.update(discovery)
        rc, _, err, _ = self.run("init", "127.0.0.1", str(port), *("%s=%s" % kv for kv in info_kv.items()))
        if rc != 0:
            raise RuntimeError("init failed: %r" % err)

    def run(self, *args, timeout=60):
        """Runs the CLI; returns (exit status, parsed stdout lines, stderr lines, seconds). The
        process is reaped with wait4 so that its own peak memory is known (self.last_rss_kb)."""
        with tempfile.TemporaryFile() as fo, tempfile.TemporaryFile() as fe:
            t0 = time.monotonic()
            p = subprocess.Popen([CLI, self.path, *args], stdout=fo, stderr=fe)
            timer = threading.Timer(timeout, p.kill)
            timer.start()
            _, status, usage = os.wait4(p.pid, 0)
            timer.cancel()
            p.returncode = os.waitstatus_to_exitcode(status)
            elapsed = time.monotonic() - t0
            fo.seek(0)
            fe.seek(0)
            stdout, stderr = fo.read().decode("utf-8", "replace"), fe.read().decode("utf-8", "replace")
        self.last_rss_kb = usage.ru_maxrss
        if p.returncode < 0:
            stderr += "\nhomekit_cli died of signal %d" % -p.returncode
        outputs.append(stdout + stderr)
        out = []
        for line in stdout.splitlines():
            try:
                out.append(json.loads(line))
            except ValueError:
                out.append(line)
        return p.returncode, out, stderr.splitlines(), elapsed

    def load(self):
        with open(self.path) as f:
            return json.load(f)

    def auth(self):
        return self.load()["device"]["auth"]

    def things(self):
        return {t["id"]: t for t in self.load()["device"]["things"]}


def setup_payload(code, category, setup_id):
    """X-HM:// text for a code, laid out as HomeKitADK's HAPAccessorySetup.c does (IP flag set)."""
    v = (category << 31) | (1 << 28) | int(code.replace("-", ""))
    digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    s = ""
    for _ in range(9):
        v, r = divmod(v, 36)
        s = digits[r] + s
    return "X-HM://" + s + setup_id


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def err_has(err, words):
    return len(err) >= 1 and words in err[0]


# ============================================================================================
def test_pairing_and_control():
    m = Mock()
    st = State(m.port)

    rc, out, err, _ = st.run("step")
    check("unpaired: the step asks for the 8-digit code", rc == 0 and out[0]["kind"] == "EnterCode" and
          "8 digits" in out[0]["prompt"] and out[0]["field"] == "Setup code", (rc, out, err))
    rc, out, err, _ = st.run("pair", "12-34")
    s = m.stats()
    check("a malformed code is refused and nothing reaches the accessory",
          rc == 1 and err_has(err, "not a setup code") and s["connections"] == 0, (rc, err, s["connections"]))

    # Wrong code: sent once, never again from this driver object.
    rc, out, err, _ = st.run("pair", WRONG, "--repeat", "3")
    s = m.stats()
    check("a wrong code fails with the accessory's refusal", rc == 1 and len(err) == 3 and
          err_has(err, "did not accept that setup code"), (rc, err))
    check("the wrong code reached the accessory once and was not sent again",
          s["setup_m3"] == 1 and s["auth_failures"] == 1 and s["setup_m1"] == 1 and
          all("not sent again" in e for e in err[1:]), (s["setup_m1"], s["setup_m3"], err))
    check("the code itself appears in no message", all(WRONG not in e and WRONG.replace("-", "") not in e for e in err))
    rc, out, err, _ = st.run("pair", WRONG2)
    s = m.stats()
    check("another code typed by the user is tried once", rc == 1 and s["setup_m3"] == 2 and s["auth_failures"] == 2,
          (rc, err, s["setup_m3"]))

    # The right code, as the text of a scanned QR code.
    payload = setup_payload(CODE, 2, "ZB01")
    rc, out, err, _ = st.run("code", payload, CODE)
    check("the X-HM:// text of the mock's code reads as the code (the CLI says so without printing it)",
          rc == 0 and out[0] == {"matches": True}, (rc, out, err))
    rc, out, err, el = st.run("pair", payload)
    s = m.stats()
    a = st.auth()
    secrets = st.load()["secrets"]["homekit"]
    check("pairing with the right code succeeds", rc == 0 and out[0]["paired"] is True, (rc, out, err))
    check("the state file holding the controller's key can be read by its owner only",
          os.stat(st.path).st_mode & 0o777 == 0o600, oct(os.stat(st.path).st_mode & 0o777))
    check("the accessory now has exactly this controller, with its key",
          list(s["pairings"].keys()) == [secrets["controller_id"]] and s["pairings"][secrets["controller_id"]] == secrets["ltpk"],
          (s["pairings"], secrets.get("controller_id")))
    check("the device keeps the accessory's id and key, and the controller id",
          a.get("accessory_id") == s["accessory_id"] and a.get("accessory_ltpk") == s["accessory_ltpk"] and
          a.get("controller_id") == secrets["controller_id"], a)
    check("pairing: the accessory saw nothing wrong", s["bad"] == [] and s["setup_ok"] == 1, s["bad"])
    info("pair-setup against the pure-Python mock took %.0f ms (process start included)" % (el * 1000))
    rc, out, err, _ = st.run("step")
    check("paired: no step left", rc == 0 and out[0]["kind"] == "None", out)
    m1, v1 = s["setup_m1"], s["verify_ok"]
    rc, out, err, _ = st.run("pair", CODE)
    s = m.stats()
    check("pairing again only checks the pairing with the accessory: no pair-setup, no code sent",
          rc == 0 and s["setup_m1"] == m1 and s["verify_ok"] == v1 + 1, (rc, err, s["setup_m1"], s["verify_ok"]))

    # Things.
    verified = m.stats()["verify_ok"]
    rc, out, err, el = st.run("enumerate")
    things = {t["id"]: t for t in out[0]["things"]} if rc == 0 else {}
    want = {"2.10", "3.10", "4.10", "7.10", "8.10", "9.10", "9.20", "10.10", "11.10", "13.10", "14.10", "18.10", "19.20",
            "20.10", "21.10", "21.20", "21.30"}
    check("enumerate lists the allowed services", rc == 0 and set(things) == want, (rc, err, sorted(things)))
    check("no lock, garage, valve, alarm, doorbell, vendor service or hidden light became a thing",
          not any(t.split(".")[0] in ("5", "6", "12", "15", "16", "17") for t in things) and "19.10" not in things,
          sorted(things))
    info("enumerate took %.0f ms" % (el * 1000))
    names = {k: v["name"] for k, v in things.items()}
    check("names come from the accessories, prefixed for multi-socket strips and sensors",
          names.get("2.10") == "Hall Lamp" and names.get("9.10") == "Hall Sensor Temperature" and
          names.get("21.10") == "Strip Left" and names.get("21.30") == "Strip 3" and names.get("19.20") == "Desk", names)
    study = names.get("18.10", "")
    check("an untrusted name is cleaned: no control or invalid bytes, at most 40 characters",
          study.startswith("Study Lamp with a name") and len(study) <= 40 and study.isprintable(), repr(study))
    lamp, heat, heater = things.get("2.10", {}), things.get("4.10", {}), things.get("13.10", {})
    check("the lamp: light with power, brightness, white and colour, and its state",
          lamp.get("type") == "light" and lamp.get("caps") == 15 and lamp.get("state") ==
          {"on": False, "brightness": 40, "hue": 30.0, "saturation": 50.0, "kelvin": 2703}, lamp)
    check("the thermostat: target, mode and reading, under the keys the app shows",
          shown(heat.get("state", {})) == {"target_c": 20.0, "current_c": 19.5, "mode": "heat"}, heat.get("state"))
    check("the heater asks before it is switched on", heater.get("ask_first") is True and heater.get("caps") == 8225, heater)
    check("sensors as the app shows them: temperature, humidity, motion, contact",
          shown(things.get("9.10", {}).get("state", {})) == {"current_c": 18.3} and
          shown(things.get("9.20", {}).get("state", {})) == {"humidity": 55} and
          shown(things.get("10.10", {}).get("state", {})) == {"motion": False} and
          shown(things.get("11.10", {}).get("state", {})) == {"contact": "open"},
          {k: things.get(k, {}).get("state") for k in ("9.10", "9.20", "10.10", "11.10")})
    strays = sorted("%s:%s" % (k, key) for k, t in things.items() for key in t.get("state", {})
                    if key not in APP_KEYS and key not in OWN_KEYS)
    check("every state key is one the app shows or one of the driver's own", not strays, strays)
    s = m.stats()
    check("enumerate: one session, one pair-verify, no protocol fault", s["bad"] == [] and s["verify_ok"] == verified + 1 and
          ["GET", "/accessories"] in s["requests"], (s["bad"], s["verify_ok"] - verified))

    # Read.
    rc, out, err, _ = st.run("read")
    s = m.stats()
    got = {t["id"]: t["state"] for t in out[0]["things"]} if rc == 0 else {}
    check("read refreshes every thing", rc == 0 and set(got) == want and got["11.10"] == {"contact": "open"} and
          shown(got["4.10"]) == {"target_c": 20.0, "current_c": 19.5, "mode": "heat"}, (rc, err, got.get("11.10")))
    asked = ",".join(p.split("id=", 1)[1] for p in s["gets"])
    check("read asks only for readable characteristics, in batches", "2.11" in asked and "4.13" in asked and
          "2.2" not in asked.split(",") and len(s["gets"]) >= 1, s["gets"])

    # Writes, checked on the accessory.
    def apply(*args):
        rc, out, err, el = st.run(*args)
        return rc, err, m.stats()

    rc, err, s = apply("apply", "2.10", "on=true", "brightness=55")
    check("light on at 55%", rc == 0 and s["values"]["2.11"] is True and s["values"]["2.12"] == 55 and s["bad"] == [],
          (rc, err, s["bad"]))
    rc, err, s = apply("apply", "2.10", "kelvin=2700")
    check("light to 2700 K arrives as 370 mireds, an integer", rc == 0 and s["values"]["2.15"] == 370 and
          isinstance(s["values"]["2.15"], int), (rc, err, s["values"].get("2.15")))
    rc, err, s = apply("apply", "2.10", "hs=120,50")
    check("light to hue 120, saturation 50", rc == 0 and s["values"]["2.13"] == 120 and s["values"]["2.14"] == 50, (rc, err))
    rc, err, s = apply("apply", "4.10", "target=20.3")
    check("heating to 20.3 C arrives as 20.5 (the thermostat's step)", rc == 0 and s["values"]["4.14"] == 20.5, (rc, err))
    rc, err, s = apply("apply", "4.10", "target=35")
    check("heating to 35 C is clamped to 30", rc == 0 and s["values"]["4.14"] == 30.0, (rc, err))
    rc, err, s = apply("apply", "4.10", "mode=auto")
    check("heating mode auto", rc == 0 and s["values"]["4.12"] == 3, (rc, err))
    puts = len(s["puts"])
    rc, err, s = apply("apply", "4.10", "mode=cool")
    check("mode cool is refused and nothing is sent", rc == 1 and err_has(err, "not a heating mode") and len(s["puts"]) == puts,
          (rc, err))
    rc, err, s = apply("apply", "7.10", "position=40")
    check("blind to 40%", rc == 0 and s["values"]["7.12"] == 40, (rc, err))
    rc, err, s = apply("apply", "8.10", "fan=50")
    check("a three-speed fan at 50% runs at speed 2", rc == 0 and s["values"]["8.11"] == 1 and s["values"]["8.12"] == 2,
          (rc, err, s["values"].get("8.12")))
    rc, err, s = apply("apply", "13.10", "mode=heat")
    check("the heater: active, heating", rc == 0 and s["values"]["13.11"] == 1 and s["values"]["13.13"] == 1, (rc, err))
    puts = len(s["puts"])
    rc, err, s = apply("apply", "9.10", "on=true")
    check("a sensor cannot be switched; nothing sent", rc == 1 and err_has(err, "is a sensor") and len(s["puts"]) == puts, err)
    rc, err, s = apply("apply", "2.10", "volume=10")
    check("volume on a light is refused; nothing sent", rc == 1 and err_has(err, "cannot do that") and len(s["puts"]) == puts, err)
    gets_before = len(s["gets"])
    rc, err, s = apply("apply-many", "3.10,14.10,21.10,21.20", "on=false")
    last = s["puts"][-1]["characteristics"] if s["puts"] else []
    check("apply_many: four things, one PUT", rc == 0 and len(s["puts"]) == puts + 1 and len(last) == 4 and
          all(c["value"] is False for c in last) and s["values"]["3.11"] is False, (rc, err, last))
    check("writes: every value in its declared format and range", s["bad"] == [], s["bad"])
    checked = [p for p in s["gets"][gets_before:] if "type=1" in p]
    asked = ",".join(p.split("id=", 1)[1].split("&")[0] for p in checked)
    check("before writing, the ids to be written are read back with their types, with each accessory's model, "
          "serial number and firmware; the database is not fetched again",
          len(checked) == 1 and all(i in asked.split(",") for i in ("3.11", "14.11", "21.11", "21.21", "3.4", "3.6", "3.7")) and
          ["GET", "/accessories"] not in s["requests"][-3:], (checked, s["requests"][-3:]))
    rc, out, err, _ = st.run("read", "2.10", "4.10")
    got = {t["id"]: t["state"] for t in out[0]["things"]} if rc == 0 else {}
    check("read after the writes shows them", rc == 0 and got.get("2.10", {}).get("on") is True and
          got["2.10"].get("brightness") == 55 and got.get("4.10", {}).get("target_c") == 30.0 and
          got["4.10"].get("mode") == "auto", got)

    # Pair-verify cost.
    rc, out, err, _ = st.run("time-verify", "5")
    if rc == 0:
        ms = sorted(out[0]["ms"])
        info("pair-verify round trip against the pure-Python mock: median %.1f ms (its Ed25519 is slow Python)" % ms[2])
    check("pair-verify five times in a row", rc == 0 and m.stats()["verify_ok"] >= 5, (rc, err))

    # Unpair, after a scan made while paired recorded sf=0 ("has a controller").
    set_info(st, "sf", "0")
    ctl = st.load()["secrets"]["homekit"]["controller_id"]
    rc, out, err, _ = st.run("unpair")
    s = m.stats()
    check("unpair removes this controller from the accessory", rc == 0 and out[0]["paired"] is False and
          s["removed"] == [ctl] and s["pairings"] == {}, (rc, out, s["removed"], s["pairings"]))
    check("unpair forgets the pairing on this side", "accessory_id" not in st.auth(), st.auth())
    check("... and the saved discovery record no longer says the accessory has a controller (a scan made while "
          "paired stored sf=0)", st.load()["device"]["info"].get("sf") in (None, "1"), st.load()["device"]["info"])
    rc, out, err, _ = st.run("enumerate")
    check("after unpair, enumerate says to pair first", rc == 1 and err_has(err, "not paired"), err)
    rc, out, err, _ = st.run("pair", CODE)
    s = m.stats()
    check("the accessory can be paired again, with the same controller key",
          rc == 0 and list(s["pairings"]) == [ctl] and st.load()["secrets"]["homekit"]["controller_id"] == ctl, (rc, err))
    secret = st.load()["secrets"]["homekit"]["ltsk"]
    check("the controller's private key never appears in anything the CLI printed",
          len(secret) == 64 and not any(secret in o for o in outputs))
    m.stop()


# ============================================================================================
def test_big_bridge():
    m = Mock("--many", "60")
    st = State(m.port)
    rc, out, err, _ = st.run("pair", CODE)
    rc, out, err, el = st.run("enumerate")
    n = len(out[0]["things"]) if rc == 0 else 0
    s = m.stats()
    check("a bridge of 81 accessories: every light listed", rc == 0 and n == 17 + 60, (rc, n, err))
    info("enumerate of the big bridge took %.0f ms" % (el * 1000))
    frames = s["frames_in"]
    extra = ",".join("%d.10" % (100 + i) for i in range(60))
    rc, out, err, _ = st.run("apply-many", extra, "brightness=30")
    s = m.stats()
    check("120 writes in one PUT go out as several frames of at most 1024 bytes",
          rc == 0 and s["frames_in"] - frames >= 4 and s["max_frame_in"] == 1024 and len(s["puts"][-1]["characteristics"]) == 120,
          (rc, err, s["frames_in"] - frames, s["max_frame_in"]))
    rc, out, err, _ = st.run("read")
    s = m.stats()
    check("reading 77 things takes several GETs in one session", rc == 0 and len(s["gets"]) >= 5 and s["bad"] == [],
          (rc, err, len(s["gets"]), s["bad"]))
    m.stop()


# ============================================================================================
def paired_state(m, **discovery):
    st = State(m.port, **discovery)
    rc, out, err, _ = st.run("pair", CODE)
    if rc != 0:
        raise RuntimeError("pairing for a test failed: %r" % err)
    rc, out, err, _ = st.run("enumerate")
    if rc != 0:
        raise RuntimeError("enumerate for a test failed: %r" % err)
    return st


def saved_at(port, paired):
    """`paired`'s pairing, things and controller key, saved for a device at another port (in a
    file of its own): what the app holds when something else now answers at the address."""
    st = State(port)
    d = st.load()
    d["device"]["auth"], d["device"]["things"], d["secrets"] = paired.auth(), paired.load()["device"]["things"], paired.load()["secrets"]
    with open(st.path, "w") as f:
        json.dump(d, f)
    return st


def setup_requests(m):
    """How many pair-setup requests the mock received (counted before it looks at them)."""
    return sum(1 for r in m.stats()["requests"] if r[1] == "/pair-setup")


def test_pairing_failures():
    m = Mock("--paired-with-other")
    st = State(m.port)
    rc, out, err, _ = st.run("pair", CODE)
    s = m.stats()
    check("paired with another controller (discovery out of date): refused clearly, the code never sent",
          rc == 1 and err_has(err, "already paired with another controller") and s["setup_m3"] == 0, (rc, err, s["setup_m3"]))
    m.stop()

    m = Mock()
    st = State(m.port, sf="0")
    rc, out, err, _ = st.run("step")
    check("discovery says paired elsewhere: the step says so", rc == 0 and "already paired" in out[0]["prompt"], out)
    rc, out, err, _ = st.run("pair", CODE)
    check("discovery says paired elsewhere: refused without contacting the accessory",
          rc == 1 and err_has(err, "already paired") and m.stats()["connections"] == 0, (rc, err))

    m.reset(max_tries=1)
    st = State(m.port)
    st.run("pair", WRONG)
    rc, out, err, _ = st.run("pair", CODE, "--repeat", "2")
    s = m.stats()
    check("after too many wrong codes the accessory locks: reported, and not asked again",
          rc == 1 and len(err) == 2 and all("too many wrong codes" in e for e in err) and s["setup_m1"] == 2 and
          s["setup_m3"] == 1, (rc, err, s["setup_m1"], s["setup_m3"]))

    m.reset(backoff_after=1, backoff_seconds=30)
    st = State(m.port)
    st.run("pair", WRONG)
    rc, out, err, _ = st.run("pair", CODE, "--repeat", "2")
    s = m.stats()
    check("back-off: the accessory's delay is reported and respected without asking again",
          rc == 1 and len(err) == 2 and "wait 30 seconds" in err[0] and "Try again in" in err[1] and s["setup_m1"] == 2 and
          s["setup_m3"] == 1, (rc, err, s["setup_m1"]))

    for fault, words, what in [("busy", "busy pairing", "busy"), ("max_peers", "cannot take another controller", "full"),
                               ("fake_m4", "could not prove", "an impostor without the code"),
                               ("bad_m6_sig", "identity could not be checked", "a bad accessory signature"),
                               ("garbage_tlv", "not understood", "a garbled TLV")]:
        m.reset(faults=[fault])
        st = State(m.port)
        rc, out, err, _ = st.run("pair", CODE)
        check("pair-setup against %s: \"%s\", nothing stored" % (what, words),
              rc == 1 and err_has(err, words) and "accessory_id" not in st.auth(), (rc, err))
    m.stop()

    # B with a leading zero byte, sent stripped (aiohomekit's case: zero salt, b = 1336).
    m = Mock("--salt", "00" * 16, "--b", "1336")
    st = State(m.port)
    rc, out, err, _ = st.run("pair", CODE)
    check("pairs with an accessory whose B arrives one byte short", rc == 0 and m.stats()["setup_ok"] == 1, (rc, err))
    m.stop()


def fresh_pairing(m, st, faults=()):
    """Clears the mock (pairings, faults, values), pairs this controller again, then sets faults."""
    m.reset()
    d = st.load()
    d["device"]["auth"] = {}
    with open(st.path, "w") as f:
        json.dump(d, f)
    rc, out, err, _ = st.run("pair", CODE)
    if rc != 0:
        raise RuntimeError("re-pairing for a test failed: %r" % err)
    if faults:
        m.config(faults=list(faults))


def test_session_failures():
    m = Mock()
    st = paired_state(m)

    # Nothing listening at the saved port.
    gone = State(free_port())
    d = gone.load()
    d["device"]["auth"], d["device"]["things"], d["secrets"] = st.auth(), st.load()["device"]["things"], st.load()["secrets"]
    with open(gone.path, "w") as f:
        json.dump(d, f)
    rc, out, err, el = gone.run("apply", "2.10", "on=true")
    check("nothing listening: \"could not be reached\" at once, well before the 2.5 s deadline",
          rc == 1 and err_has(err, "could not be reached") and el < 2.0, (rc, err, el))

    # An accessory that accepts the connection and then says nothing: the deadline holds.
    for fault, cmd, words, limit in [
            ("silent", ("apply", "2.10", "on=true"), "did not answer in time", 2.5),
            ("silent", ("enumerate",), "did not answer in time", 4.0),
            ("stall_after_verify", ("apply", "2.10", "on=true"), "did not answer in time", 2.5),
            ("stall_after_verify", ("read",), "did not answer in time", 4.0)]:
        fresh_pairing(m, st, [fault])
        rc, out, err, el = st.run(*cmd)
        # The slack above the deadline is for starting the process on a busy machine; a driver
        # that ignored its deadline would wait for the 60 s cap instead.
        check("%s, %s: \"%s\" within its %.1f s deadline (took %.2f s)" % (fault, cmd[0], words, limit, el),
              rc == 1 and err_has(err, words) and limit - 0.2 <= el <= limit + 1.5, (rc, err, el))

    # Hostile or broken replies.
    peak = 0
    for fault, cmd, words in [
            ("drop_after_verify", ("apply", "2.10", "on=true"), "closed the connection"),
            ("garbage_plain", ("enumerate",), "not understood"),
            ("garbage_tlv", ("enumerate",), "not understood"),
            ("huge_content_length", ("enumerate",), "not understood"),
            ("huge_frame", ("enumerate",), "could not be decrypted"),
            ("truncated_frame", ("enumerate",), "closed the connection"),
            ("bad_tag", ("read",), "could not be decrypted"),
            ("deep_json", ("enumerate",), "not understood"),
            ("impostor", ("apply", "2.10", "on=true"), "identity check failed"),
            ("verify_unknown", ("read",), "no longer recognises this app"),
            ("status_207", ("apply", "2.10", "on=true"), "Hall Lamp is switched off, or out of range of its bridge"),
            ("status_huge", ("apply", "2.10", "on=true"), "Hall Lamp reported an error"),
            ("http_500", ("apply", "2.10", "on=true"), "internal error")]:
        fresh_pairing(m, st, [fault])
        rc, out, err, el = st.run(*cmd)
        peak = max(peak, st.last_rss_kb)
        s = m.stats()
        ok = rc == 1 and err_has(err, words) and el < 5
        if fault == "impostor":   # nothing at all may be sent to an accessory that failed the identity check
            ok = ok and not s["puts"] and not any(r[1] in ("/accessories", "/characteristics") for r in s["requests"])
        if fault in ("status_207", "status_huge"):   # the accessory answered: nothing that reads as "it did not"
            ok = ok and not any(w in err[0].lower() for w in UNREACHABLE_WORDS)
        check("%s: \"%s\", no crash, quickly" % (fault, words), ok, (rc, err, round(el, 2)))
    if os.environ.get("HOMEKIT_E2E_SANITIZED"):
        info("memory not checked: a sanitizer build keeps large shadow maps (largest %.1f MB)" % (peak / 1024.0))
    else:
        check("hostile replies never made the CLI grow beyond 32 MB (largest: %.1f MB)" % (peak / 1024.0), 0 < peak < 32 * 1024)

    fresh_pairing(m, st, ["wrong_types"])
    rc, out, err, _ = st.run("enumerate")
    check("an accessory database with every type wrong: no crash, nothing bogus",
          rc in (0, 1) and (rc == 1 or len(out[0]["things"]) <= 1), (rc, out, err))

    fresh_pairing(m, st, ["lone_surrogate"])
    rc, out, err, _ = st.run("enumerate")
    names = {t["id"]: t["name"] for t in out[0]["things"]} if rc == 0 else {}
    check("one name with a lone surrogate escape (\"Porch \\ud83d Switch\") does not make the bridge unreadable: every "
          "thing listed, that name cleaned", rc == 0 and len(names) == 17 and names.get("14.10") == "Porch Switch", (rc, err, names.get("14.10")))

    # A write answered 207 (Multi-Status) that says nothing usable about the items: never success,
    # and never an empty reason (which the app reads as "apply each thing alone").
    for fault in ("put_207_empty", "put_207_no_list"):
        fresh_pairing(m, st)
        st.run("enumerate")   # the things as they are (an earlier hostile database may have replaced them)
        m.config(faults=[fault])
        rc, out, err, _ = st.run("apply", "2.10", "on=true")
        rc2, out2, err2, _ = st.run("apply-many", "2.10,3.10", "on=true")
        check("%s: apply and apply_many say the reply was not understood (not \"ok\", not \"each alone\")" % fault,
              rc == 1 and err_has(err, "not understood") and rc2 == 1 and err_has(err2, "not understood") and
              not each_of(out2), (rc, err, rc2, out2, err2))

    for fault in ("chunked", "event_first"):
        fresh_pairing(m, st, [fault])
        rc, out, err, _ = st.run("enumerate")
        n = len(out[0]["things"]) if rc == 0 else 0
        rc2, out2, err2, _ = st.run("apply", "2.10", "on=true")
        check("%s answers are read correctly" % fault.replace("_", " "), rc == 0 and n == 17 and rc2 == 0, (rc, err, rc2, err2))

    # Instance ids change (as after a firmware update): a clear request to refresh, then it works.
    fresh_pairing(m, st)
    st.run("enumerate")
    m.config(faults=["renumber"])
    rc, out, err, _ = st.run("apply", "2.10", "on=true")
    check("after the accessory's ids change, a write asks for a refresh", rc == 1 and err_has(err, "Refresh the device"), err)
    rc, out, err, _ = st.run("enumerate")
    ids = sorted(t["id"] for t in out[0]["things"]) if rc == 0 else []
    rc2, out2, err2, _ = st.run("apply", "2.1010", "on=true")
    check("enumerate picks up the new ids and writes work again", rc == 0 and "2.1010" in ids and rc2 == 0, (rc, err, rc2, err2))

    # Unpair edge cases.
    fresh_pairing(m, st, ["remove_no_answer"])
    rc, out, err, _ = st.run("unpair")
    check("unpair when the accessory closes without answering: confirmed by a second pair-verify",
          rc == 0 and out[0]["paired"] is False and m.stats()["pairings"] == {}, (out, m.stats()["pairings"]))
    fresh_pairing(m, st, ["verify_unknown"])
    rc, out, err, _ = st.run("unpair")
    check("unpair of an accessory that no longer knows this controller forgets the pairing",
          rc == 0 and out[0]["paired"] is False, out)
    fresh_pairing(m, st)
    m.stop()
    rc, out, err, _ = st.run("unpair")
    # The driver's half only: the app then stores Device::auth as the driver left it
    # (src/smarthome/control.cpp, home::unpair).
    check("unpair while the accessory is unreachable: the driver keeps the pairing for a later try",
          rc == 0 and out[0]["paired"] is True, out)


def set_info(st, key, value):
    """What a later discovery scan would record for the device (src/smarthome/store.cpp
    remember_scan copies discovery's info over the saved device's)."""
    d = st.load()
    d["device"]["info"][key] = value
    with open(st.path, "w") as f:
        json.dump(d, f)


def each_of(out):
    return out[0].get("each", {}) if out and isinstance(out[0], dict) else {}


# ============================================================================================
def test_partial_failures():
    """One bridged accessory switched off at the wall: the others still work, and nothing old is
    shown as current."""
    m = Mock()
    st = paired_state(m)
    m.config(unreachable=[3])   # the kettle plug
    rc, out, err, _ = st.run("apply-many", "3.10,14.10,21.10,21.20", "on=true")
    s = m.stats()
    each = each_of(out)
    check("apply_many with one thing switched off: the things are handed back one by one, each with its own result",
          rc == 1 and each.get("14.10") == "ok" and each.get("21.10") == "ok" and each.get("21.20") == "ok" and
          each.get("3.10", "").startswith("Kettle Plug is switched off") and s["values"]["14.11"] is True and
          s["values"]["21.11"] is True and s["values"]["21.21"] is True, (rc, out, err))
    check("... and the error does not read as \"the bridge did not answer\" (which costs the app a rescan)",
          not any(w in each.get("3.10", "").lower() for w in UNREACHABLE_WORDS), each.get("3.10"))
    # One of several cannot take the change at all (the panel heater has no auto mode): the others
    # are still changed, and only that one says why.
    rc2, out2, err2, _ = st.run("apply-many", "4.10,13.10", "mode=auto")
    each2 = each_of(out2)
    check("apply_many where one thing cannot take the change: the other is still changed",
          each2.get("4.10") == "ok" and "no auto mode" in each2.get("13.10", "") and m.stats()["values"]["4.12"] == 3,
          (rc2, out2, err2))
    puts = len(m.stats()["puts"])
    rc, out, err, _ = st.run("apply", "3.10", "on=false")
    check("a write to the thing that is off is refused before anything is sent",
          rc == 1 and err_has(err, "Kettle Plug is switched off") and len(m.stats()["puts"]) == puts, (rc, err))
    rc, out, err, _ = st.run("read", "2.10", "3.10")
    got = {t["id"]: t["state"] for t in out[0]["things"]} if rc == 0 else {}
    check("read with one thing off: its old values are left out, not shown as current; the others are read",
          rc == 0 and got.get("3.10") == {"reachable": False} and got.get("2.10", {}).get("brightness") == 40 and
          "reachable" not in got.get("2.10", {}), (rc, err, got))
    rc, out, err, _ = st.run("read", "3.10")
    check("reading only the thing that is off is an error that says why",
          rc == 1 and err_has(err, "Kettle Plug is switched off, or out of range of its bridge"), (rc, out, err))

    # It goes off between the check and the write: the PUT itself fails for it alone.
    m.config(unreachable=[], fail_writes=[3])
    puts = len(m.stats()["puts"])
    rc, out, err, _ = st.run("apply-many", "3.10,14.10,21.10,21.20", "on=false")
    s = m.stats()
    each = each_of(out)
    check("a PUT that fails for one of several things: each thing is then applied alone, the others are changed",
          rc == 1 and len(s["puts"]) == puts + 1 + 4 and each.get("3.10", "").startswith("Kettle Plug is switched off") and
          [each.get(k) for k in ("14.10", "21.10", "21.20")] == ["ok"] * 3 and s["values"]["14.11"] is False and
          s["values"]["21.11"] is False and s["values"]["21.21"] is False, (rc, out, err, len(s["puts"]) - puts))
    m.config(fail_writes=[])
    rc, out, err, _ = st.run("apply-many", "14.10,21.10", "on=true")
    check("when every thing works, apply_many is still one PUT and says nothing more", rc == 0 and "each" not in out[0], (rc, out))
    m.stop()


def test_forgotten_pairing():
    """The accessory forgets this app (a HomeKit reset from its maker's app): the way back is to
    type the code again, and nothing else is ever sent without it."""
    m = Mock()
    st = paired_state(m)
    set_info(st, "sf", "0")   # what a scan records while this app is the accessory's controller
    ctl = st.load()["secrets"]["homekit"]["controller_id"]
    m.config(drop_pairings=True)
    s0 = m.stats()
    rc, out, err, _ = st.run("enumerate", "--then", "step", "--then", "pair", CODE, "--then", "step", "--then", "enumerate")
    s = m.stats()
    check("an accessory that forgot this app: the error says to type its code in settings",
          len(err) >= 1 and "no longer recognises this app" in err[0] and "setup code in settings" in err[0], err)
    check("... the next step asks for the code (the same driver object, as in the app)",
          len(out) >= 1 and out[0].get("kind") == "EnterCode" and "no longer recognises this app" in out[0].get("prompt", ""),
          out[:1])
    check("... and the code makes a new pairing (sent once; discovery's old sf=0 was this app's own), after which "
          "the accessory works", rc == 1 and len(out) == 4 and out[1].get("paired") is True and
          len(out[3].get("things", [])) == 17 and s["setup_m3"] == s0["setup_m3"] + 1 and list(s["pairings"]) == [ctl],
          (rc, out[1:2], err, s["setup_m3"] - s0["setup_m3"], list(s["pairings"])))
    # The app asks next_step straight after pair() (control.cpp pairing_submit) and keeps the
    # device marked "needs setup" unless it is None.
    check("... and right after that new pairing no step is left (asked before anything else)",
          len(out) == 4 and out[2].get("kind") == "None", out[2:3])
    m.config(drop_pairings=True)
    s0 = m.stats()
    rc, out, err, _ = st.run("pair", CODE)
    s = m.stats()
    check("pair() on a pairing the accessory forgot finds out by itself and pairs again (a new process: no step needed)",
          rc == 0 and out[0]["paired"] is True and s["verify_fail"] == s0["verify_fail"] + 1 and
          s["setup_ok"] == s0["setup_ok"] + 1, (rc, err))
    m.config(drop_pairings=True, paired_with_other=True)
    s0 = m.stats()
    rc, out, err, _ = st.run("enumerate", "--then", "step", "--then", "pair", CODE, "--then", "step")
    s = m.stats()
    check("... unless another controller has it by then: refused at pair-setup M2, the code never sent",
          rc == 1 and len(err) == 2 and "already paired with another controller" in err[1] and
          s["setup_m3"] == s0["setup_m3"] and "accessory_id" in st.auth(), (rc, err, s["setup_m3"] - s0["setup_m3"]))
    check("... and from then on the step says it is paired with another controller, no longer \"type its code\"",
          len(out) == 2 and "no longer recognises" in out[0].get("prompt", "") and out[1].get("kind") == "EnterCode" and
          "already paired with another controller" in out[1].get("prompt", ""), out)
    m.stop()


def test_saved_pairing_refusals():
    """pair() and unpair() on a saved pairing act on "the accessory no longer knows this app" only
    when the accessory said so in pair-verify M4, after proving in M2 that it holds the saved key.
    Anything else at the address (an unsigned refusal in M2, another error in M4, another identity,
    an impostor, nothing at all, garbage) gets no pair-setup message, so never the code, and the
    pairing is kept. The peers have codes of their own: a code sent to one of them would be
    refused, remembered, and answered "already refused" the second time."""
    other = "999-88-777"
    a = Mock()
    paired = paired_state(a)
    ctl = paired.load()["secrets"]["homekit"]["controller_id"]

    # An unsigned Authentication error at pair-verify M2 (HomeKit ADK and HAP-NodeJS answer
    # Authentication only in M4, once the controller has identified itself in M3).
    b = Mock("--code", other, "--fault", "auth_at_m2")
    st = saved_at(b.port, paired)
    rc, out, err, _ = st.run("enumerate", "--then", "step", "--then", "pair", CODE, "--then", "pair", CODE, "--then", "unpair")
    check("an unsigned Authentication error at pair-verify M2: the session was turned down, not \"this app is forgotten\"; "
          "no step offered", len(err) == 3 and "turned down" in err[0] and "no longer recognises" not in err[0] and
          len(out) == 2 and out[0].get("kind") == "None", (out, err))
    check("... pair() sends that peer no pair-setup message, so no code (the same answer both times: nothing was "
          "refused)", setup_requests(b) == 0 and len(err) == 3 and all("turned down" in e for e in err[1:]), (err, b.stats()["requests"]))
    check("... and unpair keeps the pairing, which the real accessory still holds",
          len(out) == 2 and out[1].get("paired") is True and "accessory_id" in st.auth() and list(a.stats()["pairings"]) == [ctl],
          (out, list(a.stats()["pairings"])))
    b.stop()

    # The paired accessory answers M4 with an error other than Authentication.
    a.config(faults=["verify_m4_unknown"])
    n = setup_requests(a)
    st = saved_at(a.port, paired)
    rc, out, err, _ = st.run("enumerate", "--then", "step", "--then", "pair", CODE, "--then", "unpair")
    check("pair-verify M4 answered with Unknown (not Authentication): not \"forgotten\", no step, no pair-setup, "
          "unpair keeps the pairing", len(err) == 2 and all("turned down" in e for e in err) and len(out) == 2 and
          out[0].get("kind") == "None" and out[1].get("paired") is True and setup_requests(a) == n and
          list(a.stats()["pairings"]) == [ctl], (out, err, setup_requests(a) - n))
    a.config(faults=[])

    # Another identity, an impostor (the saved id, but not the saved key), garbage.
    c = Mock("--code", other)
    c.config(factory_reset=True)   # an unpaired accessory with an identity of its own
    d = Mock("--code", other, "--fault", "impostor")
    g = Mock("--code", other, "--fault", "garbage_tlv")
    for peer, what, words in [(c, "another accessory", "different HomeKit accessory"),
                              (d, "an impostor signing with another key", "identity check failed"),
                              (g, "garbled pair-verify answers", "not understood")]:
        st = saved_at(peer.port, paired)
        rc, out, err, _ = st.run("pair", CODE, "--then", "pair", CODE, "--then", "step")
        check("pair() on a saved pairing, %s at the address: \"%s\" both times, no pair-setup message (no code), "
              "no step" % (what, words), rc == 1 and len(err) == 2 and all(words in x for x in err) and
              setup_requests(peer) == 0 and out and out[-1].get("kind") == "None" and "accessory_id" in st.auth(),
              (out, err, setup_requests(peer)))
        peer.stop()
    st = saved_at(free_port(), paired)
    rc, out, err, _ = st.run("pair", CODE, "--then", "pair", CODE)
    check("pair() on a saved pairing, nothing at the address: \"could not be reached\" both times",
          rc == 1 and len(err) == 2 and all("could not be reached" in x for x in err), err)

    # The paired accessory itself is reset to factory settings: a new HomeKit id and key.
    a.config(factory_reset=True)
    n = setup_requests(a)
    rc, out, err, _ = paired.run("enumerate", "--then", "step", "--then", "pair", CODE, "--then", "unpair")
    check("after a factory reset (new HomeKit identity): the error says so and how to go on (scan, pair the new one, "
          "remove this one); no step; pair() sends nothing; the pairing is kept",
          len(err) == 2 and all("new HomeKit identity" in x and "Scan the network again" in x for x in err) and
          len(out) == 2 and out[0].get("kind") == "None" and out[1].get("paired") is True and setup_requests(a) == n,
          (out, err, setup_requests(a) - n))
    a.stop()


def test_pair_setup_method():
    """Method 0 (Pair Setup) unless discovery's ff says the accessory has an Apple authentication
    chip (bit 0): then method 1, as aiohomekit's pair_with_auth chooses. HomeKitADK refuses method 1
    at M4 without the chip, so the choice matters."""
    m = Mock()
    for ff in (None, "0", "2"):
        m.reset()
        st = State(m.port, **({} if ff is None else {"ff": ff}))
        rc, out, err, _ = st.run("pair", CODE)
        s = m.stats()
        check("ff %s: Pair Setup (method 0), and it pairs" % ("absent" if ff is None else "= " + ff),
              rc == 0 and s["setup_methods"] == [0] and s["setup_ok"] == 1, (rc, err, s["setup_methods"]))
    m.reset()
    st = State(m.port, ff="1")
    rc, out, err, _ = st.run("pair", CODE)
    s = m.stats()
    check("method 1 to an accessory without the chip fails at M4 with a bare 500, as HomeKitADK's does: reported, "
          "and not taken for a wrong code", rc == 1 and err_has(err, "internal error") and s["setup_m3_methods"] == [1] and
          s["auth_failures"] == 0 and "accessory_id" not in st.auth(), (rc, err, s["setup_m3_methods"]))
    m.stop()
    m = Mock("--auth-chip")
    st = State(m.port, ff="1")
    rc, out, err, _ = st.run("pair", CODE)
    s = m.stats()
    check("ff = 1 (an authentication chip): Pair Setup with Auth (method 1), and it pairs",
          rc == 0 and s["setup_methods"] == [1] and s["setup_ok"] == 1, (rc, err, s["setup_methods"]))
    m.stop()


def test_stale_ids():
    """The accessory's database changed after enumerate: an id written must still be the
    characteristic it was, in the same accessory, which still has no denied service."""
    m = Mock()
    st = paired_state(m)

    def apply_with(faults, *args):
        m.config(faults=faults)
        before = m.stats()
        rc, out, err, _ = st.run(*args)
        after = m.stats()
        m.config(faults=[])
        return rc, err, after, len(after["puts"]) - len(before["puts"]), after["requests"][len(before["requests"]):]

    rc, err, s, puts, reqs = apply_with(["reuse_ids"], "apply", "2.10", "on=false")
    check("a lock now sits at the lamp's old ids: \"lamp off\" is refused, nothing is written, the lock stays locked",
          rc == 1 and err_has(err, "has changed on the accessory") and puts == 0 and s["values"].get("2.11") == 1,
          (rc, err, puts, s["values"].get("2.11")))
    rc, err, s, puts, reqs = apply_with(["replace_accessory"], "apply", "2.10", "on=true")
    check("another accessory (a lock) now has the lamp's aid, with a switch where the lamp's On was: refused, "
          "nothing written", rc == 1 and err_has(err, "has changed on the accessory") and puts == 0 and
          s["values"].get("2.11") is False, (rc, err, puts))
    rc, err, s, puts, reqs = apply_with(["firmware_reuse"], "apply", "7.10", "position=40")
    check("new firmware put a window opener where the blind was (the same characteristic types): the database is "
          "read again, the write refused, the window stays shut",
          rc == 1 and err_has(err, "has changed on the accessory") and puts == 0 and s["values"].get("7.12") == 0 and
          ["GET", "/accessories"] in reqs, (rc, err, puts, reqs))
    rc, err, s, puts, reqs = apply_with(["firmware_update"], "apply", "2.10", "on=true")
    check("new firmware and nothing else changed: the database is read again, then the write goes ahead",
          rc == 0 and puts == 1 and ["GET", "/accessories"] in reqs, (rc, err, reqs))
    rc, err, s, puts, reqs = apply_with(["no_types"], "apply", "2.10", "on=true")
    check("an accessory that gives no types: checked against its database instead, then written",
          rc == 0 and puts == 1 and ["GET", "/accessories"] in reqs, (rc, err, reqs))
    rc, err, s, puts, reqs = apply_with(["no_types", "reuse_ids"], "apply", "2.10", "on=false")
    check("... and that check refuses the lock at the lamp's old ids", rc == 1 and err_has(err, "has changed on the accessory") and
          puts == 0 and s["values"].get("2.11") == 1, (rc, err, puts))
    gets = len(m.stats()["gets"])
    rc, err, s, puts, reqs = apply_with(["type_400"], "apply", "2.10", "on=true")
    check("an accessory that answers 400 to type=1: checked against its database instead (one GET /accessories), "
          "then written", rc == 0 and puts == 1 and reqs.count(["GET", "/accessories"]) == 1 and
          any("type=1" in g for g in s["gets"][gets:]), (rc, err, reqs))
    rc, err, s, puts, reqs = apply_with(["type_400", "reuse_ids"], "apply", "2.10", "on=false")
    check("... and that check refuses the lock at the lamp's old ids", rc == 1 and err_has(err, "has changed on the accessory") and
          puts == 0 and s["values"].get("2.11") == 1, (rc, err, puts))
    set_info(st, "c#", "5")
    st.run("enumerate")
    rc, err, s, puts, reqs = apply_with([], "apply", "2.10", "on=true")
    check("the same configuration number (c#) as at enumerate: no second look at the database",
          rc == 0 and puts == 1 and ["GET", "/accessories"] not in reqs, (rc, err, reqs))
    set_info(st, "c#", "6")
    rc, err, s, puts, reqs = apply_with([], "apply", "2.10", "on=true")
    check("a later scan reports another c#: the database is read again before the write",
          rc == 0 and puts == 1 and ["GET", "/accessories"] in reqs, (rc, err, reqs))
    rc, err, s, puts, reqs = apply_with(["reuse_ids"], "apply-many", "2.10,3.10", "on=false")
    check("apply_many with one stale thing refuses them all, with the reason (nothing written)",
          rc == 1 and err_has(err, "has changed on the accessory") and puts == 0 and s["values"].get("2.11") == 1, (rc, err, puts))
    m.stop()


def test_slow_peers():
    """A connection that never completes, and pairing or unpairing with an accessory that says
    nothing: each call ends at its own deadline. Side by side, so the 30 s wait happens once."""
    lst = socket.socket()
    lst.bind(("127.0.0.1", 0))
    lst.listen(0)
    fillers = []
    for _ in range(8):   # fill the listener's queue: the kernel then drops further connection attempts
        c = socket.socket()
        c.settimeout(0.3)
        try:
            c.connect(lst.getsockname())
            fillers.append(c)
        except OSError:
            c.close()
            break
    m = Mock()
    paired = paired_state(m)
    full_a, full_b = saved_at(lst.getsockname()[1], paired), saved_at(lst.getsockname()[1], paired)
    again = saved_at(m.port, paired)   # the same pairing, in a file of its own
    m.config(faults=["silent"])   # the paired accessory now reads every request and answers none
    setups = setup_requests(m)
    m2 = Mock("--fault", "silent")
    fresh = State(m2.port)
    jobs = {"apply, connection never completes": (full_a, ("apply", "2.10", "on=true"), "could not be reached", 2.0),
            "read, connection never completes": (full_b, ("read",), "could not be reached", 2.0),
            "pair, accessory silent": (fresh, ("pair", CODE), "did not answer in time", 30.0),
            "pair again on a saved pairing, accessory silent (pair-verify only)": (again, ("pair", CODE), "did not answer in time", 4.0),
            "unpair, accessory silent": (paired, ("unpair",), None, 8.0)}
    results = {}

    def run(name, st, cmd):
        results[name] = st.run(*cmd)

    threads = [threading.Thread(target=run, args=(name, j[0], j[1])) for name, j in jobs.items()]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for name, (st, cmd, words, limit) in jobs.items():
        rc, out, err, el = results[name]
        if words is None:   # unpair: nothing to report, the pairing kept for a later try
            ok = rc == 0 and out and out[0].get("paired") is True
        else:
            ok = rc == 1 and err_has(err, words)
        check("%s: ends at its %.0f s deadline (took %.2f s)" % (name, limit, el), ok and limit - 0.2 <= el <= limit + 1.5,
              (rc, out, err, round(el, 2)))
    check("the silent accessory never got the code", m2.stats()["setup_m3"] == 0)
    check("pair() on a saved pairing whose accessory says nothing sends no pair-setup message after the failed "
          "pair-verify (so never the code)", setup_requests(m) == setups and "accessory_id" in again.auth(),
          setup_requests(m) - setups)
    for c in fillers:
        c.close()
    lst.close()
    m.stop()
    m2.stop()


def test_claims():
    m = Mock()
    st = State(m.port)
    rc, out, err, _ = st.run("claims", "hue_bridge")
    check("an unknown device kind is not claimed", rc == 0 and out[0]["claims"] is False, out)
    st6 = State(m.port, ci="6")
    rc, out, err, _ = st6.run("claims")
    check("a HomeKit lock (category 6) is not claimed", rc == 0 and out[0]["claims"] is False, out)
    st31 = State(m.port, ci="31")
    rc, out, err, _ = st31.run("claims")
    check("a television (category 31) is not claimed: nothing of it is on the allow-list", rc == 0 and out[0]["claims"] is False, out)
    rc, out, err, _ = st.run("claims")
    check("a HomeKit bridge is claimed", rc == 0 and out[0]["claims"] is True, out)
    m.stop()


def main():
    if not os.access(CLI, os.X_OK):
        print("FAIL  %s is not built" % CLI)
        sys.exit(1)
    try:
        test_claims()
        test_pairing_and_control()
        test_big_bridge()
        test_pairing_failures()
        test_session_failures()
        test_partial_failures()
        test_forgotten_pairing()
        test_saved_pairing_refusals()
        test_pair_setup_method()
        test_stale_ids()
        test_slow_peers()
        reports = [o for o in outputs if "runtime error:" in o or "Sanitizer" in o or "terminate called" in o]
        check("no sanitizer report, uncaught exception or abort in any run", not reports,
              reports[0][:300] if reports else "")
        # (the prompts' own example, "like 123-45-678", happens to be the mock's code)
        codes = [c for c in (CODE, WRONG, WRONG2) for o in outputs
                 if c in o.replace("like 123-45-678", "") or c.replace("-", "") in o]
        check("no setup code appears in anything the CLI printed", not codes, codes[:3])
    finally:
        cleanup()
    print("%d checks, %d failed" % (run_count, fail_count))
    sys.exit(1 if fail_count else 0)


if __name__ == "__main__":
    main()
