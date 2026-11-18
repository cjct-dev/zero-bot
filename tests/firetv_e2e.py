#!/usr/bin/env python3
"""Runs firetv_cli against mock_firetv.py on 127.0.0.1 and checks what the mock saw.

usage: firetv_e2e.py [path/to/firetv_cli] [group ...]   (default: build/unit/firetv_cli, which
tests/run_unit.sh builds; every group)
groups: wrong_code command capability wake starting auth hostile unknown (pairing and enumerate always
run; naming groups is for quick checks of one area, e.g. of a deliberately broken build)

Prints one line per check and exits non-zero when any fails. Nothing outside 127.0.0.1 is
contacted: the mock binds ports the system picks, and the driver is told to send its
Wake-on-LAN packets to the mock's UDP port on 127.0.0.1. What reached the "TV" (requests,
headers, wake calls, magic packets, codes tried) is read from the mock's own counters, not from
what the client says about itself.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(HERE, "..", "build", "unit", "firetv_cli")
MOCK = os.path.join(HERE, "mocks", "firetv", "mock_firetv.py")
MAC = "00:00:5e:00:53:01"
OTHER_MAC = "00:00:5e:00:53:02"
API_KEY = "0987654321"

run_count = fail_count = 0
seen_output = []
mocks = []
workdir = tempfile.mkdtemp(prefix="firetv-e2e-")


def check(name, ok, detail=""):
    global run_count, fail_count
    run_count += 1
    fail_count += not ok
    print("%s  %s%s" % ("ok  " if ok else "FAIL", name, "" if ok or detail == "" else "  -> %s" % (detail,)), flush=True)
    return ok


class Mock:
    def __init__(self, *args):
        self.proc = subprocess.Popen([sys.executable, MOCK, "--max-seconds", "900", *args], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, text=True)
        mocks.append(self)
        line = self.proc.stdout.readline().split()
        if line[:2] != ["MOCK", "listening"]:
            raise RuntimeError("mock did not start: %r" % line)
        fields = dict(f.split("=", 1) for f in line[2:])
        self.api, self.dial, self.wol, self.control = (int(fields[k]) for k in ("api", "dial", "wol", "control"))
        self.pin = fields["pin"]

    def _post(self, path, cfg):
        req = urllib.request.Request("http://127.0.0.1:%d%s" % (self.control, path), data=json.dumps(cfg).encode(),
                                     method="POST")
        with urllib.request.urlopen(req, timeout=5) as r:
            return json.load(r)

    def stats(self):
        with urllib.request.urlopen("http://127.0.0.1:%d/stats" % self.control, timeout=5) as r:
            return json.load(r)

    def reset(self, **cfg):
        self.pin = self._post("/reset", cfg)["pin"]

    def set(self, **cfg):
        self._post("/set", cfg)

    def stop(self):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        self.proc.kill()
        self.proc.wait()


class Tv:
    """A saved device (the state file the CLI reads and writes) pointing at a mock."""
    count = 0

    def __init__(self, mock, info=None, auth=None, kind="fire_tv", name="Living Room TV", port=None, mac="",
                 ip="127.0.0.1"):
        Tv.count += 1
        self.mock = mock
        self.path = os.path.join(workdir, "tv%d.json" % Tv.count)
        merged = {"dial_port": str(mock.dial), "wakeup": "MAC=%s;Timeout=20" % MAC}
        merged.update(info or {})
        merged = {k: v for k, v in merged.items() if v is not None}
        # `mac` is Device::mac, which the app's discovery fills from the ARP table.
        device = {"id": "fire_tv:test%d" % Tv.count, "kind": kind, "name": name, "model": "", "mac": mac,
                  "ip": ip, "port": mock.api if port is None else port, "info": merged, "auth": auth or {}}
        with open(self.path, "w") as f:
            json.dump({"device": device, "things": []}, f)

    def cli(self, *args, deadline=None):
        cmd = [CLI, self.path, *args, "--wake-port", str(self.mock.wol)]
        if deadline is not None:
            cmd += ["--deadline", str(deadline)]
        t0 = time.time()
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        except subprocess.TimeoutExpired:
            return -1, None, "firetv_cli did not finish within 60 s", 60.0
        elapsed = time.time() - t0
        seen_output.append(p.stdout + p.stderr)
        out = None
        if p.stdout.strip():
            try:
                out = json.loads(p.stdout.strip().splitlines()[-1])
            except ValueError:
                out = p.stdout
        return p.returncode, out, p.stderr.strip(), elapsed

    def apply(self, change, deadline=2500):
        return self.cli("apply", json.dumps(change), deadline=deadline)

    def apply_twice(self, change, gap_ms, deadline=2500, then=None, first_device=None):
        """Two calls from one driver object, gap_ms apart (as the app would make them): `change`,
        then `then` (by default the same change again). With first_device (another Tv), the first
        call goes to that device instead. Returns (first fault, second rc, second error, total s)."""
        cmd = [CLI, self.path, "apply", json.dumps(change), "--wake-port", str(self.mock.wol), "--deadline", str(deadline),
               "--again-after", str(gap_ms)]
        if then is not None:
            cmd += ["--then", json.dumps(then)]
        if first_device is not None:
            cmd += ["--first-device", first_device.path]
        t0 = time.time()
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        seen_output.append(p.stdout + p.stderr)
        first = {}
        for line in p.stdout.splitlines():
            if line.startswith('{"first"'):
                first = json.loads(line)["first"]
        return first.get("fault"), p.returncode, p.stderr.strip(), time.time() - t0

    def apply_seq(self, changes, gap_ms, deadline=2500):
        """Several calls from one driver object, gap_ms apart. Returns ([{"ok", "fault", "error"}
        for every call but the last], last rc, last error, total s)."""
        cmd = [CLI, self.path, "apply", json.dumps(changes[0]), "--wake-port", str(self.mock.wol), "--deadline",
               str(deadline), "--again-after", str(gap_ms)]
        for change in changes[1:]:
            cmd += ["--then", json.dumps(change)]
        t0 = time.time()
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=90)
        seen_output.append(p.stdout + p.stderr)
        earlier = [json.loads(line)["first"] for line in p.stdout.splitlines() if line.startswith('{"first"')]
        return earlier, p.returncode, p.stderr.strip(), time.time() - t0

    def state(self):
        with open(self.path) as f:
            return json.load(f)

    def auth(self):
        return self.state()["device"]["auth"]

    def set_auth(self, auth):
        st = self.state()
        st["device"]["auth"] = auth
        with open(self.path, "w") as f:
            json.dump(st, f)

    def thing(self):
        things = self.state()["things"]
        return things[0] if things else {}

    def pair(self):
        """Both steps, against an awake mock; returns True when paired."""
        rc1, _, err1, _ = self.cli("pair")
        shown = self.mock.stats()["shown_pin"]
        rc2, _, err2, _ = self.cli("pair", shown or "0000")
        return rc1 == 0 and rc2 == 0, (err1, err2)


def fault(err):
    return err[len("error["):err.index("]")] if err.startswith("error[") else ""


def message(err):
    """The driver's own words, without the CLI's "error[<fault>]: " prefix."""
    return err.split("]: ", 1)[1] if err.startswith("error[") and "]: " in err else err


def reads_as_unreachable(err):
    """Whether the app's control.cpp unreachable() would take this error for "it did not answer"
    (it then searches the network for 2.5 s and may apply the change again at a new address)."""
    e = message(err).lower()
    return any(w in e for w in ("connect", "reach", "answer", "respond", "timed out", "timeout", "no route", "unreachable"))


def api_requests(st, path=None, method=None):
    return [r for r in st["requests"] if (path is None or r["path"] == path) and (method is None or r["method"] == method)]


def keys(st):
    return [k["action"] for k in st["keys"]]


# ---------------------------------------------------------------------------------------------
def pairing_tests(m):
    m.reset(state="awake", pin="4321", ok_first=2, forget_tokens=True)
    tv = Tv(m)
    rc, out, err, _ = tv.cli("step")
    check("pairing: the first step asks for the code to be shown, once",
          rc == 0 and out["kind"] == "accept_prompt" and out["wait_s"] == 0 and "4-digit code" in out["prompt"], (out, err))
    rc, out, err, _ = tv.cli("pair")
    st = m.stats()
    disp = api_requests(st, "/v1/FireTV/pin/display")
    check("pairing: step 1 makes the TV show a code", rc == 0 and st["pin_display"] == 1 and st["shown_pin"] == "4321", (rc, err))
    check("pairing: pin/display carries the API key, JSON and our name, and no token",
          len(disp) == 1 and disp[0]["api_key"] == API_KEY and disp[0]["content_type"] == "application/json" and
          disp[0]["token"] is None and json.loads(disp[0]["body"]) == {"friendlyName": "Zero Bot"}, disp)
    pending = tv.auth().get("pending", {})
    check("pairing: the TV's key is remembered from step 1 (pin computed independently by the mock)",
          pending.get("key_pin") == m.pin and isinstance(pending.get("asked"), int), pending)
    rc, out, err, _ = tv.cli("step")
    check("pairing: the second step asks for the code", rc == 0 and out["kind"] == "enter_code" and out["field"], out)
    rc, out, err, _ = tv.cli("pair", "43a1")
    check("pairing: a code that is not 4 digits is refused without sending it",
          rc == 1 and m.stats()["pin_verify"] == 0 and tv.auth().get("pending"), (rc, err))
    rc, out, err, _ = tv.cli("pair", "4321")
    st = m.stats()
    auth = tv.auth()
    check("pairing: OK, OK, then the token: three verify calls, paired", rc == 0 and st["pin_verify"] == 3 and
          len(st["issued"]) == 1, (rc, err, st["pin_verify"]))
    check("pairing: the token, the key pin and the announced wake address are stored",
          auth.get("token") == (st["issued"] or [None])[0] and auth.get("key_pin") == m.pin and
          auth.get("wake_mac") == MAC and "pending" not in auth, {k: v for k, v in auth.items() if k != "token"})
    verify = api_requests(st, "/v1/FireTV/pin/verify")
    check("pairing: the code went as {\"pin\":\"4321\"} with the API key",
          all(json.loads(r["body"]) == {"pin": "4321"} and r["api_key"] == API_KEY for r in verify), verify)
    rc, out, err, _ = tv.cli("step")
    check("pairing: nothing more to do once paired", rc == 0 and out["kind"] == "none", out)
    return tv


def wrong_code_tests(m):
    for wrong in ("400", "200text"):
        m.reset(state="awake", pin="4321", wrong_reply=wrong)
        tv = Tv(m)
        tv.cli("pair")
        rc, out, err, _ = tv.cli("pair", "0000")
        st = m.stats()
        check("wrong code (%s reply): refused, sent once, nothing stored" % wrong,
              rc == 1 and "did not accept" in err and st["pin_verify"] == 1 and "token" not in tv.auth(), (rc, err, st["pin_verify"]))
        rc, out, err, _ = tv.cli("pair", "0000")
        check("wrong code (%s reply): asked again with the same code, the TV is not contacted" % wrong,
              rc == 1 and "expired" in err and m.stats()["pin_verify"] == 1, (rc, err))
        rc, out, err, _ = tv.cli("pair", "4321")
        check("wrong code (%s reply): even the right code needs a new one first" % wrong,
              rc == 1 and m.stats()["pin_verify"] == 1, (rc, err))
        rc, out, err, _ = tv.cli("step")
        check("wrong code (%s reply): pairing starts again at step 1" % wrong, out and out["kind"] == "accept_prompt", out)

    m.reset(state="awake", pin="4321", verify="unreadable")
    tv = Tv(m)
    tv.cli("pair")
    rc, out, err, _ = tv.cli("pair", "4321")
    check("an unreadable answer to the code ends the attempt after one try",
          rc == 1 and "not understood" in err and m.stats()["pin_verify"] == 1 and "token" not in tv.auth(), (rc, err))

    m.reset(state="awake", pin="4321", faults={"POST /v1/FireTV/pin/verify": "wrongtypes"})
    tv = Tv(m)
    tv.cli("pair")
    rc, out, err, _ = tv.cli("pair", "4321")
    check("a description of the wrong type is not a token, and is not retried",
          rc == 1 and len(api_requests(m.stats(), "/v1/FireTV/pin/verify")) == 1 and "token" not in tv.auth(), (rc, err))

    m.reset(state="awake", pin="4321", verify="always_ok")
    tv = Tv(m)
    tv.cli("pair")
    rc, out, err, elapsed = tv.cli("pair", "4321")
    check("OK forever: three tries 1 s apart, then a clear failure", rc == 1 and m.stats()["pin_verify"] == 3 and
          "did not finish" in err and 1.9 < elapsed < 4.0, (rc, err, elapsed))

    m.reset(state="awake", pin="4321", verify="silent")
    tv = Tv(m)
    tv.cli("pair")
    rc, out, err, elapsed = tv.cli("pair", "4321")
    check("a code the TV took and never answered is not sent again, nor the TV woken (it was there)",
          rc == 1 and fault(err) == "not_serving" and "new code" in err and
          m.stats()["pin_verify"] == 1 and m.stats()["dial_posts"] == 0, (rc, err, m.stats()["pin_verify"]))

    # Step 1 follows the same rule (review round 2): a pin/display that went out and got no answer
    # may already have put a code on the screen, so it is neither sent again nor followed by a wake.
    m.reset(state="awake", pin="4321", faults={"POST /v1/FireTV/pin/display": "close"})
    tv = Tv(m)
    rc, out, err, _ = tv.cli("pair")
    st = m.stats()
    check("pairing: a pin/display the TV read and closed without answering is sent once, not woken, not_serving",
          rc == 1 and fault(err) == "not_serving" and len(api_requests(st, "/v1/FireTV/pin/display")) == 1 and
          st["dial_posts"] == 0 and "pending" not in tv.auth(),
          (rc, err, len(api_requests(st, "/v1/FireTV/pin/display")), st["dial_posts"]))

    # Step 2 is pinned to the key the TV presented in step 1: a different key in between (another
    # device at the address, or the TV reset) never receives the code.
    m.reset(state="awake", pin="4321", cert="a")
    tv = Tv(m)
    tv.cli("pair")
    m.set(cert="b")
    rc, out, err, _ = tv.cli("pair", "4321")
    st = m.stats()
    check("pairing: a different TLS key at step 2 is refused, and the code never goes out",
          rc == 1 and fault(err) == "key_changed" and st["pin_verify"] == 0 and
          not api_requests(st, "/v1/FireTV/pin/verify") and st["tls_connections"] >= 1, (rc, err, st["pin_verify"]))
    check("pairing: nothing is stored after a key change at step 2",
          "token" not in tv.auth() and "key_pin" not in tv.auth() and "pending" not in tv.auth(), list(tv.auth()))
    m.reset(state="awake", cert="a")

    # Reading the code off the screen takes long enough for an idle TV to close its API port
    # (firehose FireTvClientTest.aRefusedPinVerifyWakesTheTvAndRetriesTheSameAttempt).
    m.reset(state="awake", pin="4321", wake_delay=0.9)
    tv = Tv(m)
    tv.cli("pair")
    m.set(state="idle")
    rc, out, err, _ = tv.cli("pair", "4321")
    st = m.stats()
    check("pairing: an idle TV at step 2 is woken once and the code then sent once, and it pairs",
          rc == 0 and st["dial_posts"] == 1 and st["pin_verify"] == 1 and tv.auth().get("token") in st["issued"],
          (rc, err, st["dial_posts"], st["pin_verify"]))

    m.reset(state="idle", pin="4321")
    tv = Tv(m)
    rc, out, err, elapsed = tv.cli("pair")
    st = m.stats()
    check("pairing an idle TV: woken by DIAL, then it shows the code", rc == 0 and st["dial_posts"] == 1 and
          st["pin_display"] == 1, (rc, err, st["dial_posts"]))

    m.reset(state="gone")
    tv = Tv(m)
    rc, out, err, elapsed = tv.cli("pair")
    check("pairing a TV that does not answer: a plain message, quickly",
          rc == 1 and fault(err) == "no_answer" and "on and connected" in err and elapsed < 3, (rc, err, elapsed))


def enumerate_tests(m, tv):
    m.reset(state="awake", caps="tv")
    rc, out, err, elapsed = tv.cli("enumerate")
    things = out["things"] if rc == 0 else []
    t = things[0] if things else {}
    check("enumerate: one thing, a TV, named as discovery named the device",
          rc == 0 and len(things) == 1 and t["id"] == "tv" and t["type"] == "tv" and t["name"] == "Living Room TV", (rc, err))
    check("enumerate: keys, play/pause, apps, power and volume steps on a TV that reports volume",
          set(t.get("cap_names", [])) == {"power", "transport", "volume_step", "source", "keys"}, t.get("cap_names"))
    want_apps = ["Netflix", "YouTube", "Prime Video", "Amazon Prime Music", "BBC iPlayer",
                 "A very long application name that goes o"]
    check("enumerate: installed apps only, names cleaned and capped at 40", t.get("state", {}).get("apps") == want_apps,
          t.get("state", {}).get("apps"))
    check("enumerate: reachable, and the state has no claim about the screen",
          t.get("state", {}).get("reachable") is True and "on" not in t.get("state", {}), t.get("state"))
    st = m.stats()
    token = tv.auth().get("token")
    check("enumerate: every request carried the API key, the token and JSON",
          st["requests"] and all(r["api_key"] == API_KEY and r["token"] == token and r["content_type"] == "application/json"
                                 for r in st["requests"]), [(r["path"], r["content_type"]) for r in st["requests"]])
    check("enumerate: it never wakes the TV", st["dial_posts"] == 0 and not st["magic"])
    check("enumerate: within its 4 s", elapsed < 4.3, elapsed)


def command_tests(m, tv):
    m.reset(state="awake", caps="tv")
    sequence = [("home", "home"), ("up", "dpad_up"), ("down", "dpad_down"), ("left", "dpad_left"),
                ("right", "dpad_right"), ("ok", "select"), ("back", "back"), ("menu", "menu")]
    results = [tv.apply({"key": k}) for k, _ in sequence]
    st = m.stats()
    check("keys: each key is one body-less press of the right action",
          all(r[0] == 0 for r in results) and st["keys"] == [{"action": a, "form": "bodyless"} for _, a in sequence],
          (st["keys"], [r[2] for r in results if r[0]]))
    posts = api_requests(st, "/v1/FireTV", "POST")
    check("keys: POST /v1/FireTV?action=... with the API key, the token and JSON",
          all(r["query"].get("action") and r["api_key"] == API_KEY and r["token"] == tv.auth()["token"] and
              r["content_type"] == "application/json" and r["body"] == "" for r in posts), posts[:2])
    check("keys: an awake TV is never woken for a key", st["dial_posts"] == 0)

    m.reset(state="awake", caps="tv")
    rc1 = tv.apply({"transport": "pause"})[0]
    rc2 = tv.apply({"transport": "play"})[0]
    st = m.stats()
    check("play and pause: both send the one toggle, POST /v1/media?action=play",
          rc1 == 0 and rc2 == 0 and keys(st) == ["media:play", "media:play"] and st["playing"] is False, keys(st))
    rc, out, err, _ = tv.apply({"key": "rewind"})
    rc2, out2, err2, _ = tv.apply({"key": "fast_forward"})
    st = m.stats()
    scans = api_requests(st, "/v1/media", "POST")[-2:]
    check("rewind and fast-forward: the captured scan bodies, charset=utf-8",
          rc == 0 and rc2 == 0 and st["scans"] == ["back", "forward"] and
          all(r["content_type"] == "application/json; charset=utf-8" for r in scans) and
          json.loads(scans[0]["body"]) == {"direction": "back", "durationInSeconds": "10", "speed": "1"}, (err, err2, scans))
    rc, out, err, _ = tv.apply({"transport": "stop"})
    check("stop: not offered by this API, reported as unsupported", rc == 1 and fault(err) == "unsupported", err)

    m.reset(state="awake", caps="tv")
    rc, out, err, _ = tv.apply({"source": "netflix"})
    check("source: an app by name, any case", rc == 0 and m.stats()["launched"] == ["com.netflix.ninja"], (rc, err))
    rc, out, err, _ = tv.apply({"source": "prime"})
    check("source: two apps match, both named, nothing launched",
          rc == 1 and "Prime Video" in err and "Amazon Prime Music" in err and m.stats()["launched"] == ["com.netflix.ninja"],
          err)
    before = len(api_requests(m.stats(), "/v1/FireTV/appsV2"))
    rc, out, err, _ = tv.apply({"source": "Hulu"})
    st = m.stats()
    check("source: an unknown app is looked up again on the TV, then refused by name",
          rc == 1 and "No app called \"Hulu\"" in err and len(api_requests(st, "/v1/FireTV/appsV2")) == before + 1 and
          st["launched"] == ["com.netflix.ninja"], (err, before))
    rc, out, err, _ = tv.apply({"source": "Removed App"})
    check("source: an app that is not installed is not launched", rc == 1 and m.stats()["launched"] == ["com.netflix.ninja"], err)

    # The smart_home tool's own example of a source is "HDMI 1" (tool.cpp). This API has no
    # recorded input switch, so an input name is unsupported (infrared can take over) and nothing
    # is sent for it: not even the app list is read again (review round 2).
    m.reset(state="awake", caps="tv")
    inputs = []
    for name in ("HDMI 1", "hdmi2", "AV", "Antenna", "Live TV"):
        rc, out, err, _ = tv.apply({"source": name})
        inputs.append((name, rc, err))
    st = m.stats()
    check("source: an input name (HDMI 1, hdmi2, AV, Antenna, Live TV) is unsupported, 'switching inputs'; nothing sent",
          all(rc == 1 and fault(err) == "unsupported" and message(err).endswith("switching inputs.") for _, rc, err in inputs)
          and st["requests"] == [] and st["launched"] == [], (inputs, len(st["requests"])))
    m.reset(state="awake", caps="tv", apps=[{"appId": "com.netflix.ninja", "name": "Netflix", "isInstalled": True},
                                            {"appId": "com.example.livetv", "name": "Live TV", "isInstalled": True}])
    tv.cli("enumerate")
    rc, out, err, _ = tv.apply({"source": "Live TV"})
    check("source: an installed app called exactly like an input ('Live TV') is still opened",
          rc == 0 and m.stats()["launched"] == ["com.example.livetv"], (rc, err, m.stats()["launched"]))
    m.reset(state="awake", caps="tv")
    tv.cli("enumerate")

    # A message that quotes what was asked for can carry one of the words control.cpp's
    # unreachable() looks for in the quote ("Reach"); the fixed words never do, and fault_of()
    # (the CLI's error[...]) classifies it the same either way (README-snippet: use fault_of()).
    rc, out, err, _ = tv.apply({"source": "Reach"})
    rc2, out2, err2, _ = tv.apply({"source": "Hulu"})
    check("a quoted name can bring the app's 'unreachable' words in, the fixed words do not; fault_of() says other",
          rc == 1 and rc2 == 1 and fault(err) == "other" and fault(err2) == "other" and
          message(err) == 'No app called "Reach" is on the TV.' and reads_as_unreachable(err) and
          message(err2) == 'No app called "Hulu" is on the TV.' and not reads_as_unreachable(err2), (err, err2))

    m.reset(state="awake", caps="tv")
    rc, out, err, elapsed = tv.apply({"volume_step": 3})
    st = m.stats()
    times = [r["t"] for r in api_requests(st, "/v1/FireTV", "POST")]
    gaps = [b - a for a, b in zip(times, times[1:])]
    check("volume: three steps up are three volume_up presses", rc == 0 and keys(st) == ["volume_up"] * 3 and
          st["volume"] == 23, (rc, err, keys(st)))
    check("volume: presses no faster than the official app's 220 ms", gaps and min(gaps) >= 0.2, gaps)
    rc, out, err, _ = tv.apply({"volume_step": -2})
    rc2, out2, err2, _ = tv.apply({"key": "mute"})
    st = m.stats()
    check("volume: steps down, and the mute key", rc == 0 and rc2 == 0 and st["volume"] == 21 and st["muted"] is True,
          (err, err2, st["volume"], st["muted"]))
    for wanted in (True, False):
        rc, out, err, _ = tv.apply({"mute": wanted})
        check("mute=%s: a chosen mute state is unsupported (the TV's mute is a toggle), nothing sent" % wanted,
              rc == 1 and fault(err) == "unsupported" and "toggles" in err and keys(m.stats())[-1] == "mute" and
              m.stats()["muted"] is True, err)
    rc, out, err, _ = tv.apply({"volume": 50})
    check("volume: an absolute level is not offered", rc == 1 and fault(err) == "unsupported", err)
    m.reset(state="awake", caps="tv", slow_tls_every=3, slow_tls_s=0.3)
    rc, out, err, elapsed = tv.apply({"volume_step": 4})
    st = m.stats()
    times = [r["t"] for r in api_requests(st, "/v1/FireTV", "POST")]
    gaps = [b - a for a, b in zip(times, times[1:])]
    check("volume: presses reach the TV at least 220 ms apart even when one handshake is slow (paced from each answer)",
          rc == 0 and len(times) == 4 and min(gaps) >= 0.2, (rc, err, gaps))
    m.reset(state="awake", caps="tv", faults={"POST /v1/FireTV": "close"}, fault_after={"POST /v1/FireTV": 2})
    rc, out, err, elapsed = tv.apply({"volume_step": 4})
    st = m.stats()
    check("volume: a run that breaks off after 2 of 4 says so, and not as 'no answer' (no infrared repeat)",
          rc == 1 and message(err) == "Only 2 of 4 presses were confirmed by the TV. Then it stopped taking them." and
          fault(err) == "other" and st["volume"] == 22 and st["dial_posts"] == 0 and elapsed < 2.9,
          (rc, err, st["volume"], elapsed))
    check("volume: that message does not send the app looking for the TV elsewhere", not reads_as_unreachable(err), err)
    m.reset(state="awake", caps="tv")
    rc, out, err, elapsed = tv.apply({"volume_step": 10}, deadline=1000)
    st = m.stats()
    made = len(st["keys"])
    check("volume: steps that do not fit the deadline stop in time and say how many were made",
          rc == 1 and message(err) == "Only %d of 10 presses were confirmed by the TV. There was no time for the rest." % made
          and 2 <= made < 10 and fault(err) == "other" and not reads_as_unreachable(err) and elapsed < 1.4,
          (rc, err, made, elapsed))
    m.reset(state="awake", caps="tv")
    tv.apply({"volume_step": 3})
    tv.apply({"volume_step": -2})
    tv.apply({"key": "mute"})
    for k in ("power", "input", "channel_up", "5"):
        rc, out, err, _ = tv.apply({"key": k})
        check("key %s: unsupported on this TV, so infrared can take over" % k, rc == 1 and fault(err) == "unsupported", err)
    check("unsupported keys sent nothing", len(m.stats()["keys"]) == 6, keys(m.stats()))
    rc, out, err, _ = tv.apply({"key": "home", "volume": 10})
    check("a change with one unsupported part sends nothing at all", rc == 1 and len(m.stats()["keys"]) == 6, err)

    m.reset(state="awake", caps="tv")
    rc, out, err, _ = tv.apply({"on": False})
    st = m.stats()
    check("off: action=sleep, the screen goes dark", rc == 0 and keys(st) == ["sleep"] and st["display"] == "off", (rc, err))
    rc, out, err, _ = tv.cli("read")
    check("off: the API still answers afterwards (display-off, not deep sleep)",
          rc == 0 and out["things"][0]["state"]["reachable"] is True, (rc, err))
    rc, out, err, elapsed = tv.apply({"on": True}, deadline=9000)
    st = m.stats()
    check("on: the DIAL wake lights the screen again", rc == 0 and st["dial_posts"] == 1 and st["display"] == "on", (rc, err))
    check("on: a magic packet for the stored wake address went to the mock, nowhere else",
          st["magic"] and all(p["valid"] and p["mac"] == MAC for p in st["magic"]), st["magic"])
    check("on: an awake TV answers at once", elapsed < 1.5, elapsed)


def capability_variant_tests(m, tv):
    m.reset(state="awake", caps="stick")
    rc, out, err, _ = tv.cli("enumerate")
    t = out["things"][0] if rc == 0 else {}
    check("stick: no volume steps where the TV reports NotSupported", rc == 0 and "volume_step" not in t["cap_names"],
          t.get("cap_names"))
    for change in ({"volume_step": 1}, {"key": "volume_up"}, {"key": "mute"}, {"mute": True}, {"key": "power"}):
        rc, out, err, _ = tv.apply(change)
        check("stick: %s is unsupported, so infrared can take over" % json.dumps(change), rc == 1 and fault(err) == "unsupported", err)
    st = m.stats()
    check("stick: nothing was sent for them (a stick would answer 200 and ignore them)",
          st["keys"] == [] and st["ignored"] == [], st["keys"])

    m.reset(state="awake", caps="tv_power")
    rc, out, err, _ = tv.cli("enumerate")
    rc, out, err, _ = tv.apply({"key": "power"})
    check("power key: sent when the TV reports power control", rc == 0 and keys(m.stats()) == ["power"], (rc, err))

    m.reset(state="awake", caps="levels_only")
    rc, out, err, _ = tv.cli("enumerate")
    t = out["things"][0] if rc == 0 else {}
    check("volume reported only by its level field still counts", "volume_step" in t.get("cap_names", []), t.get("cap_names"))
    m.reset(state="awake", caps="tv")
    tv.cli("enumerate")


def wake_tests(m, tv):
    m.reset(state="idle", caps="tv", wake_delay=0.9)
    rc, out, err, elapsed = tv.apply({"key": "home"}, deadline=2500)
    st = m.stats()
    probes = [r for r in st["requests"] if r["path"] == "/v1/FireTV/status"]
    check("idle TV: refused, DIAL wake, status polled, home sent once", rc == 0 and st["dial_posts"] == 1 and
          keys(st) == ["home"] and probes, (rc, err, st["dial_posts"], keys(st)))
    check("idle TV: the status poll sends no credentials (a 403 counts as awake)",
          all(r["api_key"] is None and r["token"] is None for r in probes), probes[:1])
    check("idle TV: done within the 2.5 s deadline", elapsed < 2.5, elapsed)
    check("idle TV: no magic packet for a key press", not st["magic"], st["magic"])

    m.reset(state="idle", caps="tv", wake_delay=0.9)
    rc, out, err, elapsed = tv.cli("read")
    st = m.stats()
    check("read on an idle TV: reachable false, and it is not woken",
          rc == 0 and out["things"][0]["state"]["reachable"] is False and st["dial_posts"] == 0, (rc, err))
    rc, out, err, elapsed = tv.cli("enumerate")
    st = m.stats()
    t = out["things"][0] if rc == 0 else {}
    check("enumerate on an idle TV: listed from what was known, not woken",
          rc == 0 and t["state"]["reachable"] is False and "volume_step" in t["cap_names"] and
          len(t["state"]["apps"]) == 6 and st["dial_posts"] == 0, (rc, err, t.get("cap_names")))

    for state in ("hung", "hung_tls"):
        m.reset(state=state, caps="tv")
        rc, out, err, elapsed = tv.apply({"key": "home"}, deadline=2500)
        st = m.stats()
        check("%s TV: reported as not serving, not as asleep" % state, rc == 1 and fault(err) == "not_serving" and
              not reads_as_unreachable(err), (rc, err))
        if state == "hung":
            # (A hung TV never parses requests, so its request log stays empty: count connections.)
            check("hung TV (the request went out, no answer): not woken and not sent again, since it may have acted",
                  st["dial_posts"] == 0 and st["tls_connections"] == 1 and "may still have been carried out" in err and
                  elapsed < 2.9, (st["dial_posts"], st["tls_connections"], err, elapsed))
        else:
            check("hung_tls TV (TLS never completed, nothing went out): the wake was tried once, within the deadline",
                  st["dial_posts"] == 1 and st["requests"] == [] and elapsed < 2.9, (st["dial_posts"], elapsed))

    for state in ("deep", "deep_blackhole"):
        m.reset(state=state, caps="tv", mac=MAC, boot_delay=1.5)
        rc, out, err, elapsed = tv.apply({"on": True}, deadline=9000)
        st = m.stats()
        check("%s: on=true sends the magic packet and the TV comes up" % state, rc == 0 and st["boots"] == 1 and
              any(p["valid"] and p["mac"] == MAC for p in st["magic"]), (rc, err, st["magic"]))
        check("%s: within the 9 s deadline" % state, elapsed < 9.0, elapsed)

    # A wake attempt has 2 s to connect, not 1 s (firehose's limit covers connecting and the
    # answer; review round 2). The TV here is up 0.3 s after the packet, but the attempt's first
    # connection request was dropped while it slept; the system repeats it after 1 s, and that
    # repeat is answered only if the attempt is still waiting to connect.
    m.reset(state="deep_blackhole", caps="tv", mac=MAC, boot_delay=0.3)
    rc, out, err, elapsed = tv.apply({"on": True}, deadline=9000)
    check("deep_blackhole, up 0.3 s after the packet: the first attempt, still connecting after 1 s, is answered (done in under 2.2 s)",
          rc == 0 and m.stats()["boots"] == 1 and elapsed < 2.2, (rc, err, elapsed))

    # A deep-standby TV as measured (firehose section 3: it answers 6.5-7.4 s after the first
    # packet), with the app's own deadline for every change (control.cpp: 2500 ms).
    m.reset(state="deep", caps="tv", mac=MAC, boot_delay=7.0)
    rc, out, err, elapsed = tv.apply({"on": True}, deadline=2500)
    st = m.stats()
    check("deep standby, on at the app's 2500 ms: 'waking' (a packet went out), never no_answer (no infrared toggle)",
          rc == 1 and fault(err) == "waking" and "wake-up signal" in err and not reads_as_unreachable(err) and
          any(p["valid"] and p["mac"] == MAC for p in st["magic"]) and elapsed < 2.9, (rc, err, st["magic"], elapsed))
    deadline_at = time.time() + 8
    while time.time() < deadline_at and m.stats()["boots"] == 0:
        time.sleep(0.25)
    check("deep standby: the TV does come on after that call returned", m.stats()["boots"] == 1, m.stats()["state"])
    rc, out, err, elapsed = tv.apply({"on": True}, deadline=2500)
    check("deep standby: on again once it is up: done", rc == 0, (rc, err))

    # The waking/no_answer verdict rests on a DIAL request made after the resume window, never on
    # the clock alone (review round 2). A repeat at 2500 ms that starts before the TV has resumed
    # and ends after it asks again just before its deadline, so a TV that came on during the call
    # is reached, and never called no_answer (the infrared power toggle would switch it off).
    for gap in (3500, 4000):
        m.reset(state="deep", caps="tv", mac=MAC, boot_delay=7.0)
        first, rc, err, elapsed = tv.apply_twice({"on": True}, gap, deadline=2500)
        st = m.stats()
        check("deep standby, up 7.0 s after the packet, on=true twice at 2500 ms %.1f s apart: 'waking', then done"
              % (gap / 1000.0), first == "waking" and rc == 0 and st["boots"] == 1, (first, rc, err, elapsed))
    # One call at the 9000 ms README-snippet recommends: the last 2.5 s slot is at 7.5 s, and the
    # call asks once more just before its deadline instead of idling to it.
    for boot in (7.6, 8.2):
        m.reset(state="deep", caps="tv", mac=MAC, boot_delay=boot)
        rc, out, err, elapsed = tv.apply({"on": True}, deadline=9000)
        st = m.stats()
        check("deep standby, up %.1f s after the packet, one on=true at 9000 ms: done by the last request before the deadline"
              % boot, rc == 0 and st["boots"] == 1 and elapsed < 9.0 and len(st["magic"]) == 4, (rc, err, elapsed))

    # The verdict is the latest request's, not the clock's: at 8200 ms the last 2.5 s slot (7.5 s)
    # is too close to the end for one more request, and its connection request, dropped while
    # the TV slept, is not repeated before 8.5 s. It shows nothing about 8 s, so a TV that comes
    # on at 7.8 s gets 'waking', never no_answer, though the call ends 8.2 s after the packet.
    m.reset(state="deep_blackhole", caps="tv", mac=MAC, boot_delay=7.8)
    rc, out, err, elapsed = tv.apply({"on": True}, deadline=8200)
    check("deep_blackhole, up 7.8 s after the packet, one on=true at 8200 ms: done or 'waking', never no_answer",
          (rc == 0 or fault(err) == "waking") and elapsed < 8.6, (rc, err, elapsed))

    # on=false while this driver's own wake-up signal may still be bringing the TV up: not
    # already_off (the TV would then come on and stay on), but 'waking' (review round 2).
    m.reset(state="deep", caps="tv", mac=MAC, boot_delay=7.0)
    first, rc, err, elapsed = tv.apply_twice({"on": True}, 0, deadline=2500, then={"on": False})
    check("on=false right after an on=true that sent a wake-up signal: 'waking', it was not switched off; never already_off",
          first == "waking" and rc == 1 and fault(err) == "waking" and "not switched off" in err and
          not reads_as_unreachable(err), (first, rc, err))
    deadline_at = time.time() + 8
    while time.time() < deadline_at and m.stats()["boots"] == 0:
        time.sleep(0.25)
    check("... and the TV does come on, as that answer said", m.stats()["boots"] == 1, m.stats()["state"])

    # Two TVs on one driver object, as in the app: one TV's unanswered wake-up signal does not
    # shorten another's waking window (the memory is per device; review round 2). The first TV
    # never answers (nothing listens on its ports); the second is the mock in deep standby.
    stray = Tv(m, info={"wakeup": "MAC=%s;Timeout=20" % OTHER_MAC, "dial_port": "1"}, port=1)
    stray.set_auth({k: v for k, v in tv.auth().items() if k != "wake_mac"})
    m.reset(state="deep", caps="tv", mac=MAC, boot_delay=7.0)
    first, rc, err, elapsed = tv.apply_twice({"on": True}, 6000, deadline=2500, first_device=stray)
    st = m.stats()
    check("two TVs, one driver: the second TV's waking window counts from its own wake-up signal (8 s after the first TV's)",
          first == "waking" and rc == 1 and fault(err) == "waking" and [p["mac"] for p in st["magic"]] == [OTHER_MAC, MAC],
          (first, rc, err, [p["mac"] for p in st["magic"]]))

    m.reset(state="deep", caps="tv", mac=MAC)
    rc, out, err, elapsed = tv.apply({"key": "home"}, deadline=2500)
    st = m.stats()
    check("deep sleep, a key press: no answer, so infrared can take over; no magic packet",
          rc == 1 and fault(err) == "no_answer" and not st["magic"] and elapsed < 2.9, (rc, err, st["magic"], elapsed))

    # What today's discovery.cpp records for a Fire TV: no "wakeup", no "c" (DISCOVERY.md changes
    # 2 and 3 are needed for that), but Device::mac from the ARP table. That address was never
    # announced as a wake address and is not used as one: no packet, and no 'waking'.
    nomac = Tv(m, info={"wakeup": None}, auth=dict(tv.auth(), wake_mac=None), mac=MAC)
    nomac.set_auth({k: v for k, v in tv.auth().items() if k != "wake_mac"})
    nomac.cli("enumerate")
    m.reset(state="deep", caps="tv", mac=MAC)
    rc, out, err, elapsed = nomac.apply({"on": True}, deadline=4000)
    st = m.stats()
    check("deep sleep, no wake address announced (as with today's discovery; Device::mac not used): no answer, no packet",
          rc == 1 and fault(err) == "no_answer" and "off or unplugged" in err and not st["magic"] and elapsed < 4.4 and
          "wake_mac" not in nomac.auth(), (rc, err, st["magic"], elapsed))
    check("deep sleep, no wake address: DIAL is retried until the deadline rather than given up at the first refusal",
          st["dial_posts"] == 0 and elapsed > 2.4, (st["dial_posts"], elapsed))

    # A TV that never comes on, with a stored wake address: the attempts and their packets are
    # 2.5 s apart (firehose WAKE_ATTEMPT_INTERVAL_MS); the last request, 0.3 s before the deadline
    # and 8 s or more after the first packet, gets nothing, so it is no_answer (infrared may take over).
    m.reset(state="gone", caps="tv", mac=MAC)
    rc, out, err, elapsed = tv.apply({"on": True}, deadline=9500)
    st = m.stats()
    times = [p["t"] for p in st["magic"]]
    gaps = [b - a for a, b in zip(times, times[1:])]
    check("never comes on, 9.5 s: one packet per wake attempt, 2.5 s apart (none with the last request)",
          len(times) == 4 and all(2.3 < g < 2.8 for g in gaps) and all(p["valid"] and p["mac"] == MAC for p in st["magic"]),
          (times, gaps))
    check("never comes on, 9.5 s: no_answer, from the last request just before the deadline (8 s after the first packet)",
          rc == 1 and fault(err) == "no_answer" and "wake-up signal" in err and 9.0 < elapsed < 10.5, (rc, err, elapsed))
    m.reset(state="gone", caps="tv", mac=MAC)
    first, rc, err, elapsed = tv.apply_twice({"on": True}, 6000, deadline=2500)
    check("never comes on, asked twice 6 s apart at 2500 ms: 'waking', then no_answer (counted from the first packet)",
          first == "waking" and rc == 1 and fault(err) == "no_answer" and len(m.stats()["magic"]) == 2 and elapsed < 12.5,
          (first, rc, err, elapsed))

    other = Tv(m, info={"wakeup": "MAC=%s;Timeout=20" % OTHER_MAC})
    other.set_auth(tv.auth())   # stored address: MAC
    other.cli("enumerate")
    check("a stored wake address is not replaced by a later announcement", other.auth().get("wake_mac") == MAC, other.auth())
    m.reset(state="deep", caps="tv", mac=MAC, boot_delay=0.5)
    rc, out, err, elapsed = other.apply({"on": True}, deadline=9000)
    st = m.stats()
    check("the magic packet goes to the stored address, not the announced one",
          rc == 0 and st["magic"] and all(p["mac"] == MAC for p in st["magic"]), (rc, err, st["magic"]))

    for state, quick in (("gone", True), ("gone_blackhole", False)):
        m.reset(state=state, caps="tv")
        rc, out, err, elapsed = tv.apply({"key": "home"}, deadline=2500)
        check("%s: no answer within the deadline (%.2f s)" % (state, elapsed),
              rc == 1 and fault(err) == "no_answer" and elapsed < (1.0 if quick else 2.9), (rc, err, elapsed))
    for state in ("gone", "deep", "gone_blackhole"):
        m.reset(state=state, caps="tv")
        rc, out, err, elapsed = tv.apply({"on": False})
        check("off on a TV where nothing answers (%s): already_off, never no_answer (an infrared toggle would switch it on)"
              % state, rc == 1 and fault(err) == "already_off" and elapsed < 2.9 and not m.stats()["magic"], (err, elapsed))
    # ... and the 'waking' answer to on=false lasts only while the TV could still be resuming.
    m.reset(state="gone", caps="tv", mac=MAC)
    first, rc, err, elapsed = tv.apply_twice({"on": True}, 6500, deadline=2500, then={"on": False})
    check("off asked 8 s or more after an unanswered wake-up signal, the TV never came on: already_off again",
          first == "waking" and rc == 1 and fault(err) == "already_off", (first, rc, err, elapsed))
    m.reset(state="gone", caps="tv")
    rc, out, err, elapsed = tv.apply({"on": True}, deadline=3000)
    check("on for a TV that never answers, at 3000 ms: 'waking', since the packet went out less than 8 s ago",
          rc == 1 and fault(err) == "waking" and "wake-up signal" in err and elapsed < 3.4 and m.stats()["magic"],
          (err, elapsed))

    # A wake address, but no packet can go out: a TV on an IPv6 address (Wake-on-LAN here is
    # IPv4 broadcast). Nothing is waking it, so not 'waking' (review round 2). Nothing listens on
    # [::1]:1, so both ports refuse.
    v6 = Tv(m, ip="::1", port=1, info={"dial_port": "1"})
    v6.set_auth(dict(tv.auth(), wake_mac=MAC))
    m.reset(state="deep", caps="tv", mac=MAC)
    rc, out, err, elapsed = v6.apply({"on": True}, deadline=2500)
    check("a TV on an IPv6 address with a wake address: no packet can go out, so no_answer ('may be off'), not 'waking'",
          rc == 1 and fault(err) == "no_answer" and "off or unplugged" in err and not m.stats()["magic"], (rc, err))

    # on=true combined with a key (review round 2): the wake keeps time back for the key, and a key
    # left with no time is not sent and reported as 'waking', never as no_answer (the infrared
    # power toggle would switch the TV off) or not_serving (it answered). Every API handshake
    # takes 0.12 s; the API opens 1.9-2.3 s after the DIAL wake.
    results = []
    for delay in (1.9, 2.0, 2.1, 2.2, 2.3):
        m.reset(state="idle", caps="tv", wake_delay=delay, slow_tls_every=1, slow_tls_s=0.12)
        rc, out, err, elapsed = tv.apply({"on": True, "key": "home"}, deadline=2500)
        results.append((delay, rc, fault(err), keys(m.stats()), round(elapsed, 2)))
    check("on=true + a key, idle TV waking late, slow handshakes: done, or 'waking' with the key not sent; never no_answer or not_serving",
          all((rc == 0 and ks == ["home"]) or (f == "waking" and ks == []) for _, rc, f, ks, _ in results) and
          results[0][1] == 0 and all(e < 2.9 for *_, e in results), results)
    # The wake stops polling 300 ms before the deadline when a key follows, so the key is never
    # started into the last 300 ms, where a TV that answers 0.25 s late could carry it out and
    # still leave it unconfirmed. Here the API opens 2.15 s after the DIAL wake; the poll that
    # would see it is due at about 2.26 s.
    m.reset(state="idle", caps="tv", wake_delay=2.15, late=0.25)
    rc, out, err, elapsed = tv.apply({"on": True, "key": "home"}, deadline=2500)
    st = m.stats()
    check("on=true + a key, the API up only in the last 300 ms: 'waking', the key not started; never not_serving",
          rc == 1 and fault(err) == "waking" and keys(st) == [] and st["dial_posts"] == 1, (rc, err, keys(st), elapsed))


def starting_tests(m, tv):
    """A TV whose API, after the TV took the DIAL wake, accepts connections and says nothing for
    a while (ZB-060: the owner's TV, from standby, came on while the app reported a failure)."""
    for mode, delay in (("silent", 0.3), ("silent_tls", 0.0)):
        m.reset(state="idle", caps="tv", wake_delay=delay, starting_s=6.0, starting_mode=mode)
        rc, out, err, elapsed = tv.apply({"on": True}, deadline=2500)
        st = m.stats()
        check("starting (%s): on=true is done once the TV took the DIAL wake, though its API is still silent" % mode,
              rc == 0 and st["dial_posts"] == 1 and st["wakes"] == 1 and st["state"] == "starting" and
              st["silent_connections"] >= 1 and keys(st) == [] and elapsed < 2.9,
              (rc, err, st["dial_posts"], st["state"], st["silent_connections"], elapsed))

    # The very next command, while the API is still silent: "still starting up", never "not
    # replying", and nothing that would send the app looking for the TV elsewhere. A request that
    # went out (TLS completed) may still be carried out, so it says so instead of "try again".
    for mode, ends, dials in (("silent", "The command may still have been carried out, so check the TV before trying again.", 1),
                              ("silent_tls", "Try again in a moment.", 2)):
        m.reset(state="idle", caps="tv", wake_delay=0.3, starting_s=8.0, starting_mode=mode)
        first, rc, err, elapsed = tv.apply_twice({"on": True}, 200, then={"key": "home"})
        st = m.stats()
        check("starting (%s): the next command, while the API is still silent, says the TV is still starting up" % mode,
              first == "none" and rc == 1 and fault(err) == "starting" and
              message(err).startswith("The TV is still starting up. ") and message(err).endswith(ends) and
              not reads_as_unreachable(err), (first, rc, err))
        check("starting (%s): ... nothing was carried out, and no request was sent twice" % mode,
              keys(st) == [] and st["requests"] == [] and st["dial_posts"] == dials and st["state"] == "starting",
              (keys(st), st["dial_posts"], st["state"]))

    # A key press that wakes the TV itself (its API refused), whose API then starts slowly: the
    # same answer, and so for the next command.
    m.reset(state="idle", caps="tv", wake_delay=0.3, starting_s=8.0)
    first, rc, err, elapsed = tv.apply_twice({"key": "home"}, 200, then={"key": "back"})
    st = m.stats()
    check("starting: a key press that woke the TV, whose API then stays silent, and the next one: 'still starting up' both",
          first == "starting" and rc == 1 and fault(err) == "starting" and keys(st) == [] and st["dial_posts"] == 1,
          (first, rc, err, keys(st), st["dial_posts"]))

    # Once the API is up, commands work (no new wake); and once it has answered, the TV is no
    # longer "starting": a silent API is "not serving" again.
    m.reset(state="idle", caps="tv", wake_delay=0.2, starting_s=3.0, faults={"POST /v1/FireTV": "silent"},
            fault_after={"POST /v1/FireTV": 1})
    earlier, rc, err, elapsed = tv.apply_seq([{"on": True}, {"key": "home"}, {"key": "back"}], 1500)
    st = m.stats()
    check("starting: on=true while the API is silent, then a key once it is up: carried out, no new wake",
          len(earlier) == 2 and earlier[0]["ok"] and earlier[1]["ok"] and keys(st) == ["home"] and st["dial_posts"] == 1,
          (earlier, keys(st), st["dial_posts"]))
    check("starting: after the API has answered, a silent API is 'not serving' again, not 'starting'",
          rc == 1 and fault(err) == "not_serving" and "may still have been carried out" in err, (rc, err))

    # An API stuck before any wake: the wake a command sends then (DIAL answers) starts nothing.
    m.reset(state="hung_tls", caps="tv")
    first, rc, err, elapsed = tv.apply_twice({"key": "home"}, 0, then={"key": "back"})
    check("a stuck API (hung_tls): a command's DIAL wake does not make it 'starting'; not serving, twice",
          first == "not_serving" and rc == 1 and fault(err) == "not_serving", (first, rc, err))

    # Power and more while the API is silent after the wake: the TV took the wake, but the rest
    # needs the API, so the change fails as "still starting up" and nothing else is sent.
    for mode, extra in (("silent", {"key": "home"}), ("silent_tls", {"source": "Netflix"})):
        m.reset(state="idle", caps="tv", wake_delay=0.3, starting_s=6.0, starting_mode=mode)
        rc, out, err, elapsed = tv.apply(dict({"on": True}, **extra))
        st = m.stats()
        check("starting (%s): on=true with a %s fails as 'still starting up, try again', nothing else sent" %
              (mode, list(extra)[0]),
              rc == 1 and fault(err) == "starting" and message(err) == "The TV is still starting up. Try again in a moment." and
              keys(st) == [] and st["launched"] == [] and st["dial_posts"] == 1 and st["state"] == "starting",
              (rc, err, keys(st), st["launched"], st["dial_posts"], st["state"]))

    # The API answered in this call and then went silent: that is not a TV starting up.
    m.reset(state="idle", caps="tv", wake_delay=0.3, faults={"POST /v1/FireTV": "silent"})
    rc, out, err, elapsed = tv.apply({"on": True, "key": "home"})
    check("on=true + a key, the API up after the wake and then silent to the key: not serving, not 'starting'",
          rc == 1 and fault(err) == "not_serving" and "may still have been carried out" in err, (rc, err))

    # on=true again while the TV is starting, its DIAL request accepted and not answered: still
    # starting up, not "not replying" (the sentence after which the assistant offered an infrared
    # power toggle). Without an accepted wake before, that stays not serving (tested below).
    m.reset(state="idle", caps="tv", wake_delay=0.3, starting_s=8.0, faults={"POST /apps/FireTVRemote": "silent"},
            fault_after={"POST /apps/FireTVRemote": 1})
    first, rc, err, elapsed = tv.apply_twice({"on": True}, 300)
    check("starting: on=true again while the TV starts, its DIAL request accepted and unanswered: 'still starting up'",
          first == "none" and rc == 1 and fault(err) == "starting" and not reads_as_unreachable(err), (first, rc, err))

    # A TV that did not take the wake fails as before, and is not "starting" afterwards.
    m.reset(state="hung", caps="tv", faults={"POST /apps/FireTVRemote": "500"})
    first, rc, err, elapsed = tv.apply_twice({"on": True}, 200, then={"key": "home"})
    st = m.stats()
    check("a DIAL wake answered 500: on=true fails as before (not serving); the next command's silence is not 'starting'",
          first == "not_serving" and rc == 1 and fault(err) == "not_serving" and st["dial_posts"] == 1 and keys(st) == [],
          (first, rc, err, st["dial_posts"]))
    m.reset(state="idle", caps="tv", faults={"POST /apps/FireTVRemote": "silent"})
    rc, out, err, elapsed = tv.apply({"on": True}, deadline=2500)
    st = m.stats()
    check("a DIAL wake that is never answered: on=true fails as before (not serving), the TV stays idle",
          rc == 1 and fault(err) == "not_serving" and st["wakes"] == 0 and st["state"] == "idle" and elapsed < 2.9,
          (rc, err, st["wakes"], elapsed))
    nomac = Tv(m, info={"wakeup": None})
    nomac.set_auth({k: v for k, v in tv.auth().items() if k != "wake_mac"})
    m.reset(state="gone", caps="tv")
    rc, out, err, elapsed = nomac.apply({"on": True}, deadline=2500)
    check("nothing answers DIAL and no wake address: on=true fails, no_answer, as before",
          rc == 1 and fault(err) == "no_answer" and elapsed < 2.9, (rc, err, elapsed))

    # A changed key that cannot be checked still fails on=true, though the TV took the wake: the
    # API comes up 0.3 s after it presenting key b, and the one check of it gets a 500.
    m.reset(state="idle", caps="tv", wake_delay=0.3, cert="b", faults={"GET /v1/FireTV/status": "500"})
    rc, out, err, elapsed = tv.apply({"on": True}, deadline=2500)
    st = m.stats()
    check("on=true, the TV took the wake but its API's new key could not be checked: fails as before (key_changed)",
          rc == 1 and fault(err) == "key_changed" and st["dial_posts"] == 1 and
          not tv.state().get("secrets", {}).get("fire_tv", {}).get("rekeyed"), (rc, err, st["dial_posts"]))
    m.reset(state="awake", caps="tv", cert="a")

    # Deep standby keeps its timing (Wake-on-LAN, then DIAL every 2.5 s), with an API that starts
    # 5 s after the boot: at the app's 2500 ms 'waking' (no DIAL answer yet), then done once it has
    # booted, its API still silent; in one call at 9000 ms, done by the DIAL answer at 7.5 s.
    m.reset(state="deep", caps="tv", mac=MAC, boot_delay=7.0, starting_s=5.0)
    first, rc, err, elapsed = tv.apply_twice({"on": True}, 6000, deadline=2500)
    st = m.stats()
    check("deep standby, API slow after the boot, on=true at 2500 ms twice 6 s apart: 'waking', then done",
          first == "waking" and rc == 0 and st["boots"] == 1 and st["state"] == "starting", (first, rc, err, st["state"]))
    m.reset(state="deep", caps="tv", mac=MAC, boot_delay=7.0, starting_s=5.0)
    rc, out, err, elapsed = tv.apply({"on": True}, deadline=9000)
    st = m.stats()
    check("deep standby, API slow after the boot, one on=true at 9000 ms: done, four packets 2.5 s apart as before",
          rc == 0 and st["boots"] == 1 and len(st["magic"]) == 4 and st["state"] == "starting" and elapsed < 9.6,
          (rc, err, len(st["magic"]), st["state"], elapsed))
    m.reset(state="awake", caps="tv")


def auth_failure_tests(m, tv):
    m.reset(state="awake", caps="tv", revoked=True)
    rc, out, err, _ = tv.apply({"key": "home"})
    check("a token the TV refuses: pair again, nothing pressed", rc == 1 and fault(err) == "pair_again" and
          m.stats()["keys"] == [] and m.stats()["dial_posts"] == 0, err)
    rc, out, err, _ = tv.cli("enumerate")
    check("enumerate with a refused token: pair again", rc == 1 and fault(err) == "pair_again", err)
    rc, out, err, _ = tv.cli("read")
    check("read with a refused token: pair again", rc == 1 and fault(err) == "pair_again", err)

    # A TV that presents another TLS key (a Fire TV can change it, for example at a restart): as
    # the owner chose, the key is accepted when the TV still accepts the pairing, checked with one
    # status read that changes nothing; the command then goes through, and the key is remembered.
    pin_a = tv.auth()["key_pin"]
    m.reset(state="awake", caps="tv", cert="b")
    rc, out, err, _ = tv.apply({"key": "home"})
    st = m.stats()
    check("a TV with a new TLS key that still accepts the pairing: one status check, then the press goes through",
          rc == 0 and keys(st) == ["home"] and len(api_requests(st, "/v1/FireTV/status")) == 1 and st["dial_posts"] == 0,
          (rc, err, keys(st), [r["path"] for r in st["requests"]]))
    rekeyed = tv.state().get("secrets", {}).get("fire_tv", {}).get("rekeyed", {})
    check("... the new key is remembered for this TV (in the secrets) while the pairing still holds the old one",
          len(rekeyed) == 1 and list(rekeyed.values())[0].get("from") == pin_a and tv.auth()["key_pin"] == pin_a, rekeyed)
    m.reset(state="awake", caps="tv", cert="b")
    rc, out, err, _ = tv.apply({"key": "back"})
    st = m.stats()
    check("... so the next command needs no check", rc == 0 and keys(st) == ["back"] and
          not api_requests(st, "/v1/FireTV/status"), (rc, err, [r["path"] for r in st["requests"]]))
    rc, out, err, _ = tv.cli("enumerate")
    rekeyed = tv.state().get("secrets", {}).get("fire_tv", {}).get("rekeyed", {})
    check("enumerate writes the new key into the pairing and forgets the note",
          rc == 0 and tv.auth()["key_pin"] != pin_a and not rekeyed, (rc, err, rekeyed))
    pin_b = tv.auth()["key_pin"]
    # A new key and a pairing the TV no longer accepts: pair again, and nothing is pressed.
    m.reset(state="awake", caps="tv", cert="a", revoked=True)
    rc, out, err, _ = tv.apply({"key": "home"})
    st = m.stats()
    check("a new key and a refused pairing: pair again, nothing pressed, no wake, the key not taken",
          rc == 1 and fault(err) == "pair_again" and keys(st) == [] and st["dial_posts"] == 0 and
          not tv.state().get("secrets", {}).get("fire_tv", {}).get("rekeyed") and tv.auth()["key_pin"] == pin_b, (rc, err))
    # A new key on a TV that is asleep: the wake goes out, the key is checked when the API comes up.
    m.reset(state="idle", caps="tv", cert="a")
    rc, out, err, _ = tv.apply({"on": True}, deadline=9000)
    st = m.stats()
    check("on=true, an idle TV with a new key: woken, the key checked and accepted, reported as on",
          rc == 0 and st["dial_posts"] >= 1 and len(api_requests(st, "/v1/FireTV/status")) >= 1, (rc, err, st["dial_posts"]))
    # Back to key a for the tests that follow.
    rc, out, err, _ = tv.cli("enumerate")
    check("enumerate on key a again", rc == 0 and tv.auth()["key_pin"] == pin_a, (rc, err))
    m.reset(state="awake", caps="tv", cert="a")


def hostile_reply_tests(m, tv):
    m.reset(state="awake", caps="tv")
    tv.cli("enumerate")
    before = tv.thing()

    for endpoint, mode in (("GET /v1/FireTV", "garbage"), ("GET /v1/FireTV", "huge"), ("GET /v1/FireTV", "notjson"),
                           ("GET /v1/FireTV", "truncated"), ("GET /v1/FireTV", "deep"), ("GET /v1/FireTV", "hugeheader"),
                           ("GET /v1/FireTV", "drip"), ("GET /v1/FireTV", "close"),
                           ("GET /v1/FireTV/appsV2", "huge"), ("GET /v1/FireTV/appsV2", "drip"),
                           ("GET /v1/FireTV/appsV2", "deep"), ("GET /v1/FireTV/appsV2", "wrongtypes"),
                           ("GET /v1/FireTV/appsV2", "garbage"), ("GET /v1/FireTV/appsV2", "truncated")):
        m.reset(state="awake", caps="tv", faults={endpoint: mode})
        rc, out, err, elapsed = tv.cli("enumerate")
        t = out["things"][0] if rc == 0 and isinstance(out, dict) else {}
        check("hostile reply, %s %s: enumerate survives within 4 s, keeps what it knew" % (endpoint, mode),
              rc == 0 and elapsed < 4.5 and set(t["cap_names"]) == set(before["cap_names"]) and
              t["state"]["apps"] == before["state"]["apps"], (rc, err, elapsed, t.get("cap_names")))

    m.reset(state="awake", caps="tv", faults={"GET /v1/FireTV": "wrongtypes"})
    rc, out, err, _ = tv.cli("enumerate")
    t = out["things"][0] if rc == 0 else {}
    check("hostile reply: capability fields of the wrong type grant nothing", rc == 0 and "volume_step" not in t["cap_names"],
          t.get("cap_names"))
    m.reset(state="awake", caps="tv")
    tv.cli("enumerate")

    m.reset(state="awake", caps="tv", faults={"GET /v1/FireTV/appsV2": "badutf8"})
    rc, out, err, _ = tv.cli("enumerate")
    apps = out["things"][0]["state"]["apps"] if rc == 0 else []
    check("hostile reply: an app name that is not UTF-8 is repaired, the rest kept",
          rc == 0 and apps == ["Bad �� name", "Netflix"], apps)
    m.reset(state="awake", caps="tv")
    tv.cli("enumerate")

    m.reset(state="awake", caps="tv", faults={"POST /v1/FireTV": "500"})
    rc, out, err, _ = tv.apply({"key": "home"})
    check("a command the TV answers with 500: refused, and not a reason to wake it",
          rc == 1 and fault(err) == "other" and m.stats()["dial_posts"] == 0, err)
    for mode in ("silent", "close", "garbage"):
        m.reset(state="awake", caps="tv", faults={"POST /v1/FireTV": mode})
        rc, out, err, elapsed = tv.apply({"key": "home"}, deadline=2500)
        st = m.stats()
        check("a command that went out and was answered with '%s': sent once, no wake, not serving, within the deadline"
              % mode, rc == 1 and fault(err) == "not_serving" and st["dial_posts"] == 0 and
              len(api_requests(st, "/v1/FireTV", "POST")) == 1 and "may still have been carried out" in err and
              elapsed < 2.9, (rc, err, st["dial_posts"], elapsed))

    # A busy TV that carries a command out at once and answers late: the command must reach it
    # exactly once (a toggle sent twice undoes itself; a volume step or a D-pad move doubles).
    m.reset(state="awake", caps="tv", late=1.7)
    rc, out, err, elapsed = tv.apply({"transport": "play"})
    rc2, out2, err2, _ = tv.apply({"key": "up"})
    st = m.stats()
    check("an answer 1.7 s late, within the deadline: success, each command carried out once",
          rc == 0 and rc2 == 0 and keys(st) == ["media:play", "dpad_up"] and st["playing"] is True and st["dial_posts"] == 0,
          (err, err2, keys(st)))
    # A later part of a change left with under 200 ms is not sent (review round 2): it could only
    # fail for lack of time, or leave it unknown whether the TV carried it out.
    m.reset(state="awake", caps="tv", late=2.35)
    rc, out, err, elapsed = tv.apply({"key": "home", "transport": "play"})
    st = m.stats()
    check("a later part of a change with under 200 ms left is not sent: 'no time left', not no_answer or not_serving",
          rc == 1 and fault(err) == "other" and message(err) == "There was no time left to finish that. Try again in a moment."
          and keys(st) == ["home"] and st["dial_posts"] == 0 and not reads_as_unreachable(err), (rc, err, keys(st)))
    # The app launch gets the whole remaining deadline too, not the 1.5 s cap of a list read
    # (review round 2: this path had no test).
    m.reset(state="awake", caps="tv", late=1.7)
    rc, out, err, elapsed = tv.apply({"source": "Netflix"})
    st = m.stats()
    check("an app launch answered 1.7 s late, within the deadline: success, launched once, no wake",
          rc == 0 and st["launched"] == ["com.netflix.ninja"] and st["dial_posts"] == 0 and
          len(api_requests(st, "/v1/FireTV/app/com.netflix.ninja")) == 1, (rc, err, st["launched"], elapsed))
    for change, executed in (({"transport": "play"}, ["media:play"]), ({"volume_step": 1}, ["volume_up"]),
                             ({"key": "mute"}, ["mute"])):
        m.reset(state="awake", caps="tv", late=3.0)
        rc, out, err, elapsed = tv.apply(change)
        st = m.stats()
        check("an answer later than the deadline, %s: not serving, carried out once, never resent or woken" % json.dumps(change),
              rc == 1 and fault(err) == "not_serving" and "may still have been carried out" in err and keys(st) == executed and
              st["dial_posts"] == 0 and elapsed < 2.9, (rc, err, keys(st), st["dial_posts"], elapsed))
    m.reset(state="awake", caps="tv_power")
    tv.cli("enumerate")
    m.reset(state="awake", caps="tv_power", late=3.0)
    rc, out, err, elapsed = tv.apply({"key": "power"})
    st = m.stats()
    check("an answer later than the deadline, the power key: carried out once (never toggled back)",
          rc == 1 and fault(err) == "not_serving" and keys(st) == ["power"] and st["display"] == "off", (rc, err, keys(st)))
    m.reset(state="awake", caps="tv", late=3.0)
    rc, out, err, elapsed = tv.apply({"source": "Netflix"})
    st = m.stats()
    check("an answer later than the deadline, an app launch: launched once",
          rc == 1 and fault(err) == "not_serving" and st["launched"] == ["com.netflix.ninja"], (rc, err, st["launched"]))
    m.reset(state="awake", caps="tv")
    tv.cli("enumerate")

    # What a changed or unknown API answers (firehose section 2: 405 for routes a TV does not
    # serve; the forum: 400 "Bad arguments supplied"): unsupported, so infrared can take over.
    for endpoint, mode, change, name in (("POST /v1/FireTV", "404", {"key": "home"}, "the home key"),
                                         ("POST /v1/FireTV", "405", {"key": "back"}, "the back key"),
                                         ("POST /v1/FireTV", "400", {"key": "up"}, "the up key"),
                                         ("POST /v1/media", "405", {"transport": "pause"}, "play and pause"),
                                         ("POST /v1/media", "400", {"key": "rewind"}, "rewind"),
                                         ("POST /v1/FireTV", "404", {"on": False}, "switching off")):
        m.reset(state="awake", caps="tv", faults={endpoint: mode})
        rc, out, err, _ = tv.apply(change)
        check("%s answered %s: unsupported (%s), so infrared can take over" % (json.dumps(change), mode, name),
              rc == 1 and fault(err) == "unsupported" and message(err).endswith(name + ".") and m.stats()["dial_posts"] == 0,
              err)
    m.reset(state="awake", caps="tv", faults={"POST /v1/FireTV/app/com.netflix.ninja": "400"})
    rc, out, err, _ = tv.apply({"source": "Netflix"})
    check("an app launch answered 400: refused (the TV may not have it), not unsupported", rc == 1 and fault(err) == "other",
          err)
    m.reset(state="awake", caps="tv", faults={"POST /v1/FireTV/app/com.netflix.ninja": "404"})
    rc, out, err, _ = tv.apply({"source": "Netflix"})
    check("an app launch answered 404: unsupported (opening apps)", rc == 1 and fault(err) == "unsupported" and
          "opening apps" in err, err)
    m.reset(state="awake", caps="tv", faults={"GET /v1/FireTV/status": "hugeheader"})
    rc, out, err, elapsed = tv.cli("read")
    check("read with a 300 kB header: survives quickly (its 200 status line counts as an answer)",
          rc == 0 and out["things"][0]["state"]["reachable"] is True and elapsed < 2.6, (rc, err, elapsed))
    m.reset(state="awake", caps="tv", faults={"GET /v1/FireTV/status": "drip"})
    rc, out, err, elapsed = tv.cli("read")
    check("read with a reply that drips in: ends within its 2.5 s", rc == 0 and elapsed < 2.6, (rc, err, elapsed))
    m.reset(state="awake", caps="tv", faults={"GET /v1/FireTV/status": "garbage"})
    rc, out, err, elapsed = tv.cli("read")
    check("read with a reply that is not HTTP: reported unreachable", rc == 0 and out["things"][0]["state"]["reachable"] is False,
          (rc, err))
    m.reset(state="awake", caps="tv")


def unknown_device_tests(m, tv):
    st = tv.state()
    other = Tv(m, kind="android_tv")
    other.set_auth(tv.auth())
    rc, out, err, _ = other.cli("claims")
    check("an Android TV is not claimed", rc == 0 and out == {"claims": False}, out)
    m.reset(state="awake", caps="tv")
    rc, out, err, _ = other.cli("enumerate")
    check("a device of another kind is ignored: nothing is sent to it", rc == 1 and m.stats()["requests"] == [], (rc, err))
    rc, out, err, _ = tv.cli("claims")
    check("a fire_tv device is claimed", rc == 0 and out == {"claims": True}, out)
    del st

    # info "api_port" names the API port and wins over Device::port (which, off loopback, holds
    # discovery's Whisperlink SRV port and is not followed).
    other = Tv(m, info={"api_port": str(m.api)}, port=1)
    other.set_auth(tv.auth())
    m.reset(state="awake", caps="tv")
    rc, out, err, _ = other.cli("enumerate")
    check("info api_port points the driver at the API, over Device::port", rc == 0 and m.stats()["requests"] and
          out["things"][0]["state"]["reachable"] is True, (rc, err))


def main():
    if not os.access(CLI, os.X_OK):
        sys.exit("no firetv_cli at %s (run tests/run_unit.sh first)" % CLI)
    groups = sys.argv[2:]
    wanted = lambda name: not groups or name in groups
    m = Mock()
    tv = pairing_tests(m)
    if wanted("wrong_code"):
        wrong_code_tests(m)
    enumerate_tests(m, tv)   # always: it reads the capabilities and apps the other groups rely on
    if wanted("command"):
        command_tests(m, tv)
    if wanted("capability"):
        capability_variant_tests(m, tv)
    if wanted("wake"):
        wake_tests(m, tv)
    if wanted("starting"):
        starting_tests(m, tv)
    if wanted("auth"):
        auth_failure_tests(m, tv)
    if wanted("hostile"):
        hostile_reply_tests(m, tv)
    if wanted("unknown"):
        unknown_device_tests(m, tv)

    tv.cli("unpair")
    check("unpair: the pairing is forgotten", tv.auth() == {} and tv.cli("step")[1]["kind"] == "accept_prompt", tv.auth())
    tokens = m.stats()["tokens"]
    leaked = [t for t in tokens if any(t in text for text in seen_output)]
    check("no token appears in anything the tool printed", tokens and not leaked, leaked)
    mode = oct(os.stat(tv.path).st_mode & 0o777)
    check("the state file holding the token is private (0600)", mode == "0o600", mode)


if __name__ == "__main__":
    try:
        main()
    finally:
        for mock in mocks:
            mock.stop()
        shutil.rmtree(workdir, ignore_errors=True)
    print("%d checks, %d failed" % (run_count, fail_count))
    sys.exit(1 if fail_count else 0)
