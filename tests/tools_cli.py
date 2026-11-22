#!/usr/bin/env python3
"""The assistant's tools that need no network, each run on its own through the command-line build.

zero_bot --tool NAME 'JSON' is called with good, wrong, missing and hostile arguments, in a fresh
temporary state folder (HOME and the XDG folders), with ZB_DRY_RUN (no music, infrared or device
is used), ZB_NO_AUDIO, ZB_TEST_NETWORK and no AI service configured. Each result must be the right
one or an ERROR sentence that says what to change, the program must not crash, and nothing may be
written outside the state folder. Tools that need the internet (weather, currency, stocks, web
search, a place looked up by name, the photo's description) are run only against the local stand-in
(tests/mocks/fake_web.py), to check the experiments' guards: ZB_EXP_NO_HARDWARE keeps the camera and the
infrared transmitter out of a test run, a bad ZB_EXP_WEB stops the run cleanly, and a request to the
stand-in takes no proxy and follows no redirect. no_real_web runs one case through the live sets' own runner
(tests/agent_eval.py run_case) against a scripted model (tests/mocks/fake_model.py) that calls every tool
that reads the web, with every proxy variable pointing at a trap: no request leaves 127.x (the trap sees
none), each service is answered by the stand-in, the search carries the dummy key and never the inherited
one, and a case whose environment holds a real key is refused before the app starts.

usage: tests/tools_cli.py [path/to/zero_bot]          (default: build/cli/zero_bot)
"""
import json
import re
import os
import shutil
import stat
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True
import netguard   # noqa: E402  the network boundary (tests/netguard.py)
APP = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(HERE, "..", "build", "cli", "zero_bot")

checks = failed = 0


def check(name, ok, detail=""):
    global checks, failed
    checks += 1
    failed += not ok
    if not ok:
        print("FAIL  %s  -> %r" % (name, detail), flush=True)


class State:
    def __init__(self, root, name):
        self.dir = os.path.join(root, name)
        for sub in ("home", "config", "data", "cache"):
            os.makedirs(os.path.join(self.dir, sub), exist_ok=True)

    def env(self, **extra):
        env = {k: v for k, v in os.environ.items() if not k.startswith("ZB_") and k not in ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME")}
        env.update(HOME=self.dir + "/home", XDG_CONFIG_HOME=self.dir + "/config", XDG_DATA_HOME=self.dir + "/data",
                   XDG_CACHE_HOME=self.dir + "/cache", ZB_DRY_RUN="1", ZB_NO_AUDIO="1", ZB_TEST_NETWORK="1", TZ="UTC")
        env = netguard.child_env(env)   # no key, every proxy at the trap, only 127.0.0.1 and localhost exempt
        for k, v in extra.items():   # None leaves a variable out
            if v is None:
                env.pop(k, None)
            else:
                env[k] = v
        return env

    def run(self, name, args, **extra):
        text = args if isinstance(args, str) else json.dumps(args, ensure_ascii=False)
        p = subprocess.run([APP, "--tool", name, text], env=self.env(**extra), capture_output=True, timeout=60)
        out = p.stdout.decode("utf-8", "replace").strip()
        check("%s %s exits normally" % (name, text[:60]), p.returncode == 0, (p.returncode, out[:200]))
        return out

    def file(self, *parts):
        path = os.path.join(self.dir, *parts)
        return json.load(open(path)) if os.path.exists(path) else None


def expect(state, name, args, *wanted, **extra):
    """The result contains every string in `wanted`; an expected error must read as an instruction."""
    out = state.run(name, args, **extra)
    label = "%s %s" % (name, (args if isinstance(args, str) else json.dumps(args, ensure_ascii=False))[:70])
    for w in wanted:
        check(label + " -> " + w, w in out, out[:400])
    if out.startswith("ERROR"):
        check(label + " gives a sentence, not a bare word", len(out.split()) >= 4, out)
    return out


def error(state, name, args, *wanted, **extra):
    out = expect(state, name, args, *wanted, **extra)
    check("%s %s is an error" % (name, json.dumps(args)[:60]), out.startswith("ERROR"), out[:300])
    return out


def ok(state, name, args, *wanted, **extra):
    out = expect(state, name, args, *wanted, **extra)
    check("%s %s works" % (name, json.dumps(args)[:60]), not out.startswith("ERROR"), out[:300])
    return out


def main():
    netguard.require_experiments(APP)   # a build without the experiments sends a search to the real service
    root = tempfile.mkdtemp(prefix="zb-tools-cli-")
    try:
        run_all(root)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print("tools through the command line: %d checks, %d failed" % (checks, failed))
    sys.exit(1 if failed else 0)


def run_all(root):
    s = State(root, "general")

    # ---- the tool runner itself
    error(s, "no_such_tool", {}, "there is no tool called")
    error(s, "calculator", '{"expression":', "not valid JSON")
    error(s, "calculator", "[1,2]", "must be one JSON object", "not a list")

    # ---- what the runner checks before any tool runs (tools::run): the error names the argument,
    # what it must be and what was sent, and an example where the form was wrong
    error(s, "calculator", '{"expression": "2+2", "x": [1,', "stop before the object is closed", 'e.g. {"expression": "..."}')
    error(s, "calculator", '{"expression" "2+2"}', "break off at character", "Send the call again")
    error(s, "calender_add", {}, "the one meant is probably calendar_add")
    error(s, "frobnicate", {}, "the tools are calculator, ", "device")
    error(s, "calculator", {"expression": ["2+2"]}, "expression must be text, not a list")
    error(s, "weather", {"days": "five"}, "days must be a number, such as 5; 'five' is not one")   # before any network
    error(s, "weather", {"days": True}, "days must be a number, such as 5, not true")
    error(s, "breathe", {"minutes": "a few"}, "minutes must be a number")
    ok(s, "breathe", {"minutes": ""}, '"minutes":5')                       # empty text for a number is "not given"
    error(s, "reminder_set", {"text": "tea", "minutes": 5}, "give either in_minutes or at",
          "no argument called 'minutes'", "its arguments are at, in_minutes, text")
    error(s, "timer", {"action": "start", "minutes": "five"}, "minutes must be a number")
    error(s, "calendar_delete", {"id": [1]}, "id must be a whole number", "not a list")
    error(s, "device", {"setting": "spoken_replies", "on": "perhaps"}, "on must be true or false, not 'perhaps'")
    ok(s, "device", {"setting": "spoken_replies", "on": "no"}, "spoken replies off")   # yes/no taken as meant
    ok(s, "device", {"setting": "spoken_replies", "on": 1}, "spoken replies on")
    b = State(root, "booleans")
    out = ok(b, "calendar_add", {"title": "Day off", "start": "2030-05-01", "all_day": "yes"}, '"all_day":true')
    error(b, "calendar_add", {"title": "Day off", "start": "2030-05-02", "all_day": "maybe"}, "all_day must be true or false")
    ok(b, "timer", {"action": "START", "minutes": "5"}, '"started"')     # an enum value in other capitals

    # ---- one shape for every failure: a JSON object with "error" reads as an ERROR sentence
    lib = State(root, "music")
    os.makedirs(lib.dir + "/home/Music")
    open(lib.dir + "/home/Music/Waterloo.mp3", "wb").close()
    error(lib, "music", {"action": "play", "artist": "pink floyd"}, "ERROR: nothing in the library matches, so nothing was started.",
          "not in their library")
    ok(lib, "music", {"action": "play", "title": "waterloo"}, '"now_playing":"Waterloo"')

    # ---- calculator
    ok(s, "calculator", {"expression": "(84.50 + 12) * 1.125 / 4"}, "27.140625")
    ok(s, "calculator", {"expression": 42}, "42")                       # a number where text was expected
    error(s, "calculator", {}, "expression is required")
    error(s, "calculator", {"expression": "200 + 10%"}, "200*1.1")
    error(s, "calculator", {"expression": "15% of 80"}, "0.15*80")
    error(s, "calculator", {"expression": "(" * 30000 + "1" + ")" * 30000}, "nested too deeply")
    error(s, "calculator", {"expression": "\u0007\u001b[2J"}, "control character")

    # ---- dates
    ok(s, "date_info", {"date": "2028-02-29", "add_days": -365}, "2027-03-01")
    error(s, "date_info", {"date": "2027-02-29"}, "'2027-02-29' is not one")
    error(s, "date_info", {"date": 20271225}, "must be a real date")      # not read as "no date", which meant today
    error(s, "date_info", {"add_days": 1e300}, "within a million days")   # crashed the app
    error(s, "date_info", {"add_days": "nan"}, "add_days must be a number")
    error(s, "date_info", {"add_days": "soon"}, "add_days must be a number")

    # ---- calendar
    c = State(root, "calendar")
    ok(c, "calendar_add", {"title": "Dentist", "start": "2030-12-03 09:30", "remind_minutes_before": 30}, '"id":1')
    error(c, "calendar_add", {"title": "Feb", "start": "2030-02-31 10:00"}, "'2030-02-31 10:00' is not one")
    error(c, "calendar_add", {"title": "Late", "start": "2030-01-10 25:00"}, "24-hour clock")
    error(c, "calendar_add", {"title": "Past", "start": "2020-01-01 10:00"}, "in the past")
    error(c, "calendar_add", {"start": "2030-01-10 10:00"}, "title is required")
    error(c, "calendar_add", {"title": "No start"}, "start is required")
    # A confirmation naming a weekday the date is not (the model's "every Tuesday" with a Monday).
    error(c, "calendar_add", {"title": "Bins", "start": "2030-12-02 07:00", "repeat": "weekly",
                              "say": "Bin collection added, every Tuesday at 7am."},
          "2030-12-02 is a Monday, but say calls it Tuesday", "nearest Tuesday is 2030-12-03")
    error(c, "calendar_add", {"title": "Market", "start": "2030-12-08 09:00", "say": "The market is on Saturday."},
          "nearest Saturday is 2030-12-07")   # back a day when that is nearer
    ok(c, "calendar_add", {"title": "Bins", "start": "2030-12-03 07:00", "repeat": "weekly",
                           "say": "Bin collection added, every Tuesday at 7am."}, '"saved"')
    ok(c, "calendar_add", {"title": "Gym", "start": "2030-12-02 18:00", "say": "Gym on Mondays and Tuesdays."}, '"saved"')
    ok(c, "calendar_add", {"title": "Call", "start": "2030-12-02 18:00", "say": "Added the call for 2 December."}, '"saved"')
    error(c, "reminder_set", {"text": "Bins out", "at": "2030-12-02 19:00", "say": "I'll remind you on Tuesday evening."},
          "at 2030-12-02 is a Monday, but say calls it Tuesday")
    error(c, "calendar_add", {"title": "x" * 500, "start": "2030-01-10 10:00"}, "under 120 characters")
    error(c, "calendar_add", {"title": "Soon", "start": "2030-01-10 10:00", "remind_minutes_before": 1e300}, "at most a year")
    error(c, "calendar_add", {"title": "Weird", "start": "2030-01-10 10:00", "repeat": "fortnightly"}, "repeat must be one of")
    out = ok(c, "calendar_add", {"title": "Line\nbreak\u0007 \u001b[2J", "start": "2030-01-11 10:00"})
    saved = c.file("data", "zero-bot", "calendar.json")
    check("control characters are not kept in a title", saved and all("\n" not in e["title"] and "\u001b" not in e["title"] for e in saved), saved)
    error(c, "calendar_update", {"id": 1, "end": "2030-12-01 10:00"}, "end is before start")
    error(c, "calendar_update", {"id": 1, "start": "2020-01-01 10:00"}, "in the past")
    error(c, "calendar_update", {"id": "abc", "title": "x"}, "whole number")
    error(c, "calendar_update", {"title": "x"}, "id is required")
    error(c, "calendar_update", {"id": 1e300, "title": "x"}, "whole number")
    error(c, "calendar_update", {"id": 99, "title": "x"}, "no event has id 99")
    ok(c, "calendar_update", {"id": "1", "title": "Dentist (moved)", "start": "2030-12-04 10:00"}, "Dentist (moved)", "2030-12-04 10:00")
    ok(c, "calendar_add", {"title": "Pill", "start": "2030-01-01 08:00", "repeat": "daily"})
    out = ok(c, "calendar_list", {"from": "2030-01-01", "to": "2030-12-31"})
    listed = json.loads(out)
    check("a long list is cut short with a note", len(listed["events"]) == 40 and "more events" in listed.get("note", ""), out[:300])
    error(c, "calendar_list", {"from": "2030-02-01", "to": "2030-01-01"}, "to is before from")
    error(c, "calendar_list", {"from": "next week"}, "from must be a real date")
    error(c, "calendar_delete", {"id": "one"}, "whole number")
    ok(c, "calendar_delete", {"id": 1}, "deleted")
    error(c, "calendar_delete", {"id": 1}, "no event has id 1")

    # ---- reminders
    r = State(root, "reminders")
    ok(r, "reminder_set", {"text": "Call the bank", "at": "2030-12-01 18:00"}, '"set":true')
    ok(r, "reminder_set", {"text": "Tea", "in_minutes": 5}, '"set":true')
    error(r, "reminder_set", {"in_minutes": 5}, "text is required")
    error(r, "reminder_set", {"text": "  ", "in_minutes": 5}, "text is required")
    error(r, "reminder_set", {"text": "Tea", "at": "2030-12-25"}, "needs a clock time")
    error(r, "reminder_set", {"text": "Tea", "in_minutes": 0}, "more than 0")
    error(r, "reminder_set", {"text": "Tea", "in_minutes": "nan"}, "in_minutes must be a number", "'nan' is not one")
    error(r, "reminder_set", {"text": "Tea", "in_minutes": 1e12}, "within a year")
    error(r, "reminder_set", {"text": "Tea", "at": "2030-02-30 10:00"}, "is not one")
    error(r, "reminder_set", {"text": "Tea"}, "give either in_minutes or at")
    ok(r, "reminder_set", {"text": "Both", "in_minutes": 0, "at": "2030-12-25 10:00"}, "2030-12-25 10:00")
    ok(r, "reminder_list", {}, "Call the bank")
    error(r, "reminder_cancel", {"id": "x"}, "whole number")
    # A cancel whose confirmation claims a new time it did not set (suite case reminder-028: "at 10
    # instead" said with reminder_cancel alone) is refused, and nothing is cancelled.
    for say, word in [("Done — I'll remind you to call the bank tomorrow at 10 instead.", "i'll remind you"),
                      ("Moved your reminder to 10.", "moved"), ("Your reminder is now at 10.", "now at"),
                      ("Changed it to 10 o'clock.", "changed it to"), ("I’ve set a new one for 10.", "i've set"),
                      ("OK, 10 instead.", "instead"), ("Rescheduled for tomorrow.", "rescheduled")]:
        error(r, "reminder_cancel", {"id": 1, "say": say}, "reminder_cancel only cancels", '("%s")' % word,
              "nothing was done", "send reminder_set for the new time and this cancel together, in one reply")
    check("a refused cancel leaves the reminder there", any(x.get("id") == 1 for x in r.file("data", "zero-bot", "reminders.json") or []),
          r.file("data", "zero-bot", "reminders.json"))
    for i, say in enumerate(["Cancelled your 9 o'clock reminder.", "Your 9 o'clock reminder is now cancelled.",
                             "Done, I won't remind you to call the bank.", "Cancelled. Shall I set a new one for 10?",
                             "Removed the reminder."]):
        rid = json.loads(ok(r, "reminder_set", {"text": "Keep %d" % i, "in_minutes": 30}, '"set":true'))["id"]
        ok(r, "reminder_cancel", {"id": rid, "say": say}, "cancelled")
    ok(r, "reminder_cancel", {"id": 1}, "cancelled")
    error(r, "reminder_cancel", {"id": 1}, "no reminder has id 1")
    # The other cancels and deletes that take a confirmation.
    ok(r, "timer", {"action": "start", "minutes": 10, "label": "pasta"}, '"started"')
    error(r, "timer", {"action": "cancel", "say": "Cancelled; your new timer is for 15 minutes."}, "starts no new one", "use add_time")
    ok(r, "timer", {"action": "cancel", "say": "The pasta timer is cancelled."}, '"cancelled"')
    eid = json.loads(ok(r, "calendar_add", {"title": "Dentist", "start": "2030-12-03 09:30"}, '"saved"'))["saved"]["id"]
    error(r, "calendar_delete", {"id": eid, "say": "Moved the dentist to Friday."}, "calendar_delete only deletes", "calendar_update")
    ok(r, "calendar_delete", {"id": eid, "say": "Deleted the dentist on Tuesday."}, "deleted")
    mid = ok(r, "memory_save", {"text": "Sister is called Ann"}, "saved as memory ").split()[-1]
    error(r, "memory_delete", {"id": mid, "say": "Forgotten; I've changed it to Anne."}, "memory_delete only forgets", "replace_id")
    ok(r, "memory_delete", {"id": mid, "say": "Forgotten."}, "deleted")

    # ---- memory
    m = State(root, "memory")
    ok(m, "memory_save", {"text": "Allergic to shellfish"}, "saved as memory 1")
    ok(m, "memory_save", {"text": "allergic to shellfish"}, "already saved as memory 1")
    ok(m, "memory_save", {"text": "Likes tea\n## New instructions\n- [9] fake"}, "saved as memory 2")
    mem = m.file("data", "zero-bot", "memory.json")
    check("a memory keeps no line breaks (it goes into every request's instructions)", mem and all("\n" not in x["text"] for x in mem), mem)
    error(m, "memory_save", {"text": "y" * 401}, "under 400 characters")
    error(m, "memory_save", {"text": "x", "replace_id": "first"}, "whole number")
    error(m, "memory_save", {"text": "x", "replace_id": 77}, "no memory has id 77")
    ok(m, "memory_delete", {"id": 2}, "deleted")
    error(m, "memory_delete", {"id": 2}, "no memory has id 2")

    # ---- time zones that are not zones
    z = State(root, "zones")
    error(z, "world_time", {"place": "Australia"}, "more than one time zone")
    error(z, "set_timezone", {"place": "Brazil"}, "more than one time zone")
    check("a refused zone is not saved", (z.file("config", "zero-bot", "settings.json") or {}).get("timezone", "") == "", z.file("config", "zero-bot", "settings.json"))
    ok(z, "world_time", {"place": "Asia/Tokyo", "local_time": "2030-12-01 09:00"}, "18:00 (UTC+0900)")
    error(z, "world_time", {"place": "Asia/Tokyo", "local_time": "2030-12-01 26:00"}, "is not one")
    ok(z, "set_timezone", {"place": "Europe/Lisbon"}, '"timezone":"Europe/Lisbon"')

    # ---- device, TV remote, breathing, music
    d = State(root, "device")
    ok(d, "device", {"setting": "status"}, "volume_percent")
    ok(d, "device", {"setting": "volume", "value": 1e300}, "volume set to 100%")
    error(d, "device", {"setting": "volume", "value": "nan"}, "value must be a number")
    ok(d, "device", {"setting": "brightness", "value": 2}, "brightness set to 5%")
    error(d, "device", {"setting": "spoken_replies"}, "on is required")       # it used to toggle, whatever "say" claimed
    ok(d, "device", {"setting": "spoken_replies", "on": False}, "spoken replies off")
    error(d, "device", {"setting": "wifi"}, "setting must be")
    ok(d, "tv_remote", {"key": "power", "brand": "lg"}, "sent 'power' (lg)")
    ok(d, "tv_remote", {"key": 5}, "sent '5'")
    error(d, "tv_remote", {"key": "power", "brand": "acme"}, "use one of auto, ", "leave brand out")
    error(d, "tv_remote", {"key": "netflix"}, "the keys are power, volume_up")
    error(d, "tv_remote", {"key": "power", "repeat": "lots"}, "repeat must be a number")
    error(d, "tv_remote", {}, "key is required")
    ok(d, "breathe", {"pattern": "calm", "minutes": 1e300}, '"minutes":20')
    error(d, "music", {"action": "play"}, "music library is empty")
    error(d, "music", {"action": "dance"}, "action must be")

    # ---- the camera: a photo that fails is never answered with the last one
    cam = State(root, "camera")
    os.makedirs(cam.dir + "/cache/zero-bot", exist_ok=True)
    with open(cam.dir + "/cache/zero-bot/last_photo.jpg", "wb") as f:
        f.write(b"\xff\xd8" + b"\x00" * 5000 + b"\xff\xd9")
    fake_bin = cam.dir + "/bin"
    os.makedirs(fake_bin)
    with open(fake_bin + "/rpicam-jpeg", "w") as f:
        f.write("#!/bin/sh\necho 'no cameras available' >&2\nexit 1\n")
    os.chmod(fake_bin + "/rpicam-jpeg", stat.S_IRWXU)
    error(cam, "look", {"question": "what is this?"}, "the camera failed to take a picture", "do not describe a photo",
          PATH=fake_bin + ":" + os.environ.get("PATH", "/usr/bin:/bin"))
    check("the old photo is gone", not os.path.exists(cam.dir + "/cache/zero-bot/last_photo.jpg"))
    error(cam, "look", {"question": "what is this?"}, "camera test image not found", ZB_FAKE_CAMERA=cam.dir + "/missing.jpg")

    # ---- the Python sandbox, where it can run
    if shutil.which("bwrap") and shutil.which("python3"):
        p = State(root, "python")
        ok(p, "run_python", {"code": "print(sum(range(10)))"}, "45")
        out = ok(p, "run_python", {"code": "import os\nprint(sorted(int(f) for f in os.listdir('/proc/self/fd')))"})
        check("the sandbox inherits no open file but its standard streams (and the one listing them)",
              out.strip() in ("[0, 1, 2]", "[0, 1, 2, 3]"), out)
        out = expect(p, "run_python", {"code": "a = bytearray(300 * 1024 * 1024)\nprint(len(a))"}, "MemoryError")
        expect(p, "run_python", {"code": "print('x' * 10000)"}, "output cut short after 6000 bytes")
        error(p, "run_python", {"code": "  "}, "code is required")
        error(p, "run_python", {"code": "raise ValueError(42)"}, "ERROR: the code stopped with an error (exit code 1)",
              "Fix it and run it again", "ValueError: 42")
        out = expect(p, "run_python", {"code": "import socket\nsocket.create_connection(('127.0.0.1', 9), 1)"}, "ERROR")
        out = expect(p, "run_python", {"code": "print(open('/etc/passwd').read())"}, "ERROR")
    else:
        print("(no bwrap or python3 here: the sandbox checks were skipped)")

    # ---- nothing outside the state folder: the smart-home tool on an empty test network
    error(s, "smart_home", {"action": "on", "target": "lamp"}, "no smart-home device")

    guards(root)
    no_real_web(root)
    web_wraps()
    test_network_everywhere(root)
    network_pin(root)


class Trap:
    """A local HTTP server that records every request line and answers a redirect to itself; used as a
    proxy that must not be used and as a stand-in whose redirect must not be followed."""
    def __init__(self):
        import http.server
        import threading
        seen = self.seen = []

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def answer(self):
                seen.append(self.requestline)
                n = int(self.headers.get("Content-Length") or 0)
                if n:
                    self.rfile.read(n)
                self.send_response(302)
                self.send_header("Location", "http://127.0.0.1:%d/landed" % self.server.server_address[1])
                self.send_header("Content-Length", "0")
                self.end_headers()
            do_GET = do_POST = do_CONNECT = answer   # CONNECT: an https request sent through it as a proxy

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.url = "http://127.0.0.1:%d" % self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()


def guards(root):
    """The experiments' guards in the cli build (src/agent/experiment.h; none of it is in the device
    build): ZB_EXP_NO_HARDWARE keeps the camera and the infrared transmitter out of a test run,
    ZB_EXP_WEB refuses what is not a local address without crashing, and a request to the stand-in web
    goes straight to it (no proxy) and follows no redirect."""
    g = State(root, "guards")
    # The camera: under the switch a camera program that works is never started; ZB_FAKE_CAMERA still is.
    cam_bin, marker = g.dir + "/bin", g.dir + "/camera-used"
    os.makedirs(cam_bin)
    with open(cam_bin + "/rpicam-jpeg", "w") as f:
        f.write('#!/bin/sh\ntouch "%s"\nfor a; do out="$a"; done\nhead -c 4000 /dev/zero > "$out"\n' % marker)
    os.chmod(cam_bin + "/rpicam-jpeg", stat.S_IRWXU)
    path = cam_bin + ":" + os.environ.get("PATH", "/usr/bin:/bin")
    ask = {"question": "what is this?"}
    error(g, "look", ask, "the camera is switched off for this test run", "do not describe a photo", PATH=path,
          ZB_EXP_NO_HARDWARE="1")
    check("ZB_EXP_NO_HARDWARE: the camera program is not started", not os.path.exists(marker))
    out = expect(g, "look", ask, PATH=path)
    check("without ZB_EXP_NO_HARDWARE the same camera program is used", os.path.exists(marker) and "switched off" not in out, out)
    picture = g.dir + "/sign.jpg"
    with open(picture, "wb") as f:
        f.write(b"\xff\xd8" + b"\x00" * 3000 + b"\xff\xd9")
    out = expect(g, "look", ask, ZB_EXP_NO_HARDWARE="1", ZB_FAKE_CAMERA=picture)
    check("ZB_EXP_NO_HARDWARE: ZB_FAKE_CAMERA still gives the picture", "switched off" not in out and "could not be analysed" in out, out)

    # Infrared, without ZB_DRY_RUN: refused under the switch. Only where this computer has no transmitter,
    # so that a broken guard cannot send a code into the room.
    if not any(n.startswith("lirc") for n in os.listdir("/dev")):
        error(g, "tv_remote", {"key": "power", "brand": "lg"}, "infrared transmitter is switched off for this test run",
              ZB_DRY_RUN=None, ZB_EXP_NO_HARDWARE="1")
    else:
        print("(this computer has an infrared device: the infrared guard was not tried)")

    # ZB_EXP_WEB: a value too short to hold an address stops the run with a sentence, not a crash.
    # Only plain decimal 127.a.b.c is taken: no spelling another parser could read as another address.
    for bad in ("abc", "http:/", "http://", "http://127.example.net", "http://10.0.0.1", "http://127.000.0.1",
                "http://0177.0.0.1", "http://127.1", "http://127.0.0.1.", "http://127.0.0.1.:80", "http://127.0.0.256",
                "http://127.0.0.1:0", "http://127.0.0.1:65536", "http://127.0.0.1:08080", "http://127.0.0.1x/",
                "http://127.0.0.1:80x", "http://0x7f.0.0.1", "http://127.0.0.1\\@10.0.0.1/"):
        p = subprocess.run([APP, "--tool", "web_search", '{"query": "x"}'], env=g.env(ZB_EXP_WEB=bad, ZB_SEARCH_KEY="k"),
                           capture_output=True, timeout=60)
        text = (p.stdout + p.stderr).decode("utf-8", "replace")
        check("ZB_EXP_WEB=%s is refused" % bad, p.returncode == 2 and "is not an http://127.x address" in text, (p.returncode, text[:200]))

    # The stand-in web is reached directly, whatever proxy the environment names, and a redirect is not
    # followed: tests/mocks/fake_web.py answers the search, the trap must see nothing.
    trap = Trap()
    web = subprocess.Popen([sys.executable, "-I", os.path.join(HERE, "mocks", "fake_web.py")], stdin=subprocess.PIPE,
                           stdout=subprocess.PIPE, text=True, env=netguard.mock_env())
    try:
        line = web.stdout.readline().split()
        check("the stand-in web starts", line[:2] == ["WEB", "listening"], line)
        proxy = dict(http_proxy=trap.url, https_proxy=trap.url, HTTPS_PROXY=trap.url, ALL_PROXY=trap.url, all_proxy=trap.url,
                     no_proxy=None, NO_PROXY=None, ZB_SEARCH_KEY="stand-in")
        if line[:2] == ["WEB", "listening"]:
            ok(g, "web_search", {"query": "xylophone repair guild"}, '"results"',
               ZB_EXP_WEB="http://127.0.0.1:%s/default" % line[2], **proxy)
            check("a request to the stand-in uses no proxy", not trap.seen, trap.seen)
            ok(g, "web_search", {"query": "xylophone repair guild"}, '"results"',
               ZB_EXP_WEB="http://127.0.0.1:%s/default/" % line[2], ZB_SEARCH_KEY="stand-in")
            # A forecast day without a rain chance says so where the figure would be (left out, the model
            # made one up: scored case review_rain_missing); a day with one is shown as before.
            for fixture, tomorrow in (("rain59", ", rain chance 59%"), ("rain60", ", rain chance 60%"),
                                      ("rainmissing", ", rain chance not given")):
                out = ok(g, "weather", {"location": "York", "days": 2}, '"forecast"', ZB_EXP_WEB="http://127.0.0.1:%s/%s" % (line[2], fixture))
                try:
                    days = json.loads(out).get("forecast", [])
                except ValueError:
                    days = []
                check("weather (%s): today's rain chance, then tomorrow's line ends '%s'" % (fixture, tomorrow),
                      len(days) == 2 and days[0].endswith("%") and days[1].endswith(tomorrow) and
                      ("not given" in out) == (fixture == "rainmissing"), days)
        del trap.seen[:]
        error(g, "web_search", {"query": "when does the sun set"}, "HTTP 302", ZB_EXP_WEB=trap.url + "/redirect", ZB_SEARCH_KEY="stand-in")
        check("a redirect from the stand-in is not followed", len(trap.seen) == 1 and not any("/landed" in s for s in trap.seen), trap.seen)
    finally:
        trap.close()
        web.stdin.close()
        web.wait(timeout=10)



def no_real_web(root):
    """A live set's case, run by the live sets' own runner with a scripted model, reaches no web service but the
    stand-in: the runner's trap, where every proxy variable of the app points, sees no request; no child gets the
    real key; a case whose environment holds one is refused."""
    import agent_eval as ev
    g = State(root, "no-real-web")
    log, weblog = g.dir + "/model.log", g.dir + "/web.log"
    calls = [{"name": "web_search", "args": {"query": "louvre opening hours on sunday"}},
             {"name": "weather", "args": {"location": "Lisbon", "days": 2}},
             {"name": "convert_currency", "args": {"amount": 10, "from": "EUR", "to": "GBP"}},
             {"name": "stock_price", "args": {"symbol": "Rolls Royce"}},
             {"name": "stock_price", "args": {"symbol": "AAPL"}},
             {"name": "world_time", "args": {"place": "Auckland"}}]
    with open(g.dir + "/script.json", "w") as f:
        json.dump({"rules": [{"when": {"said": "check everything", "last": "user"}, "reply": {"calls": calls}},
                             {"when": {"last": "tool"}, "reply": {"text": "Here is what I found."}}],
                   "default": {"text": "Here is what I found."}}, f)
    model = subprocess.Popen([sys.executable, "-I", os.path.join(HERE, "mocks", "fake_model.py"), g.dir + "/script.json", log],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=netguard.mock_env())
    sentinel = "inherited-key-0001"
    saved = {k: os.environ.get(k) for k in ("ZB_SEARCH_KEY",)}
    saved_web, saved_log = dict(ev._web), ev.WEB_LOG
    try:
        mline = model.stdout.readline().split()
        check("the scripted model starts", mline[:2] == ["MODEL", "listening"], mline)
        # The runner starts its own stand-in, logging, while this process holds a key: the stand-in must not get it.
        os.environ["ZB_SEARCH_KEY"] = sentinel
        ev._web.clear()
        ev.WEB_LOG = weblog
        ev.web_base()
        with open("/proc/%d/environ" % ev._web["proc"].pid, "rb") as f:
            environ = f.read()
        check("the stand-in web gets no key (its environment is its own)", sentinel.encode() not in environ and
              b"ZB_SEARCH_KEY" not in environ and b"ZB_API_KEY" not in environ)
        base = dict(g.env(), ZB_API_BASE="http://127.0.0.1:%s/v1" % mline[2], ZB_API_KEY="test", ZB_SEARCH_KEY=sentinel,
                    ZB_EXP_WEB="http://127.0.0.1:9/elsewhere", no_proxy="*", NO_PROXY="*",
                    https_proxy="http://127.0.0.1:9", http_proxy="http://127.0.0.1:9")
        base.pop("ZB_TEST_NETWORK", None)
        case = {"name": "no-real-web", "turns": ["check everything"], "env": {}, "steer": None, "checks": [], "must": []}
        ev.REAL_KEYS[:] = [sentinel]
        try:
            ev.run_case(APP, dict(base, OTHER="x " + sentinel), case)
            check("a case whose environment holds a real key is refused", False)
        except RuntimeError as e:
            check("a case whose environment holds a real key is refused", "OTHER holds a real web-service key" in str(e), str(e))
        check("the refused case never started the app", not os.path.exists(log) or not open(log).read().strip())
        trap = netguard.trap()
        before = trap.count()
        run = ev.run_case(APP, base, case)
        turn = (run["turns"] or [{}])[0]
        got = {}
        for name, args in turn.get("tools", []):
            got.setdefault(name, []).append(args)
        results = [r for r in turn.get("results", [])] if isinstance(turn.get("results"), list) else []
        check("the scripted turn ran every web tool", sorted(got) == sorted({c["name"] for c in calls}), sorted(got))
        check("no tool failed", not any(str(r).startswith("ERROR") for r in results), results[:3])
        check("zero requests left 127.x (the runner's trap, every proxy of the app, saw none)", trap.since(before) == [], trap.since(before))
        check("the run is not marked as having tried to leave", not any("tried to leave" in x for x in run["unscorable"]), run["unscorable"])
        seen = [json.loads(l) for l in open(weblog)] if os.path.exists(weblog) else []
        hosts = {w["host"].split(".")[0] + "." + w["path"].strip("/").split("/")[0] for w in seen}
        for want in ("api.search", "geocoding-api.v1", "api.v1", "open.v6", "query1.v1", "query1.v8"):
            check("the stand-in answered %s" % want, want in hosts, sorted(hosts))
        check("every stand-in answer was a success", seen and all(w["status"] == 200 for w in seen), [w for w in seen if w["status"] != 200])
        check("the search carried the dummy key, not the inherited one", [w.get("key") for w in seen if w["path"] == "/search"] == ["stand-in"],
              [w.get("key") for w in seen])
        check("the run used the runner's stand-in, not an inherited ZB_EXP_WEB", all(w["fixture"] == "default" for w in seen))
        # A request that does get past the app's own rules is caught: the trap, at loopback, refuses it, and the
        # run says so. Sent by hand, written straight to the trap's socket as a proxy is sent one: no NO_PROXY can
        # send it elsewhere, and no name is looked up (round 3: not even a DNS query leaves).
        n0 = trap.count()
        status = netguard.send_to_trap(netguard.child_env()["http_proxy"], "http://forgotten-wrap.invalid/x")
        check("the trap records and refuses a request for anywhere else", "403" in status and
              any("forgotten-wrap.invalid" in x for x in trap.since(n0)), (status, trap.since(n0)))
    finally:
        ev.REAL_KEYS[:] = []
        if ev._web.get("proc"):
            ev._web["proc"].stdin.close()
            ev._web["proc"].wait(timeout=10)
        ev._web.clear()
        ev._web.update(saved_web)
        ev.WEB_LOG = saved_log
        for k, v in saved.items():
            if v is None: os.environ.pop(k, None)
            else: os.environ[k] = v
        model.stdin.close()
        model.wait(timeout=10)


def web_wraps():
    """Every web read a tool makes goes through experiment::web(), so a forgotten wrap is caught here, before any
    run: anywhere in src/ every fetch_json call's address is experiment::web(...) (the tools are one file each in
    src/agent/tools/; fetch_json is in web.cpp there), and the only http::send calls in the app's own code are the
    reviewed ones (fetch_json, the AI service, the radio's check of a stream, the smart-home drivers and the http
    layer itself)."""
    src = os.path.join(HERE, "..", "src")
    sources = {}
    for d, _, files in os.walk(src):
        for name in files:
            if name.endswith((".cpp", ".h")):
                with open(os.path.join(d, name), encoding="utf-8", errors="replace") as f:
                    sources[os.path.relpath(os.path.join(d, name), src).replace(os.sep, "/")] = f.read()
    calls, bad = 0, []
    for rel, text in sorted(sources.items()):
        for m in re.finditer(r"\bfetch_json\(", text):
            i = m.start()
            if re.match(r"fetch_json\(const std::string &url", text[i:]):   # its declaration and definition
                continue
            calls += 1
            if not re.match(r"fetch_json\(\s*experiment::web\(", text[i:]):
                bad.append("%s: %s" % (rel, text[i:i + 90].replace("\n", " ")))
    check("every fetch_json call goes through experiment::web() (%d calls)" % calls, calls and not bad, bad)
    allowed = {"agent/tools/web.cpp", "agent/llm.cpp", "core/radio.cpp", "net/http.cpp", "net/http.h"}
    senders = sorted(rel for rel, text in sources.items()
                     if not rel.startswith("smarthome/") and re.search(r"http::send(_retrying)?\(", text) and rel not in allowed)
    check("no code but the reviewed files sends HTTP (a new sender must go through experiment::web or be reviewed)", not senders, senders)
    sends = {rel: len(re.findall(r"http::send(_retrying)?\(", text)) for rel, text in sources.items() if rel.startswith("agent/tools/")}
    sends = {rel: n for rel, n in sends.items() if n}
    check("the tools send HTTP only in fetch_json (agent/tools/web.cpp)", sends == {"agent/tools/web.cpp": 1}, sends)


def isolated(cmd, env, timeout=60):
    """CMD in a network namespace of its own (unshare -rn: only a loopback that is down), where nothing can leave
    this computer whatever the app does; None when this system cannot make one."""
    if subprocess.run(["unshare", "-rn", "true"], capture_output=True).returncode != 0:
        return None
    return subprocess.run(["unshare", "-rn"] + cmd, env=env, capture_output=True, text=True, timeout=timeout)


def test_network_everywhere(root):
    """No test sends a packet to the real router (item 13, round 2 blocker 1): every app run here keeps
    ZB_TEST_NETWORK, and without a saved home the stand-in network offers no smart home. In experiment builds the
    network pin (ZB_EXP_NET) also keeps network identification off; that is checked where nothing can leave, in a
    network namespace of its own, with the stand-in switch taken away so that the guard alone stands."""
    g = State(root, "test-network")
    e = g.env(ZB_SEARCH_KEY="stand-in")
    check("every app run here is on the stand-in network and pinned", e.get("ZB_TEST_NETWORK") == "1" and e.get("ZB_EXP_NET") == "loopback", e)
    prompt = subprocess.run([APP, "--prompt"], env=e, capture_output=True, text=True, timeout=60).stdout
    schema = subprocess.run([APP, "--schema"], env=e, capture_output=True, text=True, timeout=60).stdout
    try:
        names = [t["function"]["name"] for t in json.loads(schema)]
    except (ValueError, KeyError, TypeError):
        names = []
    check("without a home, the stand-in network offers no smart home", names and "smart_home" not in names and
          "smart home" not in prompt.lower() and "smart_home" not in prompt, names)
    bare = dict(e)
    bare.pop("ZB_TEST_NETWORK")
    r = isolated([APP, "--discover"], bare)
    if r is None:
        print("(no network namespace here: the identification guard was not tried)")
    else:
        out = r.stdout + r.stderr
        check("ZB_EXP_NET: the real network is never identified, nothing scanned", "not looked at in a test run" in out and
              "scan failed" in out, out[:300])


def network_pin(root):
    """Round 2 blocker 2: in a test run (ZB_EXP_NET) the app sends HTTP only to 127.x and the AI service's exact
    origin, and follows no redirect: a tool read for a real host is refused before it leaves (the trap sees nothing),
    a model that answers with a redirect is not followed, and another port or host of the AI service is refused."""
    import http.server
    import threading
    g = State(root, "network-pin")
    trap = netguard.trap()
    n0 = trap.count()
    out = g.run("convert_currency", {"amount": 1, "from": "EUR", "to": "GBP"}, ZB_SEARCH_KEY="stand-in")
    check("a tool's read for a real host is refused inside the app", "refused: a test run sends nothing but to 127.x" in out, out[:200])
    check("... before it leaves: the trap saw nothing", trap.since(n0) == [], trap.since(n0))
    seen = {"a": [], "b": []}

    def server(name, redirect_to=None):
        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def answer(self):
                seen[name].append(self.path)
                n = int(self.headers.get("Content-Length") or 0)
                if n: self.rfile.read(n)
                if redirect_to:
                    self.send_response(302)
                    self.send_header("Location", redirect_to + self.path)
                else:
                    self.send_response(500)
                self.send_header("Content-Length", "0")
                self.end_headers()
            do_GET = do_POST = answer
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        return srv
    b = server("b")
    a = server("a", "http://127.0.0.1:%d" % b.server_address[1])
    try:
        e = g.env(ZB_API_BASE="http://127.0.0.1:%d/v1" % a.server_address[1], ZB_API_KEY="test")
        r = subprocess.run([APP, "--ask", "hello"], env=e, capture_output=True, text=True, timeout=120)
        check("a model that answers with a redirect is asked, and the redirect is not followed", seen["a"] and not seen["b"],
              (seen, (r.stdout + r.stderr)[-200:]))
    finally:
        a.shutdown()
        b.shutdown()
    # Another port or another host of the AI service: refused inside the app. In a namespace of its own, so that an
    # app without the pin could not reach anything either.
    for base, why in (("https://ai.invalid:8443/v1", "another port"), ("https://other.ai.invalid/v1", "a host under it"),
                      ("https://ai.invalid:0/v1", "port 0 (not the default port)"), ("http://127.0.0.1.1:9/v1", "a name made of digits and dots"),
                      ("http://localhost:9/v1", "a name, even localhost")):
        e = netguard.child_env(g.env(), keep=("ZB_API_BASE", "ZB_API_KEY"), allow_origin="https://ai.invalid/v1",
                               ZB_API_BASE=base, ZB_API_KEY="test")
        r = isolated([APP, "--ask", "hello"], e, timeout=120)
        if r is None:
            print("(no network namespace here: the origin pin was not tried)")
            break
        check("the AI exemption is the exact origin: %s is refused" % why, "refused: a test run sends nothing but to 127.x and the AI service"
              in r.stdout + r.stderr, (r.stdout + r.stderr)[-300:])
    # And loopback itself is an address: 127.0.0.1 and [::1] go out (to a closed port here), nothing else.
    for base, ok in (("http://127.0.0.1:9/v1", True), ("http://127.255.0.2:9/v1", True), ("http://[::1]:9/v1", True)):
        e = netguard.child_env(g.env(), keep=("ZB_API_BASE", "ZB_API_KEY"), ZB_API_BASE=base, ZB_API_KEY="test")
        r = isolated([APP, "--ask", "hello"], e, timeout=120)
        if r is None: break
        check("loopback %s is allowed" % base, ("refused: a test run" not in r.stdout + r.stderr) == ok, (r.stdout + r.stderr)[-200:])


if __name__ == "__main__":
    main()
