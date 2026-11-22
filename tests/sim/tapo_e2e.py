#!/usr/bin/env python3
"""Tapo end to end: the command-line build of the app against two pretend plugs on 127.0.0.1.

A P110 that logs in with TPAP and a P304M power strip that logs in with KLAP are started from
tests/mocks/mock_tapo.py on ports the system picks. The app runs on the stand-in test network
(ZB_TEST_NETWORK=1, where only 127.x addresses are ever contacted) with its state in a fresh
temporary folder, from a smart-home file in the old format (switches, top-level Tapo account).
The plugs' own counters (GET /mock/stats) show what reached them, so "logged in once" and "not
tried again" are checked from the plug's side.

usage: tests/sim/tapo_e2e.py [path/to/zero_bot]          (default: build/cli/zero_bot)
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request

TESTS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # tests/: netguard.py, mocks/
sys.path.insert(0, TESTS)
import netguard   # noqa: E402
netguard.sanitize()   # no key or proxy of the caller's; the app pinned (ZB_EXP_NET) and on the stand-in network

APP = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(TESTS, "..", "build", "cli", "zero_bot")
EMAIL, PASSWORD = "user@example.com", "hunter2"
SHA1 = hashlib.sha1(PASSWORD.encode()).hexdigest()
WRONG = hashlib.sha1(b"not the password").hexdigest()
OWNER = hashlib.md5(EMAIL.encode()).hexdigest().upper()
PLUG, STRIP = "Mock Plug ☕", "Mock Strip"

checks = failed = 0
outputs = []


def check(name, ok, detail=""):
    global checks, failed
    checks += 1
    failed += not ok
    print("%s  %s%s" % ("ok  " if ok else "FAIL", name, "" if ok else "  -> %r" % (detail,)), flush=True)


class Mock:
    def __init__(self, mode, model, mac, nickname=""):
        self.proc = subprocess.Popen([sys.executable, "-I", os.path.join(TESTS, "mocks", "mock_tapo.py"), "--port", "0", "--mode", mode,
                                      "--model", model, "--email", EMAIL, "--password", PASSWORD, "--mac", mac,
                                      "--nickname", nickname, "--stdin-lifeline"],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=netguard.mock_env())
        line = self.proc.stdout.readline().split()
        if line[:2] != ["MOCK", "listening"]:
            raise RuntimeError("the mock did not start: %r" % line)
        self.port = int(line[2])

    def stats(self):
        with urllib.request.urlopen("http://127.0.0.1:%d/mock/stats" % self.port, timeout=5) as r:
            return json.load(r)

    def reset(self, cfg):
        """The plug's test hook: its state and counters cleared, and the behaviour in `cfg` (mock_tapo.py)."""
        req = urllib.request.Request("http://127.0.0.1:%d/mock/reset" % self.port, data=json.dumps(cfg).encode(), method="POST")
        with urllib.request.urlopen(req, timeout=5) as r:
            r.read()

    def stop(self):
        self.proc.kill()
        self.proc.wait()


class App:
    def __init__(self):
        self.state = tempfile.mkdtemp(prefix="zb-tapo-e2e-")
        self.env = dict(os.environ, XDG_CONFIG_HOME=self.state + "/config", XDG_DATA_HOME=self.state + "/data",
                        XDG_CACHE_HOME=self.state + "/cache", ZB_TEST_NETWORK="1", ZB_NO_AUDIO="1")
        self.env.pop("ZB_DRY_RUN", None)
        self.file = self.state + "/data/zero-bot/smarthome.json"

    def run(self, *args):
        p = subprocess.run([APP, *args], env=self.env, capture_output=True, text=True, timeout=60)
        outputs.append(p.stdout + p.stderr)
        return p.stdout

    def tool(self, args):
        out = self.run("--tool", "smart_home", json.dumps(args, ensure_ascii=False)).strip()
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


def old_file(network, plug_port, strip_port):
    """What the app saved before things existed: switches per device, the account at the top."""
    tpap = json.dumps({"tls": 0, "dac": 0, "noc": 0, "pake": [2], "port": 80})
    return {"tapo": {"email": EMAIL, "password_sha1": SHA1},
            "networks": {network: {"label": "Test network", "gateway_mac": "02:00:00:00:00:00", "allowed_at": 1, "last_scan": 1,
                                   "devices": [
                {"id": "tapo:p110", "kind": "tapo", "name": "Tapo P110", "model": "P110(UK)", "mac": "aa:bb:cc:00:00:01", "last_ip": "127.0.0.1",
                 "port": plug_port, "info": {"scheme": "TPAP", "tpap": tpap, "device_type": "SMART.TAPOPLUG", "owner": OWNER},
                 "found_by": ["tapo"], "enabled": True, "last_seen": 1, "switches": [], "problem": ""},
                {"id": "tapo:p304m", "kind": "tapo", "name": "Tapo P304M", "model": "P304M(UK)", "mac": "AA-BB-CC-00-00-02", "last_ip": "127.0.0.1",
                 "port": strip_port, "info": {"scheme": "KLAP", "device_type": "SMART.TAPOPLUG", "owner": OWNER},
                 "found_by": ["tapo"], "enabled": True, "last_seen": 1, "switches": [], "problem": ""}]}}}


def nested_too_deep(app, plug, network):
    """Answers nested deeper than any plug's (ZB-093). Each is otherwise right, and before the guard was taken
    as it came. None is read now: the plug answered wrongly (it is not gone), and no password is held against
    it. The TPAP plug is left on, with normal answers."""
    def saved_tpap(text):
        root = app.load()
        next(d for d in root["networks"][network]["devices"] if d["id"] == "tapo:p110")["info"]["tpap"] = text
        app.save(root)

    tpap = json.dumps({"tls": 0, "dac": 0, "noc": 0, "pake": [2], "port": 80})
    saved_tpap(tpap[:-1] + ',"pad":' + "[" * 40 + "]" * 40 + "}")
    before = plug.stats()["discover"]
    r = app.tool({"action": "off", "target": PLUG})
    check("a saved TPAP announcement nested too deep is not used: the plug is asked again, and switches",
          r == {"now": "off", "target": PLUG} and plug.stats()["discover"] == before + 1 and plug.stats()["on"] is False,
          (r, plug.stats()))
    saved_tpap(tpap)
    plug.reset({"deep": "login"})
    r = app.tool({"action": "on", "target": PLUG})
    st = plug.stats()
    check("login answers nested too deep are not read: the login stops at its first step, nothing is switched",
          isinstance(r, str) and "the plug answered the login unexpectedly" in r and st["pake_register"] == 1 and
          st["pake_share"] == 0 and st["on"] is False and st["bad"] == [], (r, st))
    check("that is no refusal of the password", app.load()["secrets"]["tapo"].get("refused") is None, app.load()["secrets"]["tapo"])
    plug.reset({"deep": "reply"})
    r = app.tool({"action": "on", "target": PLUG})
    st = plug.stats()
    check("command answers nested too deep are not read: after the first, nothing more is sent",
          isinstance(r, str) and "the plug's answer cannot be read" in r and st["on"] is False and
          set(st["methods"]) == {"get_device_info"}, (r, st))
    plug.reset({"deep": "lost", "budget": [1, 5]})
    r = app.tool({"action": "on", "target": PLUG})
    st = plug.stats()
    check("a 'session ended' notice nested too deep is not taken as one: no second login, nothing switched",
          isinstance(r, str) and "the plug's answer could not be decrypted" in r and st["login_ok"] == 1 and st["gone"] == 1 and
          st["on"] is False, (r, st))
    plug.reset({})
    r = app.tool({"action": "on", "target": PLUG})
    check("answers as a plug sends them are read again at once", r == {"now": "on", "target": PLUG} and plug.stats()["on"] is True and
          app.load()["secrets"]["tapo"].get("refused") is None, r)


def locked_by_another(app, plug, strip):
    """A plug another app has locked says so at its first login step, before any proof of the password (fix round 4,
    E3). That is the plug's own state, not a refusal of the account: nothing is noted as refused, and the account's
    other plug still logs in. It was first kept as the account's refusal, which stopped every plug."""
    plug.reset({"locked": True})
    r = app.tool({"action": "on", "target": PLUG})
    st = plug.stats()
    check("a plug that says it is locked before any proof is not switched, and is sent no proof",
          isinstance(r, str) and "locked" in r and st["pake_register"] == 1 and st["pake_share"] == 0 and st["on"] is False, (r, st))
    check("its lock is no refusal of the password", app.load()["secrets"]["tapo"].get("refused") is None, app.load()["secrets"]["tapo"])
    logins = strip.stats()["login_ok"]
    r = app.tool({"action": "off", "target": "Socket 3"})
    check("the account's other plug still logs in and switches", r == {"now": "off", "target": "Socket 3"} and
          strip.stats()["login_ok"] == logins + 1, (r, strip.stats()))
    plug.reset({})


def main():
    if not os.access(APP, os.X_OK):
        sys.exit("no app at %s (build the cli preset first)" % APP)
    mocks, app = [], App()
    try:
        # Each plug names its own MAC, in the notation get_device_info uses; the saved MACs are in
        # the ones discovery and older files used.
        plug = Mock("tpap", "p110", "AA-BB-CC-00-00-01")
        mocks.append(plug)
        strip = Mock("klap", "p304m", "AA-BB-CC-00-00-02")
        mocks.append(strip)

        # The stand-in network's id, from the app itself; nothing is saved for it yet.
        out = app.run("--home-refresh")
        network = next((l.split(" id ")[1].strip() for l in out.splitlines() if l.startswith("network: Test network, id ")), "")
        check("the app runs on the stand-in test network", network.startswith("net-"), out)
        check("nothing saved: nothing is contacted", "Nothing here can be controlled yet." in out and
              plug.stats()["requests"] == 0 and strip.stats()["handshake1"] == 0, out)

        # Refresh: names, sockets and states, from a file in the old format.
        app.save(old_file(network, plug.port, strip.port))
        out = app.run("--home-refresh")
        check("refresh reads both devices", "2 devices ready." in out, out)
        check("refresh reads the plug's name (UTF-8 from base64) and its state", "%s" % PLUG in out and
              any(PLUG in l and " off" in l for l in out.splitlines()), out)
        check("refresh reads the strip's four sockets with their states",
              all(("Socket %d" % i) in out for i in range(1, 5)) and any("Socket 2" in l and " on " in l + " " for l in out.splitlines()), out)
        st = plug.stats()
        check("TPAP plug: one login, no bad requests", st["pake_share"] == 1 and st["login_ok"] == 1 and st["bad"] == [], st)
        st = strip.stats()
        check("KLAP strip: one login, no bad requests", st["handshake1"] == 1 and st["login_ok"] == 1 and st["bad"] == [], st)
        saved = app.load()
        devices = saved["networks"][network]["devices"]
        check("the old file is now in the new form", saved.get("version") == 2 and "tapo" not in saved and
              saved["secrets"]["tapo"]["email"] == EMAIL and all("switches" not in d for d in devices), saved.keys())
        things = {t["name"]: t for d in devices for t in d["things"]}
        check("things: the plug and four sockets, each a plug that switches", set(things) == {PLUG, "Socket 1", "Socket 2", "Socket 3", "Socket 4"}
              and all(t["type"] == "plug" and t["caps"] & 1 for t in things.values()), things)
        check("the P110 measures power (a sensor); the strip's sockets do not", things.get(PLUG, {}).get("caps") == 1 | (1 << 13) and
              things.get("Socket 1", {}).get("caps") == 1, things)

        # On and off by name.
        r = app.tool({"action": "on", "target": PLUG})
        check("on by name switches the TPAP plug on", r == {"now": "on", "target": PLUG} and plug.stats()["on"] is True, r)
        r = app.tool({"action": "on", "target": "Socket 1"})
        check("on by name switches the right socket of the KLAP strip", r == {"now": "on", "target": "Socket 1"} and
              strip.stats()["children_on"] == [True, True, False, False], (r, strip.stats()["children_on"]))
        r = app.tool({"action": "off", "device": "socket 2"})
        check("off with the old parameter name and loose spelling", r == {"now": "off", "target": "Socket 2"} and
              strip.stats()["children_on"] == [True, False, False, False], (r, strip.stats()["children_on"]))
        r = app.tool({"action": "on", "target": "Socket"})
        check("an ambiguous name is reported, nothing is switched", isinstance(r, str) and r.startswith('ERROR: "Socket" could mean any of') and
              strip.stats()["children_on"] == [True, False, False, False], r)
        r = app.tool({"action": "on", "target": "Kettle"})
        check("an unknown name is an error with the names there are", isinstance(r, str) and 'no device called "Kettle"' in r and
              '"Socket 3"' in r, r)

        # Status, with the energy readings for one plug asked about.
        r = app.tool({"action": "status", "target": PLUG})
        state = r.get("state", {}) if isinstance(r, dict) else {}
        check("status of the plug gives on, power now and energy today", state.get("on") is True and abs(state.get("power_w", 0) - 23.456) < 1e-6
              and state.get("energy_today_wh") == 321, r)
        r = app.tool({"action": "status"})
        everything = r.get("devices", {}).get("no room", {}) if isinstance(r, dict) else {}
        check("status of everything: name and on/off for each", everything == {PLUG: "on", "Socket 1": "on", "Socket 2": "off",
                                                                                 "Socket 3": "off", "Socket 4": "off"}, r)
        check("status results carry no addresses or ids", "127.0.0.1" not in json.dumps(r) and "8000000" not in json.dumps(r), r)

        # Everything off together; everything on is refused.
        r = app.tool({"action": "on", "target": "all"})
        check("everything on is refused", isinstance(r, str) and r.startswith("ERROR: everything can be switched off together"), r)
        r = app.tool({"action": "off", "target": "all"})
        check("everything off switches all five off", r == {"changed": 5, "now": "off", "target": "all"} and plug.stats()["on"] is False and
              strip.stats()["children_on"] == [False] * 4, (r, plug.stats()["on"], strip.stats()["children_on"]))

        # An address is not an identity. The plug saved as "Desk lamp" has gone, and the router gave
        # its address to another plug of the same account, one that asks before it is switched on.
        # That plug would accept the login: it is asked who it is first, and is sent nothing else.
        heater = Mock("tpap", "p110", "AA-BB-CC-00-00-03", "Heater")
        mocks.append(heater)
        root = app.load()
        devs = root["networks"][network]["devices"]
        desk = json.loads(json.dumps(next(d for d in devs if d["id"] == "tapo:p110")))
        desk.update(id="tapo:desk", name="Desk lamp", mac="aa:bb:cc:00:00:04", port=heater.port)
        desk["things"][0].update(name="Desk lamp", alias="", ask_first=False)
        devs.append(desk)
        app.save(root)
        r = app.tool({"action": "on", "target": "Desk lamp"})
        check("another plug at a saved address is not switched in its place", isinstance(r, str) and
              r.startswith('ERROR: could not change "Desk lamp"') and "another plug answered" in r and heater.stats()["on"] is False, r)
        r = app.tool({"action": "status", "target": "Desk lamp"})
        check("nor is its state reported as the saved plug's", isinstance(r, str) and "another plug answered" in r, r)
        st = heater.stats()
        check("that plug was only asked who it is", st["on"] is False and set(st["methods"]) == {"get_device_info"} and st["bad"] == [], st)
        check("its refusal to be someone else is no refusal of the password", app.load()["secrets"]["tapo"].get("refused") is None,
              app.load()["secrets"]["tapo"])
        # Saved by an earlier version with twelve zeros for its MAC, which name no plug: nothing could tell
        # the plug at its address apart, so none is switched and nothing is sent until a scan saves its MAC.
        root = app.load()
        next(d for d in root["networks"][network]["devices"] if d["id"] == "tapo:desk")["mac"] = "00:00:00:00:00:00"
        app.save(root)
        before = heater.stats()
        r = app.tool({"action": "on", "target": "Desk lamp"})
        check("a plug saved with a zero MAC is not switched, and nothing is sent", isinstance(r, str) and
              "scan the network again" in r and heater.stats() == before, (r, heater.stats()))
        root = app.load()
        root["networks"][network]["devices"] = [d for d in root["networks"][network]["devices"] if d["id"] != "tapo:desk"]
        app.save(root)
        r = app.tool({"action": "on", "target": PLUG})
        check("the plugs that are where they were saved still switch", r == {"now": "on", "target": PLUG} and plug.stats()["on"] is True, r)

        nested_too_deep(app, plug, network)
        locked_by_another(app, plug, strip)

        # A wrong password: tried once, by each plug's rules, and never again, even by a new run.
        before = (plug.stats()["pake_share"], strip.stats()["handshake1"])
        root = app.load()
        root["secrets"]["tapo"]["password_sha1"] = WRONG
        app.save(root)
        r = app.tool({"action": "on", "target": PLUG})
        check("a wrong password fails", isinstance(r, str) and r.startswith('ERROR: could not change "%s"' % PLUG), r)
        after = plug.stats()
        check("the plug saw exactly one login attempt", after["pake_share"] == before[0] + 1 and after["login_rejected"] == 1, after)
        check("the refusal is kept in the file", app.load()["secrets"]["tapo"].get("refused") is True, app.load()["secrets"]["tapo"])
        for args in ({"action": "on", "target": PLUG}, {"action": "on", "target": "Socket 3"}, {"action": "status"}):
            r = app.tool(args)
            check("a new run does not try the refused password (%s)" % args.get("target", "status"),
                  "did not accept the saved password" in (r if isinstance(r, str) else json.dumps(r)), r)
        out = app.run("--home-refresh")
        check("refresh does not try it either, and says the password is needed", "did not accept the saved password" in out, out)
        check("after the refusal neither plug was contacted for a login again",
              plug.stats()["pake_share"] == before[0] + 1 and plug.stats()["pake_register"] == after["pake_register"] and
              strip.stats()["handshake1"] == before[1], (plug.stats(), strip.stats()))

        # The right password typed again (as the settings page saves it) works at once.
        root = app.load()
        root["secrets"]["tapo"] = {"email": EMAIL, "password_sha1": SHA1}
        app.save(root)
        out = app.run("--home-refresh")
        check("with the password corrected both are ready again", "2 devices ready." in out, out)
        r = app.tool({"action": "on", "target": "Socket 4"})
        check("and switch", r == {"now": "on", "target": "Socket 4"} and strip.stats()["children_on"] == [False, False, False, True], r)

        # The same through the KLAP strip first.
        root = app.load()
        root["secrets"]["tapo"]["password_sha1"] = WRONG
        app.save(root)
        before, proofs = strip.stats()["handshake1"], strip.stats()["handshake2"]
        r = app.tool({"action": "off", "target": "Socket 4"})
        check("KLAP: a wrong password fails", isinstance(r, str) and r.startswith('ERROR: could not change "Socket 4"'), r)
        r = app.tool({"action": "off", "target": "Socket 4"})
        app.run("--home-refresh")
        check("KLAP: the strip saw one login attempt, and no proof of the wrong password",
              strip.stats()["handshake1"] == before + 1 and strip.stats()["handshake2"] == proofs, strip.stats())
        check("KLAP: the refusal is kept", app.load()["secrets"]["tapo"].get("refused") is True, app.load()["secrets"]["tapo"])

        leaked = [s for s in (PASSWORD, SHA1, WRONG) if any(s in o for o in outputs)]
        check("no password or password hash appears in anything the app printed", not leaked, leaked)
    finally:
        for m in mocks:
            m.stop()
        shutil.rmtree(app.state, ignore_errors=True)
    print("%d checks, %d failed" % (checks, failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
