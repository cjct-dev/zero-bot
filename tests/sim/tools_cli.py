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
stand-in takes no proxy and follows no redirect. station_tags: the radio stations' tags as the radio tool's schema gives
them (with the station each plays, and focus mode), and a station played by its tag. search_services: web search is offered, and searches with the
right key, exactly when the settings' list of services sets one up (an older file's search_key and ZB_SEARCH_KEY
included), and a settings file saved again keeps the list. search_shapes: every search service built in (read from
src/agent/search/providers/registry.cpp) has its shape in the stand-in web, is asked there with the stand-in's key, gives
the model the same facts as every other, and a refused key and a hostile answer come back as an ERROR or capped and
cleaned. search_failover: the failover between the services against the stand-in's fixtures that put one on a bad day
(one search a run, and several in one run against a scripted model). search_measured: each service asked, and the
cache's answer, noted in the experiments' trace and the activity log, with no words. search_allowance: the allowance file
(search_usage.json) counts searches and keeps Brave's figure, a spent allowance is passed over by a later run, the day's
cap stops a search before any service is asked, the same query again in one run is answered from the cache, and
zero_bot --search-status asks Tavily's GET /usage (a stand-in of its own) for what is left, with the key and nothing
else. search_answers: what the services built in make of answers no fixture holds (too long, '&' everywhere, results
that say nothing, the wrong shape, Brave's rate-limit figures and error codes, a /usage error). home_place: the home
city's place for the scenes' dusk and night (zero_bot --dusk), looked up by the city's name alone, kept and not asked
for again, left unknown when the service does not know the city, cannot be reached or answers with a place off the
globe, and set aside for a zone the user set elsewhere. A tool call's arguments
and a web service's answer nested deeper than 32 levels are not read (nested_too_deep, against a local server of its
own). no_real_web runs
one case through the live sets' own runner
(tests/agent/agent_eval.py run_case) against a scripted model (tests/mocks/fake_model.py) that calls every tool
that reads the web, with every proxy variable pointing at a trap: no request leaves 127.x (the trap sees
none), each service is answered by the stand-in, the search carries the dummy key and never the inherited
one, and a case whose environment holds a real key is refused before the app starts.

usage: tests/sim/tools_cli.py [path/to/zero_bot]          (default: build/cli/zero_bot)
"""
import json
import re
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import urllib.parse

TESTS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # tests/: netguard.py, mocks/
sys.path.insert(0, TESTS)
sys.path.insert(1, os.path.join(TESTS, "agent"))   # the live sets' runner (agent_eval), whose web guard no_real_web runs
sys.dont_write_bytecode = True
import netguard   # noqa: E402  the network boundary (tests/netguard.py)
sys.path.insert(2, os.path.join(TESTS, "lib"))
from search_services import registered as registered_services   # noqa: E402  (kind, name) of each service built in
APP = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(TESTS, "..", "build", "cli", "zero_bot")

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
    # A number sent as text is a number as a whole, spaces aside: "20 minutes" and "0x10" are not numbers.
    error(s, "timer", {"action": "start", "minutes": "20 minutes"}, "minutes must be a number, such as 5; '20 minutes' is not one")
    error(s, "date_info", {"add_days": "0x10"}, "add_days must be a number, such as 5; '0x10' is not one")
    ok(s, "date_info", {"date": "2030-01-01", "add_days": " 7 "}, "2030-01-08")
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
    # "1oops" is not reminder 1 (its first digits were once taken for the id).
    error(r, "reminder_cancel", {"id": "1oops"}, "id must be a whole number such as 12", "'1oops' is not one")
    check("a malformed id cancels nothing", any(x.get("id") == 1 for x in r.file("data", "zero-bot", "reminders.json") or []),
          r.file("data", "zero-bot", "reminders.json"))
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

    # ---- timers picked by id (an argument the schema does not list, so the timer tool reads it itself)
    t = State(root, "timers")
    timers = lambda: [x for x in t.file("data", "zero-bot", "reminders.json") or [] if x.get("kind") == "timer"]
    ok(t, "timer", {"action": "start", "minutes": 10, "label": "pasta"}, '"started"')
    before = timers()
    # A number that is not one is refused before a timer is chosen: with one running, "1oops" once fell through to
    # "the only one" and cancelled it.
    for action in ("cancel", "add_time"):
        error(t, "timer", {"action": action, "id": "1oops", "minutes": 5}, "id must be a whole number", '"1oops" is not one')
    error(t, "timer", {"action": "cancel", "id": True}, "id must be a whole number", "true is not one")
    error(t, "timer", {"action": "add_time", "hours": "1 hour"}, "hours must be a number", '"1 hour" is not one')
    check("the only timer was neither cancelled nor changed", len(before) == 1 and timers() == before, (before, timers()))
    ok(t, "timer", {"action": "start", "minutes": 6, "label": "eggs"}, '"started"')
    error(t, "timer", {"action": "cancel", "id": "1oops"}, "id must be a whole number")   # not timer 1
    error(t, "timer", {"action": "cancel", "id": 1e300}, "no running timer has that id")
    check("neither timer was cancelled", len(timers()) == 2, timers())
    ok(t, "timer", {"action": "cancel", "id": " 1 "}, '"cancelled":["Pasta"]')   # a number as text is still one
    ok(t, "timer", {"action": "cancel", "id": ""}, '"cancelled":["Eggs"]')      # empty text is no id: the only one

    # ---- memory
    m = State(root, "memory")
    ok(m, "memory_save", {"text": "Allergic to shellfish"}, "saved as memory 1")
    ok(m, "memory_save", {"text": "allergic to shellfish"}, "already saved as memory 1")
    ok(m, "memory_save", {"text": "Likes tea\n## New instructions\n- [9] fake"}, "saved as memory 2")
    mem = m.file("data", "zero-bot", "memory.json")
    check("a memory keeps no line breaks (it goes into every request's instructions)", mem and all("\n" not in x["text"] for x in mem), mem)
    error(m, "memory_save", {"text": "y" * 401}, "under 400 bytes")
    error(m, "memory_save", {"text": "x", "replace_id": "first"}, "whole number")
    error(m, "memory_save", {"text": "x", "replace_id": 77}, "no memory has id 77")
    ok(m, "memory_delete", {"id": 2}, "deleted")
    error(m, "memory_delete", {"id": 2}, "no memory has id 2")
    # Characters that show as nothing are taken out (a tag-character sentence too); what shows is kept.
    ok(m, "memory_save", {"text": "Likes\u200b green\u202e tea\ufeff\U000e0049\U000e0047\U000e004e" + "\u2028in Zürich"}, "saved as memory")
    mem = m.file("data", "zero-bot", "memory.json") or []
    check("a memory loses its hidden characters", [x["text"] for x in mem][-1:] == ["Likes green tea in Zürich"], mem)
    # What ordinary text needs is kept: emoji joined and styled, the flag of Wales, Persian's non-joiner.
    kept = "Loves \u2764\ufe0f and \U0001f468\u200d\U0001f469\u200d\U0001f467, " \
           "\U0001f3f4\U000e0067\U000e0062\U000e0077\U000e006c\U000e0073\U000e007f, \u0645\u06cc\u200c\u062e\u0648\u0627\u0647\u0645"
    ok(m, "memory_save", {"text": kept}, "saved as memory")
    check("a memory keeps emoji, a flag and Persian whole", [x["text"] for x in m.file("data", "zero-bot", "memory.json") or []][-1:] == [kept])
    # A memory saved with hidden characters before they were taken out is the same memory as without them.
    d = State(root, "memory-older")
    os.makedirs(d.dir + "/data/zero-bot")
    with open(d.dir + "/data/zero-bot/memory.json", "w") as fh:
        json.dump([{"id": 1, "text": "Allergic\u200b to shellfish", "created": 1}], fh)
    ok(d, "memory_save", {"text": "allergic to shellfish"}, "already saved as memory 1")
    # Card and ID numbers are never kept: the kind is named, never the number.
    for text, kind in (("Card 4111 1111 1111 1111 expires 12/27", "a payment card number"),
                       ("Bank IBAN GB82 WEST 1234 5698 7654 32", "an IBAN"), ("NI number AB 12 34 56 C", "a UK National Insurance number"),
                       ("SSN is 123-45-6789", "a US Social Security number")):
        out = error(m, "memory_save", {"text": text}, "the text holds " + kind, "without the number")
        check("the refusal does not repeat the number", not re.search(r"4111|WEST|12 34|45-6789", out), out)
        error(m, "memory_save", {"text": text, "replace_id": 1}, "the text holds " + kind)
    ok(m, "memory_save", {"text": "Phone 07700 900123, order 112-3456789-1234567, ISBN 978-0-306-40615-7"}, "saved as memory")
    check("nothing refused was saved", not any(re.search(r"4111|WEST|AB 12|45-6789", x["text"]) for x in m.file("data", "zero-bot", "memory.json")),
          m.file("data", "zero-bot", "memory.json"))

    # ---- memory's total (ZB-121): 2,000 bytes in all; what is saved past it stays, only a save that grows waits
    f = State(root, "memory-full")
    for i in range(4):
        ok(f, "memory_save", {"text": "%d" % i + "x" * 399}, "saved as memory %d" % (i + 1))   # 1,600
    ok(f, "memory_save", {"text": "y" * 300}, "saved as memory 5")                             # 1,900
    ok(f, "memory_save", {"text": "w" * 100}, "saved as memory 6")                             # 2,000: full, not over
    out = error(f, "memory_save", {"text": "Likes tea"}, "memory is full: 2000 of its 2000 bytes are used",
                "and this would make 2009", "replace_id", "which memory to forget")
    error(f, "memory_save", {"text": "z" * 150, "replace_id": 6}, "memory is full")   # a replacement that grows
    ok(f, "memory_save", {"text": "Likes tea", "replace_id": 6}, "updated memory 6")   # one that shortens is fine
    ok(f, "memory_save", {"text": "Likes coffee"}, "saved as memory 7")
    over = [{"id": i + 1, "text": "%d" % i + "w" * 399, "created": 1} for i in range(9)]   # 3,600, saved before the total
    with open(f.dir + "/data/zero-bot/memory.json", "w") as fh:
        json.dump(over, fh)
    error(f, "memory_save", {"text": "Likes tea"}, "memory is full: 3600 of its 2000 bytes are used")
    ok(f, "memory_save", {"text": "Likes tea", "replace_id": 9}, "updated memory 9")
    check("memories saved past the total are never removed for a new one", len(f.file("data", "zero-bot", "memory.json") or []) == 9)
    ok(f, "memory_delete", {"id": 2}, "deleted")
    error(f, "memory_save", {"text": "Likes coffee"}, "memory is full")   # 3,600 - 400 - 391 = 2,809: still past it

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
        # One shape for every failure (ZB-090): a JSON answer with a top-level "error", whatever its value, comes out as
        # an ERROR sentence before the note on an unknown argument is added (the note once made it unreadable as JSON,
        # and so a success).
        for value, words in ((5, "ERROR: 5. {\"attempt\":1}"), ({"code": 3}, "ERROR: {\"code\":3}. {\"attempt\":1}"),
                             ("", "ERROR: the tool failed without saying why. {\"attempt\":1}"),
                             (None, "ERROR: the tool failed without saying why. {\"attempt\":1}"),
                             ("disk full", "ERROR: disk full. {\"attempt\":1}")):
            printed = json.dumps({"attempt": 1, "error": value})
            out = error(p, "run_python", {"code": "print(%r)" % printed, "bogus": 1}, words, "(this tool has no argument called 'bogus'")
            check("run_python printing %s: the shaped error comes first" % printed, out.startswith(words), out)
        out = error(p, "run_python", {"code": "print('{\"error\": 5')", "bogus": 1}, "ERROR: {\"error\": 5 (this tool has no argument")
        # The key as JSON reads it, however it is escaped: "error" is "error".
        out = error(p, "run_python", {"code": "print('{\"attempt\": 1, \"err\\\\u006fr\": \"disk full\"}')"},
                    "ERROR: disk full. {\"attempt\":1}")
    else:
        print("(no bwrap or python3 here: the sandbox checks were skipped)")

    # ---- nothing outside the state folder: the smart-home tool on an empty test network
    error(s, "smart_home", {"action": "on", "target": "lamp"}, "no smart-home device")

    station_tags(root)
    guards(root)
    search_services(root)
    search_shapes(root)
    search_failover(root)
    search_measured(root)
    search_allowance(root)
    search_answers(root)
    brave_limits(root)
    home_place(root)
    nested_too_deep(root)
    no_real_web(root)
    web_wraps()
    test_network_everywhere(root)
    network_pin(root)


def station_tags(root):
    """The stations' tags as the model reads them (the radio tool's schema, --schema) and as the tool plays them: the
    tags an allowed value each, with the station each plays (the first in the user's order); focus mode is the tag
    "focus", and without it the model is told not to guess a station and where the tag goes; a tag plays through the
    radio's own path, a name wins over a tag, and a tag no station has is an ERROR naming the tags there are."""
    g = State(root, "tags")
    os.makedirs(g.dir + "/config/zero-bot", exist_ok=True)

    def radio_spec(stations):
        with open(g.dir + "/config/zero-bot/radio.json", "w") as f:
            json.dump({"stations": stations}, f)
        try:
            tools = json.loads(subprocess.run([APP, "--schema"], env=g.env(), capture_output=True, text=True, timeout=60).stdout)
            return next(t["function"] for t in tools if t["function"]["name"] == "radio")
        except (ValueError, KeyError, TypeError, StopIteration):
            return {}
    focus_on = ('For focus or study music or lo-fi, or help to focus, study or concentrate ("help me study" on its own asks for this), '
                'play the tag "focus" rather than offering it.')
    focus_off = 'No station has the tag "focus": asked for focus or study music or lo-fi, or help to focus or study, do not guess a station'
    stations = [{"name": "Harbour FM", "nicknames": ["Harbour"], "url": "http://127.0.0.1:9/harbour.mp3", "tags": ["Pop", "local"]},
                {"name": "Northern Radio 4", "url": "http://127.0.0.1:9/four.mp3", "tags": "talk"},
                {"name": "City Talk", "url": "http://127.0.0.1:9/talk.mp3", "tags": ["talk", "news"]},
                {"name": "Lantern FM", "url": "http://127.0.0.1:9/lantern.mp3", "tags": ["focus", "study", "focus"]}]
    spec = radio_spec(stations)
    tag = spec.get("parameters", {}).get("properties", {}).get("tag", {})
    check("tags: each once, in lower case, as the tag's allowed values", tag.get("enum") == ["pop", "local", "talk", "news", "focus", "study"], tag)
    check("tags: the station each plays", '"pop", "local": "Harbour FM"; "talk": "Northern Radio 4"; "news": "City Talk"; '
          '"focus", "study": "Lantern FM"' in tag.get("description", ""), tag)
    check("tags: focus mode named in the description", focus_on in spec.get("description", "") and focus_off not in spec.get("description", ""),
          spec.get("description"))
    ok(g, "radio", {"action": "play", "tag": "talk"}, '{"playing":"Northern Radio 4"}')
    ok(g, "radio", {"action": "play", "tag": "focus"}, '{"playing":"Lantern FM"}')
    ok(g, "radio", {"action": "play", "station": "City Talk", "tag": "focus"}, '{"playing":"City Talk"}')
    ok(g, "radio", {"action": "play", "station": "harbour"}, '{"playing":"Harbour FM"}')
    error(g, "radio", {"action": "play", "tag": "jazz"}, 'none of the user\'s stations has the tag "jazz"',
          'Their tags are "pop", "local", "talk", "news", "focus", "study"', "radio.json")
    error(g, "radio", {"action": "play", "tag": 5}, "give the station to play")   # not text: as no tag, like a station
    for name, odd in (("not a list", {"a": 1}), ("a list of numbers", [1, 2]), ("empty words", ["", "  "])):
        spec = radio_spec([dict(stations[0], tags=odd)])
        check("tags %s: no tag parameter, and focus mode says how to add the tag" % name,
              "tag" not in spec.get("parameters", {}).get("properties", {}) and focus_off in spec.get("description", "")
              and '"focus" to one in their station file, radio.json.' in spec.get("description", ""), spec)
    spec = radio_spec([dict(stations[0], tags=["lofi"]), stations[3]])
    check("tags: with a station tagged lofi, focus mode leaves lo-fi to that tag",
          'For focus or study music, or help to focus, study or concentrate ("help me study" on its own asks for this), play the tag '
          '"focus" rather than offering it.' in spec.get("description", ""), spec)


class LocalServer:
    """An HTTP server on 127.0.0.1, on a port the system picks, served by a thread of its own."""
    def __init__(self, handler):
        import http.server
        import threading
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.url = "http://127.0.0.1:%d" % self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()


class Trap(LocalServer):
    """A local HTTP server that records every request line and answers a redirect to itself; used as a
    proxy that must not be used and as a stand-in whose redirect must not be followed."""
    def __init__(self):
        import http.server
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

        super().__init__(H)


class Answering(LocalServer):
    """A local HTTP server that answers every request with `status`, `headers` and `body` (each settable between runs; the
    body JSON text or bytes), for answers no stand-in fixture holds (tests/mocks/fake_web.py stays as it is: its bytes are
    hashed into every live case). It notes each request in `seen` (its line, the key it carried as a bearer token and the
    length of any body sent), and counts in `cut` the answers the other side closed before they were written whole."""
    def __init__(self):
        import http.server
        owner = self
        self.status, self.headers, self.body, self.seen, self.cut = 200, {}, "{}", [], 0

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def answer(self):
                n = int(self.headers.get("Content-Length") or 0)
                if n:
                    self.rfile.read(n)
                owner.seen.append((self.requestline, self.headers.get("Authorization", ""), n))
                raw = owner.body if isinstance(owner.body, bytes) else owner.body.encode()
                try:
                    self.send_response(owner.status)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(raw)))
                    for name, value in owner.headers.items():
                        self.send_header(name, value)
                    self.end_headers()
                    self.wfile.write(raw)
                except (BrokenPipeError, ConnectionResetError):
                    owner.cut += 1
            do_GET = do_POST = answer

        super().__init__(H)


def search_services(root):
    """Web search is set up by the settings' list of services (Settings::search, src/agent/search/): an entry counts when
    it is switched on, of a kind this version knows, with a key; an older file's search_key is one Tavily entry, and
    ZB_SEARCH_KEY is the Tavily entry's key. Exactly then is web_search offered and the prompt says search is available,
    and the search carries that entry's key (the stand-in takes only its own). A settings file saved again keeps the
    list, a newer version's entry with its fields included, and no longer holds search_key."""
    web = subprocess.Popen([sys.executable, "-I", os.path.join(TESTS, "mocks", "fake_web.py")], stdin=subprocess.PIPE,
                           stdout=subprocess.PIPE, text=True, env=netguard.mock_env())
    try:
        line = web.stdout.readline().split()
        check("the stand-in web starts", line[:2] == ["WEB", "listening"], line)
        later = {"kind": "later", "key": "later-key", "monthly": 5}
        forms = (("an older file's search_key", {"search_key": "stand-in"}, None, True),
                 ("a Tavily entry", {"search": [{"kind": "tavily", "key": "stand-in"}]}, None, True),
                 ("a newer version's kind first", {"search": [later, {"kind": "tavily", "key": "stand-in"}]}, None, True),
                 ("ZB_SEARCH_KEY over the Tavily entry's key", {"search": [{"kind": "tavily", "key": "wrong"}]}, "stand-in", True),
                 ("ZB_SEARCH_KEY without a list", {}, "stand-in", True),
                 ("an entry switched off", {"search": [{"kind": "tavily", "key": "stand-in", "enabled": False}]}, None, False),
                 ("only a newer version's kind", {"search": [dict(later, key="stand-in")]}, None, False),
                 ("an entry without a key", {"search": [{"kind": "tavily", "key": ""}]}, None, False),
                 ("an older file's empty search_key", {"search_key": ""}, None, False))
        for i, (name, settings, env_key, works) in enumerate(forms):
            g = State(root, "search-%d" % i)
            os.makedirs(g.dir + "/config/zero-bot", exist_ok=True)
            with open(g.dir + "/config/zero-bot/settings.json", "w") as f:
                json.dump(settings, f)
            e = g.env(ZB_EXP_WEB="http://127.0.0.1:%s/default" % line[2], ZB_SEARCH_KEY=env_key)
            prompt = subprocess.run([APP, "--prompt"], env=e, capture_output=True, text=True, timeout=60).stdout
            try:
                names = [t["function"]["name"] for t in json.loads(
                    subprocess.run([APP, "--schema"], env=e, capture_output=True, text=True, timeout=60).stdout)]
            except (ValueError, KeyError, TypeError):
                names = []
            check("%s: web_search %s" % (name, "offered" if works else "not offered"), names and ("web_search" in names) == works, names)
            check("%s: the prompt says search is %s" % (name, "available" if works else "not set up"),
                  ("- Web search: available\n" in prompt) == works and ("- Web search: not set up" in prompt) != works)
            if works:
                ok(g, "web_search", {"query": "louvre opening hours on sunday"}, '"results":[{', ZB_EXP_WEB=e["ZB_EXP_WEB"],
                   ZB_SEARCH_KEY=env_key)
            else:
                error(g, "web_search", {"query": "louvre opening hours on sunday"}, "web search is not set up on this device",
                      ZB_EXP_WEB=e["ZB_EXP_WEB"])
        g = State(root, "search-2")   # the newer version's entry first: a save keeps it whole, and the list
        ok(g, "set_timezone", {"place": "Europe/Lisbon"}, '"timezone":"Europe/Lisbon"')
        saved = g.file("config", "zero-bot", "settings.json") or {}
        check("a settings file saved again keeps the list in order, a newer version's entry whole",
              saved.get("search") == [dict(later, enabled=True), {"kind": "tavily", "key": "stand-in", "enabled": True}], saved.get("search"))
        g = State(root, "search-0")   # the older file's search_key
        ok(g, "set_timezone", {"place": "Europe/Lisbon"}, '"timezone":"Europe/Lisbon"')
        saved = g.file("config", "zero-bot", "settings.json") or {}
        check("an older file's search_key is saved as one Tavily entry, and no longer as search_key",
              saved.get("search") == [{"kind": "tavily", "key": "stand-in", "enabled": True}] and "search_key" not in saved, saved)
    finally:
        web.stdin.close()
        web.wait(timeout=10)


class StandInWeb:
    """tests/mocks/fake_web.py, logging every request: url(fixture) is ZB_EXP_WEB for that fixture, and since(mark) the
    requests logged after `mark` (a count of lines)."""
    def __init__(self, root):
        self.log = os.path.join(root, "stand-in-%d.log" % id(self))
        self.proc = subprocess.Popen([sys.executable, "-I", os.path.join(TESTS, "mocks", "fake_web.py"), "--log", self.log],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=netguard.mock_env())
        line = self.proc.stdout.readline().split()
        check("the stand-in web starts", line[:2] == ["WEB", "listening"], line)
        self.port = line[2] if len(line) > 2 else "9"

    def url(self, fixture):
        return "http://127.0.0.1:%s/%s" % (self.port, fixture)

    def lines(self):
        return [json.loads(l) for l in open(self.log)] if os.path.exists(self.log) else []

    def since(self, mark):
        return self.lines()[mark:]

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=10)


SEARCHED = "louvre opening hours on sunday"   # a search.json rule's question: its facts come back from every service


def write_services(g, services):
    """SERVICES ([kind, key], ...) as the settings' list of search services, each entry keeping what the app saved
    beside it (a refusal's mark)."""
    os.makedirs(g.dir + "/config/zero-bot", exist_ok=True)
    path = g.dir + "/config/zero-bot/settings.json"
    saved = json.load(open(path)) if os.path.exists(path) else {}
    saved["search"] = [dict(saved_entry(saved, kind), kind=kind, key=key) for kind, key in services]
    with open(path, "w") as f:
        json.dump(saved, f)


def search_with(g, web, fixture, services, query=SEARCHED):
    """One web_search through the app with SERVICES as the settings' list (each [kind, key]) at the stand-in's FIXTURE:
    (the result, the stand-in's requests for it)."""
    write_services(g, services)
    mark = len(web.lines())
    out = g.run("web_search", {"query": query}, ZB_EXP_WEB=web.url(fixture))
    return out, web.since(mark)


def saved_entry(saved, kind):
    """The settings' entry of KIND as the app last saved it (what it keeps beside the key), or {}."""
    return next((dict(e) for e in saved.get("search", []) if e.get("kind") == kind), {})


def search_shapes(root):
    """Every search service built into the app (src/agent/search/providers/registry.cpp, read here) has its shape in the
    stand-in web (fake_web.SEARCH_SHAPES), is asked there with the stand-in's key when it is the one set up, and its answer
    reaches the model as the same facts whichever service gave it; a key the service refuses, and a hostile answer,
    come back as the model's ERROR or capped and cleaned."""
    sys.path.insert(0, os.path.join(TESTS, "mocks"))
    import fake_web
    services = registered_services()
    check("the registry lists the services built in, Tavily first", services and services[0][0] == "tavily", services)
    web = StandInWeb(root)
    try:
        rule = fake_web.search_rule(fake_web.FIXTURES["default"], SEARCHED)
        facts = [fake_web.fill(fake_web.shown(v)) for v in fake_web.facts(rule["id"], 1).values()]
        for kind, name in services:
            check("%s has a shape in the stand-in web" % name, kind in fake_web.SEARCH_SHAPES, sorted(fake_web.SEARCH_SHAPES))
            g = State(root, "shape-" + kind)
            out, asked = search_with(g, web, "default", [[kind, "stand-in"]])
            check("%s is asked, once, with the stand-in's key" % name, [(a.get("service"), a.get("key")) for a in asked] == [(kind, "stand-in")], asked)
            check("%s: the answer holds the rule's facts %s" % (name, facts), all(f in out for f in facts) and not out.startswith("ERROR"), out[:400])
            # Text as text: Brave marks the words searched for with <strong> and writes "'" as &#x27;.
            plain = "Next year's championships begin on the last Monday of June."
            out, asked = search_with(g, web, "default", [[kind, "stand-in"]], query="who won the mens singles at wimbledon this year")
            check("%s: the results are plain text, as served (no markup, no entities)" % name, plain in out and
                  not re.search(r"<strong>|&#x27;|&quot;|&amp;", out), out[:400])
            out, asked = search_with(g, web, "default", [[kind, "not-the-stand-in"]])
            check("%s: a refused key is an ERROR for the model" % name, out.startswith("ERROR") and [a.get("key") for a in asked] == ["other"],
                  (out[:200], asked))
            out, asked = search_with(g, web, "search_hostile", [[kind, "stand-in"]])
            try:
                hits = json.loads(out).get("results", [])
            except (ValueError, AttributeError):
                hits = None
            check("%s: a hostile answer is read, its results four at most" % name, isinstance(hits, list) and 0 < len(hits) <= 4, out[:300])
            size = lambda h, k: len(str(h.get(k, "")).encode())   # a cap is in bytes, then "…" (3 more) when cut
            for h in hits or []:
                if not isinstance(h, dict): continue
                check("%s: a hostile title is capped and cleaned" % name, size(h, "title") <= 203 and
                      not re.search(r"[\x00-\x1f]", h.get("title", "")), h.get("title", "")[:80])
                check("%s: a hostile source and text are capped" % name, size(h, "source") <= 303 and size(h, "text") <= 503,
                      (size(h, "source"), size(h, "text")))
    finally:
        web.close()


def searches_in_one_run(g, web, fixture, services, n, same=False, **extra):
    """N web searches in one run of the app (--chat, against a scripted model that searches once for each message, each
    time in other words for the same rule of the stand-in, or with `same` in the same words, which the app's cache then
    answers), with SERVICES as the settings' list at the stand-in's FIXTURE (EXTRA: more of the app's environment): the
    services the stand-in was asked, in order, and the tool results."""
    write_services(g, services)
    script = g.dir + "/search-script.json"
    searches = [{"when": {"last": "user", "said": "search number %d" % i},
                 "reply": {"calls": [{"name": "web_search", "args": {"query": SEARCHED if same else "%s (%d)" % (SEARCHED, i)}}]}}
                for i in range(n)]
    with open(script, "w") as f:
        json.dump({"rules": searches + [{"when": {"last": "tool"}, "reply": {"text": "Done."}}], "default": {"text": "Done."}}, f)
    model = subprocess.Popen([sys.executable, "-I", os.path.join(TESTS, "mocks", "fake_model.py"), script, g.dir + "/model.log"],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=netguard.mock_env())
    try:
        line = model.stdout.readline().split()
        mark = len(web.lines())
        env = g.env(ZB_EXP_WEB=web.url(fixture), ZB_API_BASE="http://127.0.0.1:%s/v1" % line[2], ZB_API_KEY="test", ZB_FAST_MODE="0",
                    **extra)
        p = subprocess.run([APP, "--chat"], input="".join("search number %d\n" % i for i in range(n)), env=env,
                           capture_output=True, text=True, timeout=60 + 10 * n)
        return [a.get("service") for a in web.since(mark)], p.stdout
    finally:
        model.stdin.close()
        model.wait(timeout=10)


def search_failover(root):
    """The failover between search services (src/agent/search/search.cpp), against the stand-in's fixtures that put one
    service on a bad day: the services are asked in the user's order, the next only when the one before failed or found
    nothing (never two paid searches otherwise), the answer the model reads is the one that worked, one ERROR sentence
    when none did; a refused key is never sent again, not even by a later run; a spent service, or one that said nothing
    is left, is passed over; one that failed twice in a row rests; a slow first service is given 5 s, a service alone
    the whole 8; a query holding a card or ID number reaches none."""
    sys.path.insert(0, os.path.join(TESTS, "mocks"))
    import fake_web
    rule = fake_web.search_rule(fake_web.FIXTURES["default"], SEARCHED)
    facts = [fake_web.shown(v) for v in fake_web.facts(rule["id"], 1).values()]
    TB, BT = [["tavily", "stand-in"], ["brave", "stand-in"]], [["brave", "stand-in"], ["tavily", "stand-in"]]
    web = StandInWeb(root)
    try:
        # One search each: (fixture, the settings' order, the services asked, whether the facts come back).
        for fixture, order, asked_want, works in (
                ("default", TB, ["tavily"], True), ("default", BT, ["brave"], True),
                ("tavily_down", TB, ["tavily", "brave"], True), ("tavily_spent", TB, ["tavily", "brave"], True),
                ("tavily_empty", TB, ["tavily", "brave"], True), ("brave_busy", BT, ["brave", "tavily"], True),
                ("brave_spent", BT, ["brave", "tavily"], True), ("all_down", TB, ["tavily", "brave"], False)):
            g = State(root, "failover-%s-%s" % (fixture, order[0][0]))
            out, asked = search_with(g, web, fixture, order)
            label = "%s, %s first" % (fixture, order[0][0])
            check(label + ": asked " + " then ".join(asked_want), [a.get("service") for a in asked] == asked_want, asked)
            check(label + ": a second service only after the first failed or found nothing",
                  all(a["status"] != 200 or fixture == "tavily_empty" for a in asked[:-1]), asked)
            check(label + ": every request carried the stand-in's key", all(a.get("key") == "stand-in" for a in asked), asked)
            if works:
                check(label + ": the facts come back", all(f in out for f in facts) and not out.startswith("ERROR"), out[:300])
            else:
                check(label + ": one sentence when none answered", out.startswith("ERROR: web search is not available right now") and
                      out.count("ERROR") == 1, out[:300])

        # A refused key: the next service answers, and the key is not sent again, by this run or a later one.
        g = State(root, "failover-refused")
        out, asked = search_with(g, web, "brave_key_refused", BT)
        check("brave_key_refused: Brave, then Tavily answers", [a.get("service") for a in asked] == ["brave", "tavily"] and
              all(f in out for f in facts), (asked, out[:200]))
        saved = saved_entry(g.file("config", "zero-bot", "settings.json") or {}, "brave")
        check("the refusal is kept beside the key, as a print of it (16 hex digits), never the key",
              re.fullmatch(r"[0-9a-f]{16}", saved.get("refused", "")) is not None and "stand-in" not in saved.get("refused", ""), saved)
        out, asked = search_with(g, web, "default", BT)
        check("a later run does not send the refused key again", [a.get("service") for a in asked] == ["tavily"], asked)

        # A card or ID number never goes to a search service (ZB-122): refused before any is asked, naming the kind.
        g = State(root, "failover-private")
        for query, kind in (("is 4111 1111 1111 1111 a visa card", "a payment card number"),
                            ("who owns GB82 WEST 1234 5698 7654 32", "an IBAN"), ("123-45-6789 lookup", "a US Social Security number")):
            out, asked = search_with(g, web, "default", TB, query=query)
            check("a query holding %s: refused, naming the kind and not the number, and no service asked" % kind,
                  out.startswith("ERROR: the query holds " + kind) and "Search without it" in out and
                  not re.search(r"4111|WEST|6789", out) and asked == [], (out[:200], asked))
        for query in ("opening hours of 112-3456789-1234567 order desk 07700 900123", "summer olympics medal table 2012 2016 2020 2024",
                      "imei 490154203237518 blocked"):
            out, asked = search_with(g, web, "default", TB, query=query)
            check("a query with no card or ID number is searched: " + query, [a.get("service") for a in asked][:1] == ["tavily"], (out[:200], asked))

        # Several searches in one run: what the run remembers of each service.
        for fixture, order, n, want, why in (
                ("tavily_down", TB, 3, ["tavily", "brave", "tavily", "brave", "brave"], "two failures in a row: it rests"),
                ("tavily_spent", TB, 2, ["tavily", "brave", "brave"], "a spent allowance: passed over"),
                ("brave_left_0", BT, 2, ["brave", "tavily"], "an answer that says nothing is left: passed over after it"),
                ("brave_busy", BT, 3, ["brave", "tavily", "brave", "tavily", "tavily"], "too many at once is a failure, not a spent month")):
            g = State(root, "failover-run-%s" % fixture)
            asked, printed = searches_in_one_run(g, web, fixture, order, n)
            check("%s, %d searches in one run: %s" % (fixture, n, why), asked == want, (asked, printed[-300:]))

        # Deadlines: a slow first service is left after 5 s and the next answers; a service alone is given 8 s.
        g = State(root, "failover-slow")
        started = time.monotonic()
        out, asked = search_with(g, web, "tavily_slow", TB)
        took = time.monotonic() - started
        check("tavily_slow: left after 5 s, Brave answers", 4.5 <= took < fake_web.SLOW_S and all(f in out for f in facts),
              (round(took, 1), asked, out[:200]))
        started = time.monotonic()
        out, asked = search_with(g, web, "tavily_slow", TB[:1])
        took = time.monotonic() - started
        check("tavily_slow, Tavily alone: waited for, inside the 8 s", fake_web.SLOW_S <= took < 8.5 and all(f in out for f in facts),
              (round(took, 1), out[:200]))
    finally:
        web.close()


# Tavily's documented example answer to GET /usage: the key has used 150 of 1,000 credits, the plan 500 of 15,000.
TAVILY_USAGE = {"key": {"usage": 150, "limit": 1000, "search_usage": 100, "extract_usage": 25, "crawl_usage": 15, "map_usage": 7,
                        "research_usage": 3},
                "account": {"current_plan": "Bootstrap", "plan_usage": 500, "plan_limit": 15000, "paygo_usage": 25, "paygo_limit": 100,
                            "search_usage": 350, "extract_usage": 75, "crawl_usage": 50, "map_usage": 15, "research_usage": 10}}


def usage_file(g):
    return g.file("data", "zero-bot", "search_usage.json") or {}


def search_lines(g):
    """The activity log's lines for the services a web search asked (activity::search), without their time stamps."""
    path = os.path.join(g.dir, "cache", "zero-bot", "activity.log")
    lines = open(path, encoding="utf-8").read().splitlines() if os.path.exists(path) else []
    return [l[20:] for l in lines if l[20:].startswith("search ")]


def trace_of(path):
    return [json.loads(l) for l in open(path, encoding="utf-8")] if os.path.exists(path) else []


def search_measured(root):
    """Every service a web search asks, and every answer from its cache, is noted (ZB-103): in the experiments' trace
    (ZB_EXP_TRACE, "search": the service's kind, the outcome, its status, the time, whether the cache answered; in its
    request, "req", when there is one) and in the activity log (DIAGNOSTICS, temporary), with no words: not the query,
    not the answer's."""
    TB = [["tavily", "stand-in"], ["brave", "stand-in"]]
    web = StandInWeb(root)
    try:
        g = State(root, "measured")
        trace = os.path.join(g.dir, "trace.jsonl")
        write_services(g, TB)
        out = g.run("web_search", {"query": SEARCHED}, ZB_EXP_WEB=web.url("tavily_down"), ZB_EXP_TRACE=trace)
        events = [e for e in trace_of(trace) if e.get("ev") == "search"]
        got = [(e.get("kind"), e.get("outcome"), e.get("status"), e.get("cached")) for e in events]
        check("the trace notes each service asked: Tavily down, then Brave's answer", got == [("tavily", "failing", 503, False),
              ("brave", "ok", 200, False)], events)
        check("each with its time, and outside a request no request number", all(isinstance(e.get("ms"), int) and e["ms"] >= 0 and
              "req" not in e for e in events), events)
        lines = search_lines(g)
        check("the activity log has a line for each service asked", len(lines) == 2 and
              re.match(r"^search tavily -> failing, HTTP 503 \(\d+ ms\)$", lines[0]) and
              re.match(r"^search brave -> ok, HTTP 200 \(\d+ ms\)$", lines[1]), lines)
        words = [w for w in SEARCHED.split() if len(w) > 3 and w in (json.dumps(events) + " ".join(lines)).lower()]
        check("neither holds a word of the query or the answer", not words and '"results"' in out and
              not any(chunk in " ".join(lines) + json.dumps(events) for chunk in re.findall(r'"title":"([^"]{12})', out)), words)

        g = State(root, "measured-cache")
        trace = os.path.join(g.dir, "trace.jsonl")
        asked, _ = searches_in_one_run(g, web, "default", TB, 2, same=True, ZB_EXP_TRACE=trace)
        events = [e for e in trace_of(trace) if e.get("ev") == "search"]
        got = [(e.get("req"), e.get("kind"), e.get("outcome"), e.get("cached")) for e in events]
        check("in a conversation each search is in its own request: the service asked, then the cache's answer",
              asked == ["tavily"] and got == [(1, "tavily", "ok", False), (2, None, "ok", True)], (asked, events))
        lines = search_lines(g)
        check("the cache's answer is a line of the activity log too", len(lines) == 2 and lines[1] == "search (cache) -> ok (0 ms)", lines)
    finally:
        web.close()


def search_status(g, base):
    p = subprocess.run([APP, "--search-status"], env=g.env(ZB_EXP_WEB=base), capture_output=True, text=True, timeout=60)
    check("--search-status exits normally", p.returncode == 0, (p.returncode, p.stdout[-300:], p.stderr[-300:]))
    return p.stdout


def search_allowance(root):
    """What the app keeps of each search service's allowance (src/agent/search/usage.cpp, search_usage.json): the
    searches each answered and the day's, Brave's own figure from its reply headers, counts and figures only (no key, no
    query); a spent allowance passed over by a later run too; past the day's cap one ERROR and no service asked; the same
    query again in one run answered from the cache, not counted; and
    Tavily's free GET /usage, asked only by --search-status (the settings page's button), with the key and nothing more:
    its figure kept, a 401 marking the key refused, after which it is not sent again."""
    BT = [["brave", "stand-in"], ["tavily", "stand-in"]]
    web = StandInWeb(root)
    try:
        g = State(root, "allowance-count")
        for query in ("louvre opening hours on sunday", "who won the mens singles at wimbledon this year"):
            search_with(g, web, "default", BT, query=query)
        saved = usage_file(g)
        brave = next((v for k, v in saved.get("services", {}).items() if k.startswith("brave:")), {})
        check("two searches counted, for the day and for Brave, with Brave's figure (812 of 1000 left)",
              saved.get("today") == 2 and brave.get("used") == 2 and brave.get("left") == 812 and brave.get("limit") == 1000, saved)
        text = json.dumps(saved)
        check("the allowance file holds no key and no query", "stand-in" not in text and "louvre" not in text and "wimbledon" not in text, text)
        path = os.path.join(g.dir, "data", "zero-bot", "search_usage.json")
        check("the allowance file can be read by its owner only", os.path.exists(path) and stat.S_IMODE(os.stat(path).st_mode) == 0o600)

        g = State(root, "allowance-spent")
        out, asked = search_with(g, web, "tavily_spent", BT[::-1])
        check("tavily_spent: Tavily, then Brave answers", [a.get("service") for a in asked] == ["tavily", "brave"], asked)
        out, asked = search_with(g, web, "default", BT[::-1])
        check("a later run passes the spent allowance over: only Brave is asked", [a.get("service") for a in asked] == ["brave"], asked)

        g = State(root, "allowance-cap")
        os.makedirs(os.path.join(g.dir, "data", "zero-bot"), exist_ok=True)
        with open(os.path.join(g.dir, "data", "zero-bot", "search_usage.json"), "w") as f:
            json.dump({"version": 1, "day": time.strftime("%Y-%m-%d", time.gmtime()), "today": 100}, f)   # TZ is UTC
        out, asked = search_with(g, web, "default", BT)
        check("past the day's cap: one ERROR, and no service asked", out.startswith("ERROR: web search has reached this device's "
              "limit of 100 searches a day") and not asked, (out[:200], asked))

        g = State(root, "allowance-cache")
        asked, printed = searches_in_one_run(g, web, "default", BT, 3, same=True)
        check("the same query three times in one run: one search, the others answered from the cache",
              asked == ["brave"] and printed.count("Opening hours | Louvre Museum") >= 3 and usage_file(g).get("today") == 1,
              (asked, usage_file(g), printed[-300:]))
    finally:
        web.close()

    server = Answering()
    try:
        g = State(root, "allowance-ask")
        write_services(g, BT)
        server.body = json.dumps(TAVILY_USAGE)
        out = search_status(g, server.url)
        check("--search-status asks Tavily's GET /usage once, with the key as a bearer token and no body",
              server.seen == [("GET /api.tavily.com/usage HTTP/1.1", "Bearer stand-in", 0)], server.seen)
        check("what Tavily said is shown: the key's 850 of 1000 (less than the plan's)", "tavily: ready; 0 this month; it said 850 of "
              "1000 left" in out and "brave: ready; 0 this month\n" in out and "today: 0 of 100 searches" in out, out)
        tavily = next((v for k, v in usage_file(g).get("services", {}).items() if k.startswith("tavily:")), {})
        check("and kept in the allowance file", tavily.get("left") == 850 and tavily.get("limit") == 1000, usage_file(g))
        server.seen.clear()
        server.status, server.body = 401, json.dumps({"detail": {"error": "Unauthorized: missing or invalid API key."}})
        out = search_status(g, server.url)
        check("a 401 from GET /usage: the key is marked refused", "tavily: key refused: type it again" in out and
              re.fullmatch(r"[0-9a-f]{16}", saved_entry(g.file("config", "zero-bot", "settings.json") or {}, "tavily").get("refused", "")),
              out)
        server.seen.clear()
        out = search_status(g, server.url)
        check("and the refused key is not sent again", server.seen == [] and "tavily: key refused" in out, (server.seen, out))
    finally:
        server.close()


def brave_limits(root):
    """ZB-130, found by tests/tools/search_contract.py: Brave's monthly limit is 0 on an unlimited plan, which says
    nothing of an allowance (searches go on, the service is never marked spent), and the query Brave is sent is cut to
    its documented 75 words and 600 characters, at a word. Against a look-alike of its answer here, so that the
    stand-in web (hashed into every live case) stays as it is."""
    import http.server
    import threading
    import urllib.parse
    asked = []

    class Brave(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            asked.append(urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query).get("q", [""])[0])
            body = json.dumps({"type": "search", "web": {"results": [
                {"title": "Eiffel Tower", "url": "https://example.org/eiffel", "description": "The tower is 330 metres tall."}]}}).encode()
            self.send_response(200)
            for name, value in (("Content-Type", "application/json"), ("X-RateLimit-Limit", "1, 0"),
                                ("X-RateLimit-Remaining", "0, 0"), ("X-RateLimit-Reset", "1, 0"), ("Content-Length", str(len(body)))):
                self.send_header(name, value)
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *a):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Brave)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        g = State(root, "brave-limits")
        write_services(g, [["brave", "stand-in-key"]])
        web = "http://127.0.0.1:%d" % server.server_address[1]
        first = g.run("web_search", {"query": "how tall is the eiffel tower"}, ZB_EXP_WEB=web)
        second = g.run("web_search", {"query": "how tall is the eiffel tower in feet"}, ZB_EXP_WEB=web)
        check("an unlimited plan (a month's limit of 0): every search answered", "330 metres" in first and "330 metres" in second,
              (first[:160], second[:160]))
        check("an unlimited plan: Brave is asked again, never passed over as spent", len(asked) == 2, asked)
        usage = json.dumps(g.file("data", "zero-bot", "search_usage.json") or {})
        check("an unlimited plan: no spent allowance is kept", "spent" not in usage.lower() or '"spent_until": 0' in usage, usage[:200])
        words = ["word%d" % i for i in range(120)]
        g.run("web_search", {"query": " ".join(words)}, ZB_EXP_WEB=web)
        sent = asked[-1] if len(asked) == 3 else ""
        check("a long query is cut to Brave's 75 words, at a word", sent.split() == words[:75], sent[:80])
        g.run("web_search", {"query": " ".join(["abcdefghijklmnopqrstuvwxyz"] * 40)}, ZB_EXP_WEB=web)
        sent = asked[-1] if len(asked) == 4 else ""
        check("a long query is cut to Brave's 600 characters, at a word", 0 < len(sent.encode()) <= 600 and
              set(sent.split()) == {"abcdefghijklmnopqrstuvwxyz"}, len(sent.encode()))
    finally:
        server.shutdown()


def home_place(root):
    """ZB-132: where the scenes' dusk and night are reckoned for (zero_bot --dusk, which looks the home city up as
    App::locate_home does). The home city is found at the stand-in's place service, the country choosing among the
    places it lists, kept with its zone and not asked for again; a zone the user set for another place (travelling)
    wins over it, one with no city of its own (Etc/UTC) does not. A city the service does not know, a service that
    cannot be reached and a place off the globe leave it unknown. The request carries the city's name and nothing
    else (a look-alike of the service sees it whole: the stand-in logs no query)."""
    def dusk(g, web, **settings):
        os.makedirs(g.dir + "/config/zero-bot", exist_ok=True)
        path = g.dir + "/config/zero-bot/settings.json"
        saved = json.load(open(path)) if os.path.exists(path) else {}
        saved.update(settings)
        with open(path, "w") as f:
            json.dump(saved, f)
        p = subprocess.run([APP, "--dusk"], env=g.env(ZB_EXP_WEB=web), capture_output=True, timeout=60)
        out = p.stdout.decode("utf-8", "replace")
        check("--dusk with %s exits normally" % json.dumps(settings), p.returncode == 0, (p.returncode, out[:200]))
        return out, (g.file("config", "zero-bot", "settings.json") or {}).get("home_place")

    web = StandInWeb(root)
    try:
        g = State(root, "home-place")
        mark = len(web.lines())
        out, place = dusk(g, web.url("default"), home_city="Sydney", home_country="Australia", timezone="Etc/UTC")
        check("a home city: found, and the sun reckoned for it", "home city: found" in out and "from: the home city" in out, out)
        check("a home city: its coordinates and zone saved", place and abs(place["lat"] + 33.87) < 0.01 and
              abs(place["lon"] - 151.21) < 0.01 and place["zone"] == "Australia/Sydney", place)
        check("a home city: one request, to the place service", [(r["host"], r["path"]) for r in web.since(mark)] ==
              [("geocoding-api.open-meteo.com", "/v1/search")], web.since(mark))
        mark = len(web.lines())
        out, place = dusk(g, web.url("default"))
        check("a home city's place known: not asked for again", web.since(mark) == [] and "home city:" not in out and
              "from: the home city" in out, (out, web.since(mark)))
        out, _ = dusk(g, web.url("default"), timezone="Asia/Tokyo")
        check("a zone set for another place (travelling): that zone's city", "from: the time zone's city; zone Asia/Tokyo" in out, out)
        out, _ = dusk(g, web.url("default"), timezone="Australia/Sydney")
        check("the home city's own zone set: the home city", "from: the home city" in out, out)

        canada = State(root, "home-place-canada")
        out, place = dusk(canada, web.url("default"), home_city="Sydney", home_country="Canada", timezone="Etc/UTC")
        check("the country chooses among the places listed", place and abs(place["lat"] - 46.14) < 0.01 and
              place["zone"] == "America/Glace_Bay", place)

        unknown = State(root, "home-place-unknown")
        out, place = dusk(unknown, web.url("default"), home_city="Atlantis", timezone="Etc/UTC")
        check("a city the service does not know: said, left unknown, fixed hours", "home city: not known to the place service" in out and
              place is None and "from: fixed hours; zone Etc/UTC" in out, (out, place))
        out, place = dusk(unknown, web.url("default"), timezone="Europe/London")
        check("no home place: the zone's city", "from: the time zone's city; zone Europe/London" in out, out)
    finally:
        web.close()

    service = Answering()
    try:
        g = State(root, "home-place-look-alike")
        service.body = json.dumps({"results": [{"name": "Sydney", "latitude": -33.87, "longitude": 151.21, "country_code": "AU",
                                                "country": "Australia", "timezone": "../../etc/passwd"}]})
        out, place = dusk(g, service.url, home_city="Sydney", home_country="Australia", timezone="Etc/UTC")
        line = service.seen[-1][0] if service.seen else ""
        query = urllib.parse.parse_qs(urllib.parse.urlsplit(line.split(" ")[1] if " " in line else "").query)
        check("the request carries the city's name and nothing else", query == {"language": ["en"], "count": ["10"], "name": ["Sydney"]},
              line)
        check("a zone that is not one is not kept", place and place["zone"] == "", place)
        g2 = State(root, "home-place-off-globe")
        service.body = json.dumps({"results": [{"name": "Sydney", "latitude": 123, "longitude": 151.21, "timezone": "Australia/Sydney"}]})
        out, place = dusk(g2, service.url, home_city="Sydney", timezone="Etc/UTC")
        check("a place off the globe: not kept", "home city: not known" in out and place is None, (out, place))
        g3 = State(root, "home-place-unreached")
        service.status, service.body = 503, "<html>down</html>"
        out, place = dusk(g3, service.url, home_city="Sydney", timezone="Etc/UTC")
        check("a place service that cannot be reached: said, left unknown", "home city: not reached" in out and place is None, (out, place))
    finally:
        service.close()


def search_answers(root):
    """What the services built in make of answers no stand-in fixture holds (an Answering server of this test's own): an
    answer far longer than a service sends is cut off, not read to its end, and is no answer; a description full of '&'
    is read at once; results that say nothing are passed over; an answer of the wrong shape is no answer, not "nothing
    found"; Brave's rate-limit figures are taken only as whole numbers in windows that match its limit header, and only
    the month's window at 0 is a spent allowance; Brave's key counts as refused only by a 422 with its code (or a 401); a
    spent month (429 QUOTA_LIMITED) is not sent again, even by Brave alone; and a GET /usage error reaches --search-status
    in the app's own words, none of the answer's."""
    T, B = [["tavily", "stand-in"]], [["brave", "stand-in"]]
    web = Answering()
    states = iter(range(1000))
    fresh = lambda: State(root, "answers-%d" % next(states))   # each check from a first start: no mark kept from another

    def search(g, services, query="anything"):
        write_services(g, services)
        del web.seen[:]
        started = time.monotonic()
        out = g.run("web_search", {"query": query}, ZB_EXP_WEB=web.url + "/answers")
        return out, len(web.seen), time.monotonic() - started

    try:
        web.body = b'{"answer": "Never read.", "results": [], "pad": "' + b"a" * (32 << 20) + b'"}'
        out, n, took = search(fresh(), T)
        check("an answer of 32 MB is cut off, not read to its end, and is no answer",
              out.startswith("ERROR: web search is not available") and web.cut == 1, (out[:120], web.cut))
        web.body = json.dumps({"answer": "Read whole.", "results": [], "pad": "a" * (400 << 10)})
        out, n, took = search(fresh(), T)
        check("an answer of 400 kB is read whole", "Read whole." in out, out[:120])

        took = {}
        for kb in (120, 480):   # each '&' looks a few characters ahead for its ';', not to the end: no time that grows as its square
            web.body = json.dumps({"web": {"results": [{"title": "Ampersands", "url": "https://example.com/", "description": "&" * (kb << 10)}]}})
            out, n, took[kb] = search(fresh(), B)
        check("a description of 480 kB of '&' takes hardly longer than one of 120 kB",
              took[480] - took[120] < 0.5 and "Ampersands" in out, ({k: round(v, 2) for k, v in took.items()}, out[:120]))

        blank = [{"title": "", "url": "https://example.com/blank", "content": " "}] * 3
        good = [{"title": "Result %d" % i, "url": "https://example.com/%d" % i, "content": "Text %d." % i} for i in range(3000)]
        web.body = json.dumps({"answer": "", "results": blank + good})
        out, n, took = search(fresh(), T)
        hits = json.loads(out).get("results", []) if out.startswith("{") else []
        check("results that say nothing are passed over: four read, from the first that says something",
              [h.get("title") for h in hits] == ["Result 0", "Result 1", "Result 2", "Result 3"], out[:300])

        for services, body in ((T, "[]"), (T, '{"results": "none"}'), (T, '{"answer": 5, "results": []}'), (B, "[1]"),
                               (B, '{"web": []}'), (B, '{"web": {"results": {}}}')):
            web.body = body
            out, n, took = search(fresh(), services)
            check("%s answering %s: no answer, not nothing found" % (services[0][0], body),
                  out.startswith("ERROR: web search is not available"), out[:200])
        web.body = json.dumps({"type": "search", "query": {"original": "anything"}})
        out, n, took = search(fresh(), B)
        check("Brave's answer without web results is nothing found", out == '{"results":[]}', out[:200])

        hit = json.dumps({"web": {"results": [{"title": "T", "url": "https://example.com/", "description": "D."}]}})
        both = {"X-RateLimit-Limit": "50, 1000", "X-RateLimit-Reset": "1, 3600"}
        for headers, left, spent, what in (
                (dict(both, **{"X-RateLimit-Remaining": "49, 812"}), 812, False, "both windows: the month's figure"),
                (dict(both, **{"X-RateLimit-Remaining": "49, 0"}), 0, True, "the month's window at 0: spent"),
                (dict(both, **{"X-RateLimit-Remaining": "0, 812"}), 812, False, "the second's window at 0: not spent"),
                ({"X-RateLimit-Limit": "50", "X-RateLimit-Remaining": "0"}, -1, False, "one window only: which, is not known"),
                ({"X-RateLimit-Remaining": "49, 0"}, -1, False, "no limit header to match the windows with"),
                (dict(both, **{"X-RateLimit-Remaining": "49, 812.5"}), -1, False, "not a whole number"),
                (dict(both, **{"X-RateLimit-Remaining": "49, 8e2"}), -1, False, "not written as a whole number"),
                (dict(both, **{"X-RateLimit-Remaining": "0, 812, 3"}), -1, False, "more windows than the limit header has")):
            web.body, web.headers = hit, headers
            g = fresh()
            out, n, took = search(g, B)
            brave = next((v for k, v in (g.file("data", "zero-bot", "search_usage.json") or {}).get("services", {}).items()
                          if k.startswith("brave:")), {})
            check("Brave's figures, %s: %s left%s" % (what, left, ", spent" if spent else ""), "D." in out and
                  brave.get("left", -1) == left and bool(brave.get("spent_until")) == spent, (brave, out[:100]))
        web.headers = {}

        for status, refused in ((422, True), (401, True), (503, False), (429, False), (400, False)):
            web.status = status
            web.body = json.dumps({"type": "ErrorResponse", "error": {"code": "SUBSCRIPTION_TOKEN_INVALID", "detail": "Invalid."}})
            g = fresh()
            out, n, took = search(g, B)
            mark = saved_entry(g.file("config", "zero-bot", "settings.json") or {}, "brave").get("refused", "")
            check("Brave's SUBSCRIPTION_TOKEN_INVALID in a %d: %s" % (status, "the key refused" if refused else "not a refusal"),
                  bool(mark) == refused and out.startswith("ERROR"), (mark, out[:120]))

        web.status = 429
        for body, headers, sends, what in (
                ({"code": "QUOTA_LIMITED"}, {}, 1, "its month spent (QUOTA_LIMITED): sent once"),
                ({"code": "OTHER"}, dict(both, **{"X-RateLimit-Remaining": "49, 0"}), 1, "its month's window at 0: sent once"),
                ({"code": "RATE_LIMITED"}, {}, 2, "too many at once (RATE_LIMITED): sent again, once")):
            web.body, web.headers = json.dumps({"type": "ErrorResponse", "error": body}), headers
            out, n, took = search(fresh(), B)
            check("Brave alone, a 429, " + what, n == sends and out.startswith("ERROR"), (n, out[:120]))
        web.status, web.headers = 200, {}

        g = fresh()
        write_services(g, T)
        web.status, web.body = 500, json.dumps({"detail": {"error": "the token Bearer stand-in is not valid here"}})
        out = search_status(g, web.url)
        check("a GET /usage error is told in the app's own words, none of the answer's",
              "did not say: it answered with an error (HTTP 500)" in out and "Bearer" not in out and "not valid here" not in out, out)
    finally:
        web.close()


def nested_too_deep(root):
    """JSON nested deeper than util::kMaxJsonDepth (32) is not read (ZB-093): a tool call's arguments, and a web
    service's answer, each otherwise right, at 32 levels in all and at 33. Before the guard both were taken."""
    g = State(root, "nested")
    pad = lambda levels: "[" * levels + "]" * levels
    ok(g, "calculator", '{"expression": "2+2", "pad": %s}' % pad(31), "4")
    error(g, "calculator", '{"expression": "2+2", "pad": %s}' % pad(32), "the arguments nest lists or objects more than 32 deep",
          'e.g. {"expression": "..."}')
    web = Answering()
    try:
        for levels, works in ((31, True), (32, False)):
            web.body = '{"answer": "Words from a nested answer.", "results": [], "pad": %s}' % pad(levels)
            env = dict(ZB_EXP_WEB=web.url + "/nested", ZB_SEARCH_KEY="stand-in")
            if works:
                ok(g, "web_search", {"query": "a nested answer"}, "Words from a nested answer.", **env)
            else:
                out = error(g, "web_search", {"query": "a nested answer"}, "web search is not available right now", **env)
                check("a web answer nested 33 levels is not read", "Words from" not in out, out)
    finally:
        web.close()


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
    web = subprocess.Popen([sys.executable, "-I", os.path.join(TESTS, "mocks", "fake_web.py")], stdin=subprocess.PIPE,
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
        error(g, "web_search", {"query": "when does the sun set"}, "web search is not available right now", ZB_EXP_WEB=trap.url + "/redirect",
              ZB_SEARCH_KEY="stand-in")
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
    model = subprocess.Popen([sys.executable, "-I", os.path.join(TESTS, "mocks", "fake_model.py"), g.dir + "/script.json", log],
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
    src = os.path.join(TESTS, "..", "src")
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
