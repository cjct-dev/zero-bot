#!/usr/bin/env python3
"""Tests the scorer of the agent test sets (tests/agent_eval.py, tests/agent_suite.py) offline: no
service, no app, standard library only. Deliberately wrong transcripts must fail and right ones must
pass, so a change to the scorer is known to reject bad behaviour before a live run is spent on it.

Each transcript is written as the command-line agent prints it (`zero_bot --chat`), with or without
the per-call records and the state record that the cli build adds ("[call] {...}", "[state] {...}"),
and goes through the same parser as a live run. The bad ones come from the evaluation review of
19 November 2026: a failed action reported as done, a later turn hiding an earlier mistake, a long
result cut short, a wrong number or time that still matched, a timeout counted once per missing turn.

usage: tests/scorer_test.py [-v]
"""
import datetime
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True   # importing the runners leaves no __pycache__ in tests/
import agent_eval as ev   # noqa: E402
import agent_suite as suite   # noqa: E402

VERBOSE = "-v" in sys.argv[1:]
checks = failed = 0


def check(name, ok, detail=""):
    global checks, failed
    checks += 1
    failed += not ok
    if not ok or VERBOSE:
        print("%s  %s%s" % ("ok  " if ok else "FAIL", name, "" if ok else "  -> %s" % (detail,)), flush=True)
    return ok


def test(f):
    """Runs f now; an exception (a scorer without the function, say) is a failure, not a crash."""
    try:
        f()
    except Exception as e:   # noqa: BLE001
        check(f.__name__, False, "raised %r" % e)
    return f


# ------------------------------------------------------------------ transcripts, as the cli prints them
def cut(s, n=300):
    b = s.encode()
    return s if len(b) <= n else b[:n].decode("utf-8", "ignore") + "…"


def transcript(*turns):
    """turns: dicts with user, calls [(name, args, result)], answer, and optionally state (a dict for the
    "[state]" record) and records=False (an older run: no "[call]" or "[state]" lines). {"new": True}
    is "/new". Returns the text the cli would print."""
    out = ""
    for i, t in enumerate(turns):
        if t.get("new"):
            out += "> "
            continue
        records = t.get("records", True)
        out += "> [user] %s\n" % t["user"]
        for j, (name, args, result) in enumerate(t.get("calls", [])):
            out += "[tool +%d.0s] %s\n" % (j + 1, name)
            if records:
                out += "[call] " + json.dumps({"id": "call_%d_%d" % (i, j), "name": name, "args": json.dumps(args),
                                               "result": result}) + "\n"
        if t.get("answer"):
            out += "[first words +%.1fs]\n" % (len(t.get("calls", [])) + 0.5)
        if not t.get("hide_lines"):   # hide_lines: the old lines lost (history trimmed during the turn)
            for name, args, result in t.get("calls", []):
                out += "  -> %s %s\n" % (name, json.dumps(args))
                out += "  <- %s\n" % cut(result)
        if records and t.get("state") is not None:
            out += "[state] " + json.dumps(t["state"]) + "\n"
        if "answer" in t:
            out += "[answer %.1fs] %s\n" % (t.get("total", 2.0), t["answer"])
    return out + "> "


def turns_of(text):
    """The turns a live run would be scored on (as tests/agent_eval.py's run_case splits them)."""
    return [ev.parse_turn(seg) for seg in ev.TURN_RE.split(text)[1:]]


def case(name):
    for c in ev.CASES:
        if c["name"] == name: return c
    raise KeyError(name)


def suite_case(turns):
    """The generated case with exactly these turns, compiled as tests/agent_suite.py runs it."""
    for c in json.load(open(os.path.join(HERE, "suite", "cases.json"), encoding="utf-8")):
        if c["turns"] == turns: return suite.compile_case(c)
    raise KeyError(turns)


def passes(c, text):
    return ev.score(c, turns_of(text))


NOW = datetime.datetime.now()
EPOCH = int(NOW.timestamp())


def state(timers=(), reminders=(), events=(), memory=()):
    """A "[state]" record: timers as (label, seconds), reminders as (text, datetime), events as
    (title, datetime), memory as texts."""
    rem = [{"id": i + 1, "text": label, "due": EPOCH + secs, "fired": False, "kind": "timer", "started": EPOCH, "length": secs}
           for i, (label, secs) in enumerate(timers)]
    rem += [{"id": 50 + i, "text": text, "due": int(when.timestamp()), "fired": False} for i, (text, when) in enumerate(reminders)]
    cal = [{"id": 100 + i, "title": title, "start": int(when.timestamp()), "end": 0, "notes": "", "remind_min": -1, "repeat": "",
            "all_day": False, "alerted_for": 0, "created": EPOCH} for i, (title, when) in enumerate(events)]
    mem = [{"id": 200 + i, "text": text, "created": EPOCH} for i, text in enumerate(memory)]
    return {"now": EPOCH, "reminders": rem, "calendar": cal, "memory": mem}


def started(label, minutes):
    return json.dumps({"started": {"id": 1, "label": label, "length": "%d minutes" % minutes, "ends_at": "later"}})


def at(day, hhmm):
    return datetime.datetime.combine(day, datetime.time(int(hhmm[:2]), int(hhmm[3:])))


FRIDAY = ev.next_weekday(4)[0]
ERROR = "ERROR: the timer could not be saved. Tell the user it did not work"

# ------------------------------------------------------------------ 1. a failed action reported as done


@test
def failed_timer_reported_as_set():
    bad = transcript({"user": "set a timer for 12 minutes", "answer": "Timer set for 12 minutes.", "state": state(),
                      "calls": [("timer", {"action": "start", "minutes": 12, "say": "Timer set for 12 minutes."}, ERROR)]})
    check("finding 1: a timer whose call failed, then 'Timer set', fails", passes(case("timer"), bad) != [])
    old = transcript({"user": "set a timer for 12 minutes", "answer": "Timer set for 12 minutes.", "records": False,
                      "calls": [("timer", {"action": "start", "minutes": 12}, ERROR)]})
    check("finding 1: the same from a saved run without records fails too", passes(case("timer"), old) != [])
    good = transcript({"user": "set a timer for 12 minutes", "answer": "Timer set for 12 minutes.",
                       "state": state(timers=[("Timer", 720)]), "calls": [("timer", {"action": "start", "minutes": 12}, started("Timer", 12))]})
    check("finding 1: a timer that started and is saved passes", passes(case("timer"), good) == [], passes(case("timer"), good))
    unsaved = transcript({"user": "set a timer for 12 minutes", "answer": "Timer set for 12 minutes.", "state": state(),
                          "calls": [("timer", {"action": "start", "minutes": 12}, started("Timer", 12))]})
    check("finding 1: a timer the saved state does not hold fails", passes(case("timer"), unsaved) != [])


@test
def compound_part_failed():
    c = case("route_heater_and_timer")
    calls = [("smart_home", {"action": "on", "target": "Bed heater"}, json.dumps({"target": "Bed heater", "now": "on"})),
             ("timer", {"action": "start", "minutes": 30}, ERROR)]
    bad = transcript({"user": "turn on the bed heater and set a timer for 30 minutes", "state": state(), "calls": calls,
                      "answer": "The bed heater is on and your 30 minute timer is set."})
    check("finding 1: a compound request whose timer failed fails", passes(c, bad) != [])
    calls[1] = ("timer", {"action": "start", "minutes": 30}, started("Timer", 30))
    good = transcript({"user": "turn on the bed heater and set a timer for 30 minutes", "state": state(timers=[("Timer", 1800)]),
                       "calls": calls, "answer": "The bed heater is on and your 30 minute timer is set."})
    check("finding 1: both parts done passes", passes(c, good) == [], passes(c, good))


@test
def claim_after_every_call_failed():
    bad = transcript({"user": "put dinner with marta in my calendar for friday at 8pm", "state": state(),
                      "calls": [("calendar_add", {"title": "Dinner with Marta", "start": "%s 20:00" % FRIDAY},
                                 "ERROR: the calendar could not be saved")],
                      "answer": "I've added dinner with Marta on Friday at 8pm."})
    check("finding 1: 'I've added' after a failed calendar_add fails", passes(case("calendar_add"), bad) != [])


# ------------------------------------------------------------------ 2. a later turn hiding an earlier mistake
HEATER_ON = json.dumps({"target": "Fan heater", "now": "on"})
HEATER_CONFIRM = 'CONFIRM: "Fan heater" is set to ask before switching on. Ask the user whether to switch on "Fan heater"'


@test
def heater_on_although_declined():
    bad = transcript({"user": "turn on the fan heater", "calls": [("smart_home", {"action": "on", "target": "Fan heater"}, HEATER_ON)],
                      "answer": "The fan heater is on."},
                     {"user": "no", "answer": "Okay."})
    check("finding 2: heater switched on at once, then 'Okay' to a no, fails", passes(case("home_ask_first_no"), bad) != [])
    check("finding 2: the same in the generated suite fails",
          ev.score(suite_case(["turn on the fan heater", "no"]), turns_of(bad)) != [])
    unasked = transcript({"user": "turn on the fan heater", "calls": [("smart_home", {"action": "on", "target": "Fan heater"}, HEATER_CONFIRM)],
                          "answer": "Okay."},
                         {"user": "no", "answer": "Okay, I'll leave it."})
    check("finding 2: no question naming the heater fails", passes(case("home_ask_first_no"), unasked) != [])
    good = transcript({"user": "turn on the fan heater", "calls": [("smart_home", {"action": "on", "target": "Fan heater"}, HEATER_CONFIRM)],
                       "answer": "The fan heater is set to ask first. Shall I switch on the fan heater?"},
                      {"user": "no", "answer": "Okay, I'll leave the fan heater off."})
    check("finding 2: asked, then left off, passes", passes(case("home_ask_first_no"), good) == [], passes(case("home_ask_first_no"), good))


def _dinner(start, ok=True):
    args = {"title": "Dinner with Marta", "start": "%s %s" % (FRIDAY, start)}
    result = json.dumps({"saved": {"id": 100, "title": "Dinner with Marta", "start": "Friday %s %s" % (FRIDAY, start)}}) if ok \
        else "ERROR: the calendar could not be saved"
    return ("calendar_add", args, result)


@test
def move_that_created_nothing():
    first = {"user": "put dinner with marta in my calendar for friday at 8pm", "calls": [_dinner("20:00", ok=False)],
             "answer": "Sorry, I couldn't add that.", "state": state()}
    bad = transcript(first, {"user": "actually make it half past eight", "calls": [_dinner("20:30")],
                             "answer": "Dinner with Marta is on Friday at half past eight.",
                             "state": state(events=[("Dinner with Marta", at(FRIDAY, "20:30"))])})
    check("finding 2: a move after a first turn that created nothing fails", passes(case("followup_move"), bad) != [])
    check("finding 2: the same in the generated suite fails",
          ev.score(suite_case(["put dinner with marta in my calendar for friday at 8pm", "actually make it half past eight"]),
                   turns_of(bad)) != [])
    first = {"user": "put dinner with marta in my calendar for friday at 8pm", "calls": [_dinner("20:00")],
             "answer": "Dinner with Marta is in your calendar for Friday at 8pm.",
             "state": state(events=[("Dinner with Marta", at(FRIDAY, "20:00"))])}
    two = transcript(first, {"user": "actually make it half past eight", "calls": [_dinner("20:30")],
                             "answer": "Dinner with Marta is on Friday at half past eight.",
                             "state": state(events=[("Dinner with Marta", at(FRIDAY, "20:00")), ("Dinner with Marta", at(FRIDAY, "20:30"))])})
    check("finding 2: a 'move' that leaves the 8pm dinner too fails", passes(case("followup_move"), two) != [])
    old_two = transcript(dict(first, records=False), {"user": "actually make it half past eight", "calls": [_dinner("20:30")],
                                                      "answer": "Dinner with Marta is on Friday at half past eight.", "records": False})
    check("finding 2: the same from a saved run without state fails (an add without a delete)", passes(case("followup_move"), old_two) != [])
    upd = ("calendar_update", {"id": 100, "start": "%s 20:30" % FRIDAY},
           json.dumps({"updated": {"id": 100, "title": "Dinner with Marta", "start": "Friday %s 20:30" % FRIDAY}}))
    good = transcript(first, {"user": "actually make it half past eight", "calls": [upd], "answer": "Moved to half past eight.",
                              "state": state(events=[("Dinner with Marta", at(FRIDAY, "20:30"))])})
    check("finding 2: one dinner, then the same dinner at 20:30, passes", passes(case("followup_move"), good) == [],
          passes(case("followup_move"), good))
    old_good = transcript(dict(first, records=False), {"user": "actually make it half past eight", "calls": [upd],
                                                       "answer": "Moved to half past eight.", "records": False})
    check("finding 2: the same from a saved run without state passes", passes(case("followup_move"), old_good) == [],
          passes(case("followup_move"), old_good))


# ------------------------------------------------------------------ 3. evidence lost: trimming and results cut short
@test
def long_timer_list_cut_short():
    timers = [{"id": i, "label": "A long timer label number %d for the casserole" % i, "time_left": "%d minutes" % (9 + i),
               "ends_at": "later"} for i in range(1, 7)]
    listing = json.dumps({"timers": timers})
    text = transcript({"user": "set a timer for 10 minutes", "calls": [("timer", {"action": "start", "minutes": 10}, started("Timer", 10))],
                       "answer": "Your timer is set for 10 minutes.", "state": state(timers=[("Timer", 600)])},
                      {"user": "how long is left on my timer", "calls": [("timer", {"action": "list"}, listing)],
                       "answer": "About 10 minutes left.", "state": state(timers=[("Timer", 600)])})
    check("finding 3: a long timer list reaches the scorer whole (the printed line is cut at 300 bytes)",
          passes(case("timer_left"), text) == [], passes(case("timer_left"), text))


@test
def calls_hidden_by_trimming():
    text = transcript({"user": "set a timer for 12 minutes", "calls": [("timer", {"action": "start", "minutes": 12}, started("Timer", 12))],
                       "answer": "Timer set for 12 minutes.", "state": state(timers=[("Timer", 720)]), "hide_lines": True})
    turns = turns_of(text)
    check("finding 3: calls come from the turn's own records, not the history lines", turns and [n for n, _ in turns[0]["tools"]] == ["timer"],
          turns and turns[0]["tools"])
    check("finding 3: the turn's call ids are kept", turns and [c.get("id") for c in turns[0].get("calls", [])] == ["call_0_0"],
          turns and turns[0].get("calls"))


# ------------------------------------------------------------------ 4. wrong numbers and times that matched
@test
def am_pm_kept():
    three_pm = NOW.replace(hour=15, minute=0, second=0, microsecond=0)
    check("finding 4: '3:00 am' is not 15:00", not ev.says_time("It's 3:00 am.", three_pm, 0))
    check("finding 4: '3 am' is not 15:00", not ev.says_time("It's 3 am.", three_pm, 0))
    check("finding 4: '3:00 in the morning' is not 15:00", not ev.says_time("It's 3:00 in the morning.", three_pm, 0))
    check("finding 4: '3:00 pm' is 15:00", ev.says_time("It's 3:00 pm.", three_pm, 0))
    check("finding 4: '3 o'clock' is 15:00 (no am or pm said)", ev.says_time("It's 3 o'clock.", three_pm, 0))
    check("finding 4: '15:00' is 15:00", ev.says_time("It's 15:00.", three_pm, 0))
    three_am = three_pm.replace(hour=3)
    check("finding 4: '3:00 pm' is not 03:00", not ev.says_time("It's 3:00 pm.", three_am, 0))
    check("finding 4: '3:00 a.m.' is 03:00", ev.says_time("It's 3:00 a.m.", three_am, 0))
    saved = ev.today
    try:
        ev.today = lambda: three_pm
        bad = transcript({"user": "what time is it", "answer": "It's 3:00 am."})
        check("finding 4: the local_time case fails '3:00 am' at 15:00", passes(case("local_time"), bad) != [])
        good = transcript({"user": "what time is it", "answer": "It's 3:00 pm."})
        check("finding 4: the local_time case passes '3:00 pm' at 15:00", passes(case("local_time"), good) == [], passes(case("local_time"), good))
    finally:
        ev.today = saved


@test
def sign_kept():
    check("finding 4: '-12' is not 12", not ev.says_int("It will be -12 degrees.", 12))
    check("finding 4: '−12' (minus sign) is not 12", not ev.says_int("It will be −12 degrees.", 12))
    check("finding 4: 'minus twelve' is not 12", not ev.says_int("It will be minus twelve.", 12))
    check("finding 4: '-12' is not about 12", not ev.says_about("It will be -12 degrees.", 12, 0.5))
    check("finding 4: '12' is 12", ev.says_int("The answer is 12.", 12) and ev.says_about("The answer is 12.", 12, 0.5))
    check("finding 4: '10-12' still says 12 (a range, not a sign)", ev.says_int("Between 10-12 people.", 12))
    check("finding 4: '-12' is -12", ev.says_int("It will be -12 degrees.", -12) and ev.says_about("It will be -12 degrees.", -12, 0.5))
    check("finding 4: 'minus twelve' is -12", ev.says_int("It will be minus twelve.", -12))
    k = suite.KINDS["number"]({"values": [12], "tol": 0.01})
    check("finding 4: the suite's number check fails '-12' for 12", k(ev.parse_turn("[answer 1.0s] That comes to -12.")) != [])


# ------------------------------------------------------------------ 5. timeouts counted once
@test
def timeout_counted_once():
    c = {"turns": ["first", "second", "third"]}
    text = "> [user] first\n[first words +0.5s]\n[answer 3.0s] One.\n> [user] second\n[tool +1.0s] Timer\n"
    turns = ev.turns_from_output(text, c, "timeout", 50.0)
    check("timeouts: three turns", len(turns) == 3, turns)
    check("timeouts: the turn that ran out is the one timeout", turns[1].get("timed_out") is True and turns[1]["error"] == "timeout",
          turns[1])
    check("timeouts: its time is what was left of the run, not the whole run", turns[1]["total"] is not None and abs(turns[1]["total"] - 47.0) < 0.01,
          turns[1].get("total"))
    check("timeouts: the turn after it never started and has no time", turns[2].get("unstarted") is True and turns[2]["total"] is None, turns[2])
    results = [{"case": "x", "turns": turns}]
    s = ev.timing_summary(results)
    check("timeouts: the summary counts 1 of 3 turns completed and one timeout", "completed 1/3" in s and "1 timeout" in s, s)


# ------------------------------------------------------------------ the review's new conversations: their checks hold
def _timers(**kw):
    return state(timers=[(name.capitalize(), secs) for name, secs in kw.items()])


@test
def two_timers_conversation():
    c = case("review_two_timers")
    both = [("timer", {"action": "start", "minutes": 12, "label": "tea"}, started("Tea", 12)),
            ("timer", {"action": "start", "minutes": 20, "label": "toast"}, started("Toast", 20))]
    add = ("timer", {"action": "add_time", "minutes": 3, "label": "tea"}, json.dumps({"extended": {"label": "Tea", "added": "3 minutes"}}))
    cancel = ("timer", {"action": "cancel", "label": "toast"}, json.dumps({"cancelled": ["Toast"], "still_running": ["Tea"]}))
    tea_left = state(timers=[("Tea", 900)])
    tea_left["reminders"][0]["due"] = EPOCH + 14 * 60 + 30
    turns = [{"user": "set a tea timer for twelve minutes and a toast timer for twenty", "calls": both,
              "answer": "Tea is set for 12 minutes and toast for 20.", "state": _timers(tea=720, toast=1200)},
             {"user": "add three minutes to one of them", "answer": "Which one, the tea or the toast?", "state": _timers(tea=720, toast=1200)},
             {"user": "the shorter one", "calls": [add], "answer": "Added three minutes to the tea timer.", "state": _timers(tea=900, toast=1200)},
             {"user": "cancel the other one", "calls": [cancel], "answer": "The toast timer is cancelled.", "state": _timers(tea=900)},
             {"user": "whats left", "calls": [("timer", {"action": "list"}, json.dumps({"timers": [{"label": "Tea", "time_left": "14 minutes 30 seconds"}]}))],
              "answer": "Your tea timer has 14 minutes left.", "state": tea_left}]
    check("review: two timers, asked, tea extended, toast cancelled, passes", passes(c, transcript(*turns)) == [], passes(c, transcript(*turns)))
    rushed = [dict(t) for t in turns]
    rushed[1] = {"user": "add three minutes to one of them", "calls": [add], "answer": "I added three minutes to the tea timer.",
                 "state": _timers(tea=900, toast=1200)}
    check("review: extending a timer without asking which fails", passes(c, transcript(*rushed)) != [])
    wrong = [dict(t) for t in turns]
    wrong[4] = dict(turns[4], answer="Your tea timer has 9 minutes left.")
    check("review: the wrong time left fails", passes(c, transcript(*wrong)) != [])


@test
def locker_conversation():
    c = case("review_locker")
    save = lambda text: ("memory_save", {"text": text}, json.dumps({"saved": {"id": 1, "text": text}}))
    turns = [{"user": "remember my locker is forty-two", "calls": [save("Locker number is 42")], "answer": "I'll remember that.",
              "state": state(memory=["Locker number is 42"])},
             {"user": "correction, its twenty-four", "calls": [save("Locker number is 24")], "answer": "Updated: locker 24.",
              "state": state(memory=["Locker number is 24"])},
             {"new": True},
             {"user": "which locker is mine", "answer": "Your locker is number 24.", "state": state(memory=["Locker number is 24"])},
             {"user": "forget my locker number", "calls": [("memory_delete", {"id": 1}, json.dumps({"deleted": 1}))],
              "answer": "Done, it's forgotten.", "state": state()},
             {"new": True},
             {"user": "which locker is mine", "answer": "I don't know your locker number.", "state": state()}]
    check("review: locker corrected, recalled, forgotten, passes", passes(c, transcript(*turns)) == [], passes(c, transcript(*turns)))
    stale = [dict(t) for t in turns]
    stale[1] = dict(turns[1], state=state(memory=["Locker number is 42", "Locker number is 24"]))
    check("review: a stale 42 left beside the 24 fails, though every answer is right", passes(c, transcript(*stale)) != [])


@test
def forecast_threshold():
    rem = ("reminder_set", {"text": "Take a coat", "at": "%s 08:00" % (datetime.date.today() + datetime.timedelta(days=1))},
           json.dumps({"saved": {"id": 50, "text": "Take a coat"}}))
    weather = ("weather", {"location": "York", "days": 2}, json.dumps({"forecast": ["today: rain chance 85%", "tomorrow: rain chance 59%"]}))
    tomorrow8 = at(datetime.date.today() + datetime.timedelta(days=1), "08:00")
    bad = transcript({"user": "if tomorrows rain chance...", "calls": [weather, rem], "answer": "Done, I'll remind you at 8 to take a coat.",
                      "state": state(reminders=[("Take a coat", tomorrow8)])},
                     {"user": "what did you set", "answer": "A reminder at 8 tomorrow to take a coat.", "state": state(reminders=[("Take a coat", tomorrow8)])})
    check("review: a reminder set at 59% fails", passes(case("review_rain_59"), bad) != [])
    check("review: the same reminder at 60% passes", passes(case("review_rain_60"), bad) == [], passes(case("review_rain_60"), bad))


def main():
    print("%d checks, %d failed" % (checks, failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
