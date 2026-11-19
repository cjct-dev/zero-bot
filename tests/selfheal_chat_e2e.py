#!/usr/bin/env python3
"""What the request loop does when a tool call or the service fails, through the real loop.

The command-line build runs in --chat mode against a scripted chat service (tests/mocks/mock_chat.py)
that plays a model making the mistakes models make: a call cut short, a number written as a word, a
misspelt tool, the same failed call sent again, "done" after a failure, an empty reply; and a
service that fails on the way (a 503, an overloaded stream, a connection lost after a tool ran).
One case has a pretend Tapo plug (tests/mocks/mock_tapo.py) that refuses to switch, on the stand-in
test network (ZB_TEST_NETWORK=1), so that what reached the device is counted on the device's side.
Everything is on 127.0.0.1 with ports the system picks, and each case has a fresh state folder.

Checked: the model is told precisely what to change and its corrected call works; the service and
reads are tried again only when that is safe; the same failed change is never sent to a device
twice in one request (only once the user has asked again); and every failure ends in something
the user is told.

usage: tests/selfheal_chat_e2e.py [path/to/zero_bot]          (default: build/cli/zero_bot)
"""
import hashlib
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(HERE, "..", "build", "cli", "zero_bot")
EMAIL, PASSWORD = "user@example.com", "hunter2"
LAMP_ON = {"action": "on", "target": "Desk lamp", "say": "The desk lamp is on."}
LOST = "I lost the connection to the AI service before I could finish, so please ask again."

checks = failed = 0


def check(name, ok, detail=""):
    global checks, failed
    checks += 1
    failed += not ok
    print("%s  %s%s" % ("ok  " if ok else "FAIL", name, "" if ok else "  -> %r" % (detail,)), flush=True)


def start(cmd, tag):
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    line = p.stdout.readline().split()
    if line[:2] != [tag, "listening"]:
        p.kill()
        raise RuntimeError("%s did not start: %r" % (cmd[2], line))
    return p, int(line[2])


def mock_call(port, path, body=None):
    data = None if body is None else json.dumps(body).encode()
    with urllib.request.urlopen(urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data), timeout=5) as r:
        return json.load(r)


class Run:
    """What one --chat conversation produced."""
    def __init__(self, out, requests, files, device):
        lines = out.splitlines()
        self.calls = [l.strip()[3:] for l in lines if l.strip().startswith("-> ")]
        self.results = [l.strip()[3:] for l in lines if l.strip().startswith("<- ")]
        self.answers = [l.split("] ", 1)[1] if "] " in l else "" for l in lines if l.startswith("[answer")]
        self.errors = [l[8:] for l in lines if l.startswith("[error] ")]
        self.withdrawn = sum(l.startswith("[draft withdrawn]") for l in lines)
        self.requests = requests
        self.files = files
        self.device = device
        self.out = out


def closed_port():
    """A port on 127.0.0.1 that nothing listens on: bound, then closed again at once."""
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def converse(script, said, plug=False, refuse=False, ask_first=False, stations=None):
    state = tempfile.mkdtemp(prefix="zb-selfheal-e2e-")
    procs = []
    try:
        env = dict(os.environ, XDG_CONFIG_HOME=state + "/config", XDG_DATA_HOME=state + "/data", XDG_CACHE_HOME=state + "/cache",
                   HOME=state + "/home", ZB_TEST_NETWORK="1", ZB_NO_AUDIO="1", ZB_API_KEY="test-key", TZ="UTC")
        for v in ("ZB_DRY_RUN", "ZB_FAKE_CAMERA", "ZB_SEARCH_KEY", "http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY",
                  "all_proxy", "ALL_PROXY"):
            env.pop(v, None)
        for d in ("config/zero-bot", "data/zero-bot", "home"):
            os.makedirs(os.path.join(state, d))
        plug_port = None
        if plug:
            p, plug_port = start([sys.executable, "-I", os.path.join(HERE, "mocks", "mock_tapo.py"), "--port", "0", "--mode", "tpap",
                                  "--model", "p110", "--email", EMAIL, "--password", PASSWORD, "--stdin-lifeline"], "MOCK")
            procs.append(p)
            out = subprocess.run([APP, "--home-refresh"], env=env, capture_output=True, text=True, timeout=60).stdout
            network = next((l.split(" id ")[1].strip() for l in out.splitlines() if l.startswith("network: Test network, id ")), "")
            owner = hashlib.md5(EMAIL.encode()).hexdigest().upper()
            info = {"scheme": "TPAP", "device_type": "SMART.TAPOPLUG", "owner": owner,
                    "tpap": json.dumps({"tls": 0, "dac": 0, "noc": 0, "pake": [2], "port": 80})}
            home = {"version": 2, "secrets": {"tapo": {"email": EMAIL, "password_sha1": hashlib.sha1(PASSWORD.encode()).hexdigest()}},
                    "networks": {network: {"label": "Test network", "gateway_mac": "02:00:00:00:00:00", "allowed_at": 1, "last_scan": 1,
                                           "devices": [{"id": "tapo:p110-0", "kind": "tapo", "name": "Tapo P110", "model": "P110(UK)",
                                                        "last_ip": "127.0.0.1", "port": plug_port, "enabled": True, "things": [],
                                                        "info": info}]}}}
            path = state + "/data/zero-bot/smarthome.json"
            json.dump(home, open(path, "w"))
            subprocess.run([APP, "--home-refresh"], env=env, capture_output=True, text=True, timeout=60)
            home = json.load(open(path))
            for d in home["networks"][network]["devices"]:
                for t in d["things"]:
                    t["alias"], t["ask_first"] = "Desk lamp", ask_first
            json.dump(home, open(path, "w"))
            mock_call(plug_port, "/mock/reset", {"refuse_set": refuse})   # also forgets the setup's session and counts
        json.dump({"llm_model": "scripted", "fast_mode": False, "speak_replies": False}, open(state + "/config/zero-bot/settings.json", "w"))
        if stations:
            json.dump({"stations": stations}, open(state + "/config/zero-bot/radio.json", "w"))
        json.dump(script, open(state + "/script.json", "w"))
        log = state + "/requests.jsonl"
        chat, chat_port = start([sys.executable, "-I", os.path.join(HERE, "mocks", "mock_chat.py"), state + "/script.json", log], "CHAT")
        procs.append(chat)
        env["ZB_API_BASE"] = "http://127.0.0.1:%d/v1" % chat_port
        p = subprocess.run([APP, "--chat"], input="\n".join(said) + "\n", env=env, capture_output=True, text=True, timeout=180)
        requests = [json.loads(l) for l in open(log)] if os.path.exists(log) else []
        files = {}
        for name in ("calendar", "reminders"):
            f = state + "/data/zero-bot/%s.json" % name
            files[name] = json.load(open(f)) if os.path.exists(f) else []
        device = mock_call(plug_port, "/mock/stats") if plug else None
        return Run(p.stdout, requests, files, device)
    finally:
        for pr in procs:
            pr.kill()
            pr.wait()
        shutil.rmtree(state, ignore_errors=True)


def main():
    if not os.access(APP, os.X_OK):
        sys.exit("no app at %s (build the cli preset first)" % APP)

    # ---- the service itself: a failure on the way is tried again, a refusal is not
    r = converse({"user": [["two plus two", [{"status": 503}, {"tool": "calculator", "args": {"expression": "2+2"}}]]],
                  "after": [["4", {"text": "Two plus two is 4."}]]}, ["what is two plus two"])
    check("a 503 from the service is sent again, and the request carries on", r.answers == ["Two plus two is 4."] and not r.errors,
          (r.answers, r.errors))
    check("... one resend, then the tool's round", len(r.requests) == 3, len(r.requests))
    r = converse({"user": [["hello", [{"sse_error": "Overloaded, please try again"}, {"text": "Hello there."}]]]}, ["hello"])
    check("an overloaded service said so inside the stream: sent again", r.answers == ["Hello there."] and len(r.requests) == 2,
          (r.answers, len(r.requests)))
    r = converse({"user": [["hello", {"status": 401}]]}, ["hello"])
    check("a refusal (401) is not sent again, and is shown", len(r.requests) == 1 and r.errors, (len(r.requests), r.errors))

    # ---- a malformed call, a wrong type and a misspelt tool: told precisely, corrected, done
    r = converse({"user": [["17 times 23", {"tool": "calculator", "raw": "{\"expression\": \"17*23\""}]],
                  "after": [["ERROR: the arguments were not valid JSON", {"tool": "calculator", "args": {"expression": "17*23"}}],
                            ["391", {"text": "17 times 23 is 391."}]]}, ["what is 17 times 23"])
    check("a call cut short is told so, with an example", r.results[:1] and r.results[0].startswith(
        "ERROR: the arguments were not valid JSON (they stop before the object is closed)") and '{"expression": "..."}' in r.results[0],
          r.results)
    check("... and the corrected call works", r.results[1:2] == ["391"] and r.answers == ["17 times 23 is 391."], (r.results, r.answers))

    say = "Your timer is set for 5 minutes."
    r = converse({"user": [["timer for five", {"tool": "timer", "args": {"action": "start", "minutes": "five", "say": say}}]],
                  "after": [["ERROR: minutes must be a number", {"tool": "timer", "args": {"action": "start", "minutes": 5, "say": say}}]]},
                 ["set a timer for five minutes"])
    check("a number written as a word is named, with what was sent", r.results[:1] and
          "minutes must be a number, such as 5; 'five' is not one" in r.results[0], r.results)
    timers = [x for x in r.files["reminders"] if x.get("kind") == "timer"]
    check("... the corrected call starts one timer, and its own sentence is spoken", len(timers) == 1 and r.answers == [say],
          (timers, r.answers))

    say = "Lunch with Sam is in your calendar."
    event = {"title": "Lunch with Sam", "start": "2030-06-01 13:00", "say": say}
    r = converse({"user": [["lunch with sam", {"tool": "calender_add", "args": event}]],
                  "after": [["ERROR: there is no tool called", {"tool": "calendar_add", "args": event}]]},
                 ["put lunch with sam in my calendar on the first of june 2030 at 1pm"])
    check("a misspelt tool name gets the one meant", r.results[:1] and "the one meant is probably calendar_add" in r.results[0], r.results)
    check("... and the event is saved once", len(r.files["calendar"]) == 1 and r.answers == [say], (r.files["calendar"], r.answers))

    # ---- a change that failed at the device is not sent again by the model in the same request
    script = {"user": [["turn on the desk lamp", {"tool": "smart_home", "args": LAMP_ON}],
                       ["try again", {"tool": "smart_home", "args": LAMP_ON}]],
              "after": [["ERROR: could not change", {"tool": "smart_home", "args": LAMP_ON}],     # a stubborn model asks again
                        ["ERROR: this exact call already failed",
                         [{"text": "Sorry, the desk lamp did not respond."}, {"text": "I've turned on the desk lamp."}]]],
              "note": [["nothing has been done", {"text": "Sorry, the desk lamp is still not responding, so it is off."}]]}
    r = converse(script, ["turn on the desk lamp", "try again"], plug=True, refuse=True)
    sets = r.device["methods"].count("set_device_info") if r.device else -1
    check("the plug said no, and the model was told", len(r.results) >= 1 and r.results[0].startswith(
        'ERROR: could not change "Desk lamp": the plug answered with error code -1001'), r.results)
    check("the same call sent again by the model is not made again", len(r.results) >= 2 and
          r.results[1].startswith("ERROR: this exact call already failed in this request, so it was not made again"), r.results)
    check("the plug was asked once per request the user made: twice in all", sets == 2, (sets, r.device and r.device["methods"]))
    check("the say sentence of a failed call is never spoken", all("The desk lamp is on." not in a for a in r.answers), r.answers)
    check("'I've turned on the desk lamp' after the failure is withdrawn and the failure told instead",
          r.answers == ["Sorry, the desk lamp did not respond.", "Sorry, the desk lamp is still not responding, so it is off."]
          and r.withdrawn == 1, (r.answers, r.withdrawn))

    # ---- an offer put as a question is not "done": after a failure, or for a change that waits for a yes
    offer = "Shall I put it in for the 3rd of March 2031 instead, since that date has gone by?"
    r = converse({"user": [["lunch with sam", {"tool": "calendar_add", "args": {"title": "Lunch with Sam", "start": "2020-03-03 13:00",
                                                                                 "say": "Lunch is in."}}]],
                  "after": [["ERROR: that start is in the past", {"text": offer}]],
                  "note": [["nothing has been done", {"text": "Sorry, that date is in the past."}]]},
                 ["put lunch with sam in my calendar on 3 march 2020 at 1pm"])
    check("'Shall I put it in for next year?' after a failed call is spoken as it is, not withdrawn",
          r.answers == [offer] and r.withdrawn == 0 and len(r.requests) == 2, (r.answers, r.withdrawn, len(r.requests)))
    question = "Shall I put the desk lamp on?"
    r = converse({"user": [["turn on the desk lamp", {"tool": "smart_home", "args": LAMP_ON}], ["yes", {"tool": "smart_home", "args": LAMP_ON}]],
                  "after": [["CONFIRM", {"text": question}]],
                  "note": [["nothing has been done", {"text": "The desk lamp asks first. Switch it on?"}]]},
                 ["turn on the desk lamp", "yes"], plug=True, ask_first=True)
    sets = r.device["methods"].count("set_device_info") if r.device else -1
    check("a change waiting for a yes: '%s' is put to the user at once" % question,
          r.answers[:1] == [question] and r.withdrawn == 0, (r.answers, r.withdrawn))
    check("... and the yes switches the lamp, once", sets == 1 and r.answers[1:] == ["The desk lamp is on."], (sets, r.answers))
    r = converse({"user": [["remind me", {"tool": "reminder_set", "args": {"text": "tea"}}]],
                  "after": [["ERROR", {"text": "Done, shall I do anything else?"}]],
                  "note": [["nothing has been done", {"text": "I could not set it: it needs a time."}]]}, ["remind me about tea"])
    check("'Done, shall I do anything else?' after a failed call is still withdrawn",
          r.answers == ["I could not set it: it needs a time."] and r.withdrawn == 1, (r.answers, r.withdrawn))

    # ---- radio: a station that did not play is not announced as playing (ZB-055). The station's
    # address is a port nothing listens on, so the check before playing fails and nothing is played.
    stations = [{"name": "Closed FM", "url": "http://127.0.0.1:%d/live.mp3" % closed_port()}]
    play = {"tool": "radio", "args": {"action": "play", "station": "Closed FM", "say": "Here is Closed FM."}}
    told = "Sorry, Closed FM did not play: its server could not be reached."
    for claim in ("Here is Closed FM.", "Here’s Closed FM for you.", "Playing Closed FM now.", "Closed FM is playing now."):
        r = converse({"user": [["closed fm", play]], "after": [["ERROR", {"text": claim}]],
                      "note": [["nothing has been done", {"text": told}]]}, ["play closed fm"], stations=stations)
        check("radio: the call failed, so %r is withdrawn and the failure told" % claim,
              r.results[:1] and r.results[0].startswith("ERROR:") and r.answers == [told] and r.withdrawn == 1,
              (r.results, r.answers, r.withdrawn))
    for honest in ("Closed FM is not playing: its server could not be reached.", "Here's the problem: Closed FM did not answer.",
                   "I couldn't start playing Closed FM, its server is not answering.", "Closed FM isn't playing right now, sorry."):
        r = converse({"user": [["closed fm", play]], "after": [["ERROR", {"text": honest}]],
                      "note": [["nothing has been done", {"text": "withdrawn by mistake"}]]}, ["play closed fm"], stations=stations)
        check("radio: after the failure %r is spoken as it is" % honest, r.answers == [honest] and r.withdrawn == 0,
              (r.answers, r.withdrawn))
    # ... nor a sentence that says no or asks (review of ZB-055): "skip this song" with no music on fails
    # with "nothing is playing", and saying so is the honest answer, as it was before the radio's claims.
    for honest in ("Here's an idea: shall I play another station?", "Here is the list of stations you have: Closed FM."):
        r = converse({"user": [["closed fm", play]], "after": [["ERROR", {"text": honest}]],
                      "note": [["nothing has been done", {"text": "withdrawn by mistake"}]]}, ["play closed fm"], stations=stations)
        check("radio: after the failure %r is spoken as it is" % honest, r.answers == [honest] and r.withdrawn == 0,
              (r.answers, r.withdrawn))
    skip = {"tool": "music", "args": {"action": "next"}}
    for honest in ("Nothing is playing right now, so there is nothing to skip.", "No music is playing at the moment.",
                   "There's nothing to skip: no song is playing."):
        r = converse({"user": [["skip", skip]], "after": [["ERROR", {"text": honest}]],
                      "note": [["nothing has been done", {"text": "withdrawn by mistake"}]]}, ["skip this song"])
        check("music: nothing to skip, and %r is spoken as it is" % honest,
              r.results == ["ERROR: nothing is playing"] and r.answers == [honest] and r.withdrawn == 0, (r.results, r.answers, r.withdrawn))

    # ---- never silence: an empty reply goes back once; then an answer is made here
    r = converse({"user": [["capital of portugal", {"text": ""}]], "note": [["Your reply was empty", {"text": "Lisbon."}]]},
                 ["what is the capital of portugal"])
    check("an empty reply is sent back once, and the second answer is used", r.answers == ["Lisbon."] and len(r.requests) == 2,
          (r.answers, len(r.requests)))
    r = converse({"user": [["say nothing", {"text": ""}]], "note": [["Your reply was empty", {"text": ""}]]}, ["say nothing"])
    check("empty twice: the user is told so rather than left with silence", r.answers == ["Sorry, I have no answer to that."], r.answers)
    r = converse({"user": [["remind me", {"tool": "reminder_set", "args": {"text": "tea"}}]],
                  "after": [["ERROR", {"text": ""}]],
                  "note": [["Your reply was empty", {"text": ""}]]}, ["remind me about tea"])
    check("... and after a failed call it says something did not work", r.answers == ["Sorry, something I tried did not work."], r.answers)

    # ---- the service lost after tools ran: the user hears what is known
    r = converse({"user": [["two plus two", {"tool": "calculator", "args": {"expression": "2+2"}}]], "after": [["4", {"status": 500}]]},
                 ["what is two plus two"])
    check("the service lost after a tool: said so, not silence", r.answers == [LOST] and r.errors, (r.answers, r.errors))
    check("... after trying it three times in all", len(r.requests) == 4, len(r.requests))
    timer = {"tool": "timer", "args": {"action": "start", "minutes": 5, "say": "Your timer is set for 5 minutes."}}
    r = converse({"user": [["a reminder for tea", {"tools": [timer, {"tool": "reminder_set", "args": {"text": "tea"}}]}]],
                  "after": [["ERROR", {"status": 500}]]}, ["set a timer for 5 minutes and a reminder for tea"])
    check("one call worked, one failed, then the service was lost: the user hears both",
          r.answers == ["Your timer is set for 5 minutes. Something I tried did not work, and I lost the connection to the AI service "
                        "before I could say what. Please ask again."], r.answers)

    print("%d checks, %d failed" % (checks, failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
