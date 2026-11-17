#!/usr/bin/env python3
"""The spoken question before a risky smart-home change, through the real request loop.

The command-line build runs in --chat mode against a scripted chat service (tests/mocks/mock_chat.py)
and a pretend Tapo P110 (tests/mocks/mock_tapo.py), both on 127.0.0.1 with ports the system picks,
on the stand-in test network (ZB_TEST_NETWORK=1) with its state in a fresh temporary folder. The
plug is called "Fan heater" and is set to ask before it is switched on.

The scripted model always repeats the change when the user answers, as a confused or manipulated
model might; what decides is the app's own check that the question was put (a reply that asks,
naming the plug) and that the very next message is a plain yes. The plug's own state shows
whether it was switched.

usage: tests/home_chat_e2e.py [path/to/zero_bot]          (default: build/cli/zero_bot)
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(HERE, "..", "build", "cli", "zero_bot")
EMAIL, PASSWORD = "user@example.com", "hunter2"
SHA1 = hashlib.sha1(PASSWORD.encode()).hexdigest()
OWNER = hashlib.md5(EMAIL.encode()).hexdigest().upper()
ON = {"action": "on", "target": "Fan heater", "say": "The fan heater is on."}
ASK = {"text": "Shall I turn on the fan heater?"}

checks = failed = 0


def check(name, ok, detail=""):
    global checks, failed
    checks += 1
    failed += not ok
    print("%s  %s%s" % ("ok  " if ok else "FAIL", name, "" if ok else "  -> %s" % (detail,)), flush=True)


def start(cmd, tag):
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    line = p.stdout.readline().split()
    if line[:2] != [tag, "listening"]:
        p.kill()
        raise RuntimeError("%s did not start: %r" % (cmd[2], line))
    return p, int(line[2])


def stop(p):
    p.kill()
    p.wait()


def scenario(name, answers, after_confirm, want_on, chat_says=None):
    """The user asks for the fan heater, the model is told CONFIRM and replies `after_confirm`
    (one reply, or a list used in turn), then the user says each of `answers`."""
    state = tempfile.mkdtemp(prefix="zb-chat-e2e-")
    procs = []
    try:
        plug, plug_port = start([sys.executable, "-I", os.path.join(HERE, "mocks", "mock_tapo.py"), "--port", "0", "--mode", "tpap",
                                 "--model", "p110", "--email", EMAIL, "--password", PASSWORD, "--stdin-lifeline"], "MOCK")
        procs.append(plug)
        env = dict(os.environ, XDG_CONFIG_HOME=state + "/config", XDG_DATA_HOME=state + "/data", XDG_CACHE_HOME=state + "/cache",
                   ZB_TEST_NETWORK="1", ZB_NO_AUDIO="1", ZB_API_KEY="test-key")
        for v in ("ZB_DRY_RUN", "http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY"):
            env.pop(v, None)
        os.makedirs(state + "/config/zero-bot")
        os.makedirs(state + "/data/zero-bot")
        out = subprocess.run([APP, "--home-refresh"], env=env, capture_output=True, text=True, timeout=60).stdout
        network = next((l.split(" id ")[1].strip() for l in out.splitlines() if l.startswith("network: Test network, id ")), "")
        tpap = json.dumps({"tls": 0, "dac": 0, "noc": 0, "pake": [2], "port": 80})
        home = {"version": 2, "secrets": {"tapo": {"email": EMAIL, "password_sha1": SHA1}},
                "networks": {network: {"label": "Test network", "gateway_mac": "02:00:00:00:00:00", "allowed_at": 1, "last_scan": 1,
                                       "devices": [{"id": "tapo:p110", "kind": "tapo", "name": "Tapo P110", "model": "P110(UK)",
                                                    "last_ip": "127.0.0.1", "port": plug_port, "enabled": True, "things": [],
                                                    "info": {"scheme": "TPAP", "tpap": tpap, "device_type": "SMART.TAPOPLUG",
                                                             "owner": OWNER}}]}}}
        path = state + "/data/zero-bot/smarthome.json"
        json.dump(home, open(path, "w"))
        subprocess.run([APP, "--home-refresh"], env=env, capture_output=True, text=True, timeout=60)
        home = json.load(open(path))
        thing = home["networks"][network]["devices"][0]["things"][0]
        thing["alias"], thing["ask_first"] = "Fan heater", True
        json.dump(home, open(path, "w"))
        json.dump({"llm_model": "scripted", "fast_mode": False, "speak_replies": False}, open(state + "/config/zero-bot/settings.json", "w"))

        script = {"user": [["fan heater", {"tool": "smart_home", "args": ON}]] + [[a, {"tool": "smart_home", "args": ON}] for a in answers],
                  "after": [["CONFIRM", after_confirm], ["{", {"text": "Done."}], ["ERROR", {"text": "That did not work."}]],
                  "default": {"text": "OK."}}
        json.dump(script, open(state + "/script.json", "w"))
        log = state + "/requests.jsonl"
        chat, chat_port = start([sys.executable, "-I", os.path.join(HERE, "mocks", "mock_chat.py"), state + "/script.json", log], "CHAT")
        procs.append(chat)
        env["ZB_API_BASE"] = "http://127.0.0.1:%d/v1" % chat_port
        p = subprocess.run([APP, "--chat"], input="\n".join(["turn on the fan heater"] + answers) + "\n", env=env, capture_output=True,
                           text=True, timeout=120)
        with urllib.request.urlopen("http://127.0.0.1:%d/mock/stats" % plug_port, timeout=5) as r:
            is_on = json.load(r)["on"]
        results = [l.strip()[3:] for l in p.stdout.splitlines() if l.strip().startswith("<- ")]
        check("%s: the first call is put as a question" % name, bool(results) and results[0].startswith("CONFIRM"), results[:1])
        check("%s: the plug is %s" % (name, "switched on" if want_on else "left off"), is_on is want_on, (is_on, results))
        if chat_says:
            answers_out = [l for l in p.stdout.splitlines() if l.startswith("[answer")]
            check("%s: %s" % (name, chat_says[0]), chat_says[1](answers_out, results), (answers_out, results))
        # Nothing secret ever goes to the chat service.
        sent = open(log).read() if os.path.exists(log) else ""
        check("%s: no password, hash or plug address in what the chat service saw" % name,
              all(s not in sent for s in (PASSWORD, SHA1, "127.0.0.1:%d" % plug_port)), "")
    finally:
        for pr in procs:
            stop(pr)
        shutil.rmtree(state, ignore_errors=True)


def main():
    if not os.access(APP, os.X_OK):
        sys.exit("no app at %s (build the cli preset first)" % APP)
    # Asked, and answered yes: switched, and the say sentence is spoken without another model call.
    scenario("asked, then yes", ["yes"], ASK, True,
             ("the confirmation sentence is spoken", lambda a, r: len(a) == 2 and "The fan heater is on." in a[1]))
    # The question was never put: a later yes is a yes to something else.
    scenario("not asked ('Done. Anything else?'), then yes", ["yes"], {"text": "Done. Anything else?"}, False)
    scenario("an empty reply, then yes", ["yes"], {"text": ""}, False)
    # Asked, but the answer is not a plain yes.
    scenario("asked, then an unrelated request", ["please set a timer for ten minutes"], ASK, False)
    scenario("asked, then 'leave it off please'", ["leave it off please"], ASK, False)
    # Asked, "no", asked again (the model is told the answer was not a yes), then yes.
    scenario("asked, no, asked again, yes", ["no", "yes"], [ASK, ASK], True,
             ("the second CONFIRM says the answer was not a clear yes",
              lambda a, r: len(r) == 3 and "was not a clear yes" in r[1] and r[2].startswith("{")))
    print("%d checks, %d failed" % (checks, failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
