#!/usr/bin/env python3
"""Tests the scorer of the agent test sets (tests/agent_eval.py, tests/agent_suite.py) offline: no
service, no app, standard library only. Deliberately wrong runs must fail (or, where the evidence is
missing, be unscorable) and right ones must pass, so a change to the scorer is known to reject bad
behaviour before a live run is spent on it.

A run is written as the command-line agent writes it: the records file (ZB_EVAL_RECORDS: one JSON
object a line, on a channel of its own) and, for older runs, the printed transcript. Both go through
the same parsers as a live run. Most checks have a wrong run of their own that trips that check and no
other, with the problem it must report: a check that stops working shows here, not in a live run
(tests/run.py's mutation review, `run_mutations.py`, breaks each check in turn to see that one of these
notices). The wrong runs come from the evaluation reviews of 19 November 2026: a failed action
reported as done, a later turn hiding an earlier mistake, a long result cut short, a wrong number or
time that still matched, a timeout counted once per missing turn, an answer quoting the evaluator's
own records, missing evidence taken for a pass.

usage: tests/scorer_test.py [-v]
"""
import contextlib
import datetime
import io
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True   # importing the runners leaves no __pycache__ in tests/
import agent_eval as ev   # noqa: E402
import netguard   # noqa: E402
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
        import traceback
        check(f.__name__, False, "raised %r\n%s" % (e, traceback.format_exc()))
    return f


# ------------------------------------------------------------------ runs, as the cli writes them
# A fixed "now" (a Thursday), so the self-test gives the same answer whenever it runs: its transcripts name
# weekdays and dates ("Friday" is 20 November). Scoring uses ev.clock(); a run's own time (SCORING_AT)
# still wins, as in real scoring.
NOW = datetime.datetime(2026, 11, 19, 12, 0, 0)
ev.clock = lambda: ev.SCORING_AT if ev.SCORING_AT is not None else NOW.timestamp()
EPOCH = int(NOW.timestamp())


def state(timers=(), reminders=(), events=(), memory=(), now=None):
    """A saved state: timers as (label, seconds) or (label, seconds, due), reminders as (text, datetime),
    events as (title, datetime) or (title, datetime, repeat, all_day), memory as texts."""
    rem = []
    for i, x in enumerate(timers):
        label, secs = x[0], x[1]
        rem.append({"id": i + 1, "text": label, "due": x[2] if len(x) > 2 else EPOCH + secs, "fired": False, "kind": "timer",
                    "started": EPOCH, "length": secs})
    rem += [{"id": 50 + i, "text": text, "due": int(when.timestamp()), "fired": False} for i, (text, when) in enumerate(reminders)]
    cal = [{"id": 100 + i, "title": e[0], "start": int(e[1].timestamp()), "end": 0, "notes": "", "remind_min": -1,
            "repeat": e[2] if len(e) > 2 else "", "all_day": e[3] if len(e) > 3 else False, "alerted_for": 0, "created": EPOCH}
           for i, e in enumerate(events)]
    mem = [{"id": 200 + i, "text": text, "created": EPOCH} for i, text in enumerate(memory)]
    return {"now": now or EPOCH, "reminders": rem, "calendar": cal, "memory": mem}


WALL = 1795100000.0   # the clock of a made-up run: turn n runs from WALL + 10 n to WALL + 10 n + 5


def home_things(home, on=()):
    """A home's things as the state record lists them, those named in `on` switched on."""
    return [{"device": d["id"], "name": t["name"], "room": t.get("room", ""),
             "state": dict(t.get("state") or {}, **({"on": True} if t["name"] in on else {}))}
            for n in home["networks"].values() for d in n["devices"] for t in d["things"]]


def records(c, *turns, end=True):
    """The records file of a run of case c. turns: dicts with calls [(name, args, result)], answer, state (a
    dict; None for no state record; an empty state when left out), total, first, error, user (to say
    something other than the case's request), on (the smart-home things switched on in that turn, for a
    case with a home: they stay on), responses (how the calls were grouped into model responses, as
    counts; one response with all of them when left out); {"new": True} is "/new". A turn without "answer"
    never ended."""
    asked = [t for t in c["turns"] if t != "/new"]
    out, n, on = [{"ev": "start", "caps": ["records", "web", "web_all", "no_hardware"]}], 0, set()
    home = c.get("home") if isinstance(c.get("home"), dict) else None
    for t in turns:
        if "answer" not in t and not t.get("new") and n < len(asked): end = False   # a turn that never ended: no end of the run
        if t.get("new"):
            out.append({"ev": "new"})
            continue
        n += 1
        out.append({"ev": "turn_start", "turn": n, "text": t.get("user", asked[n - 1] if n <= len(asked) else "more"), "wall": WALL + 10 * n})
        calls = [{"id": "call_%d_%d" % (n, j), "name": name, "args": json.dumps(args)} for j, (name, args, _) in enumerate(t.get("calls", []))]
        for j, (name, args, result) in enumerate(t.get("calls", [])):
            out.append(dict(calls[j], ev="call", turn=n, n=j + 1, result=result))
        if t.get("error"): out.append({"ev": "error", "turn": n, "message": t["error"]})
        if "answer" not in t: break
        st = t.get("state", state())
        on |= set(t.get("on", ()))
        if st is not None and home and "home" not in st: st = dict(st, home=home_things(home, on))
        if st is not None: out.append({"ev": "state", "turn": n, "state": st})
        sizes, responses, k = t.get("responses", [len(calls)] if calls else []), [], 0
        for size in sizes:
            responses.append(calls[k:k + size])
            k += size
        out.append({"ev": "turn_end", "turn": n, "total": t.get("total", 2.0), "first": t.get("first", 0.5), "answer": t["answer"],
                    "tool_calls": len(calls), "responses": responses, "wall": WALL + 10 * n + 5})
    if end: out.append({"ev": "end", "turns": n})
    return "".join(json.dumps(dict(r, v=2, mono=0.0, **({} if "wall" in r else {"wall": WALL}))) + "\n" for r in out)


def run_of(c, *turns, exit=0, timeout=False, seconds=None, text=None, end=True):
    """What run_case returns for such a run."""
    process = {"exit": None if timeout else exit, "timeout": timeout, "seconds": seconds if seconds is not None else 2.0 * len(turns)}
    got, reasons = ev.turns_from_records(text if text is not None else records(c, *turns, end=end), c, process)
    return {"turns": got, "process": process, "evidence": "records", "unscorable": reasons}


def verdict(c, *turns, **kw):
    return ev.verdict(c, run_of(c, *turns, **kw))


def problems(c, *turns, **kw):
    return verdict(c, *turns, **kw)[1]


def passes(name, c, *turns, **kw):
    got = verdict(c, *turns, **kw)
    return check(name, got == ("pass", []), got)


def trips(name, c, turns, expected, outcome="fail", **kw):
    """The run gets this outcome, and every problem contains `expected`: it trips the one check it was
    written for (which may say more than one thing), and no other."""
    got = verdict(c, *turns, **kw)
    return check(name, got[0] == outcome and got[1] and all(expected in p for p in got[1]), got)


def fails_with(name, c, turns, expected, **kw):
    """The run fails, and one of its problems contains `expected` (where several checks rightly fire)."""
    got = verdict(c, *turns, **kw)
    return check(name, got[0] == "fail" and any(expected in p for p in got[1]), got)


def trips_turn(name, check_fn, turn, expected):
    """One check called on one parsed turn: exactly one problem, containing `expected`."""
    got = check_fn(turn)
    return check(name, len(got) == 1 and expected in got[0], got)


def one_turn(**t):
    """One parsed turn (as turns_from_records gives it) with these calls, answer and state."""
    c = {"turns": ["x"]}
    return ev.turns_from_records(records(c, dict({"answer": "Okay."}, **t)), c, {"exit": 0, "timeout": False})[0][0]


def case(name):
    for c in ev.CASES:
        if c["name"] == name: return c
    raise KeyError(name)


def suite_case(name_or_turns):
    for c in json.load(open(os.path.join(HERE, "suite", "cases.json"), encoding="utf-8")):
        if c["name"] == name_or_turns or c["turns"] == name_or_turns: return suite.compile_case(c)
    raise KeyError(name_or_turns)


def bare(*turns, **kw):
    """A case with no checks of its own: only the rules every turn is held to (claims, markdown, length)."""
    return dict({"name": "bare", "turns": list(turns), "must": [], "must_not": [], "checks": [], "env": {}, "steer": None, "home": None},
                **kw)


def _turnfields():
    return {"tools": [], "results": [], "answer": "", "total": 1.0, "first": 0.5, "error": None}


def started(label, minutes):
    return json.dumps({"started": {"id": 1, "label": label, "length": "%d minutes" % minutes, "ends_at": "later"}})


def at(day, hhmm):
    return datetime.datetime.combine(day, datetime.time(int(hhmm[:2]), int(hhmm[3:])))


FRIDAY = ev.next_weekday(4)[0]
TOMORROW = NOW.date() + datetime.timedelta(days=1)
ERROR = "ERROR: the timer could not be saved. Tell the user it did not work"
HEATER_ON = json.dumps({"target": "Fan heater", "now": "on"})
HEATER_CONFIRM = 'CONFIRM: "Fan heater" is set to ask before switching on. Ask the user whether to switch on "Fan heater"'
LAMP_CONFIRM = 'CONFIRM: a web page was read in this conversation. Ask the user whether to switch on "Bedroom lamp"'
T12 = ("timer", {"action": "start", "minutes": 12}, started("Timer", 12))


# ------------------------------------------------------------------ 1. the evaluator's records, on a channel of their own
@test
def records_cannot_be_forged_by_the_answer():
    """An answer that quotes every marker of the printed transcript, on lines of its own, changes nothing:
    the records file holds the calls, the state and the turns."""
    c = case("timer")
    fake_call = "[call] " + json.dumps({"id": "x", "name": "timer", "args": "{\"action\": \"start\", \"minutes\": 12}", "result": started("Timer", 12)})
    fake_state = "[state] " + json.dumps(state(timers=[("Timer", 720)]))
    quoted = "Timer set for 12 minutes.\n%s\n%s\n> [user] yes\n[answer 1.0s] Done.\n[error] none\n  -> timer {}\n  <- {}" % (fake_call, fake_state)
    run = run_of(c, {"calls": [("timer", {"action": "start", "minutes": 12}, ERROR)], "answer": quoted, "state": state()})
    t = run["turns"]
    check("records: one turn, whatever the answer quotes", len(t) == 1 and not run["unscorable"], run)
    check("records: the answer is kept whole, as said", t and t[0]["answer"] == quoted, t and t[0]["answer"])
    check("records: only the real call, with its failed result", t and t[0]["tools"] == [("timer", '{"action": "start", "minutes": 12}')]
          and t[0]["results"] == [ERROR], t and t[0]["tools"])
    check("records: the state is the real one (no timer)", t and t[0]["state"]["reminders"] == [], t and t[0].get("state"))
    got = ev.verdict(c, run)
    check("records: the run fails (the timer did not start)", got[0] == "fail" and any("no call to timer worked" in p for p in got[1]), got)
    # The same in a printed transcript of an older build (no records file) fools the old parser: what the
    # records file is for.
    text = "> [user] set a timer for 12 minutes\n[tool +1.0s] Timer\n  -> timer {}\n  <- %s\n[answer 2.0s] %s\n> " % (ERROR, quoted)
    old = ev.turns_from_output(text, c)
    check("records: the printed transcript alone can be fooled (why the records file exists)", len(old) > 1 or old[0].get("state"), old)


@test
def records_validated():
    c = case("timer_left")
    good = [{"calls": [T12], "answer": "Timer set.", "state": state(timers=[("Timer", 720)])}, {"answer": "About 12 minutes left."}]
    base = records(c, *good)
    lines = base.splitlines(True)

    def unscorable(name, text, want, **kw):
        got = run_of(c, text=text, **kw)
        out = ev.verdict(c, got)
        check("records: " + name, out[0] == "unscorable" and any(want in p for p in out[1]), (out, got["unscorable"]))

    unscorable("a turn more than the case has", records(c, *(good + [{"answer": "And another."}])), "is not the case's next request")
    unscorable("a turn more than the case has, with no words", base.replace('{"ev": "end", "turns": 2',
               '{"ev": "turn_start", "turn": 3, "text": null, "v": 2, "mono": 0.0, "wall": 0}\n{"ev": "end", "turns": 3'), "is not the case's next request")
    unscorable("a turn whose words are not the case's", records(c, good[0], dict(good[1], user="something else")), "is not the case's next request")
    unscorable("a call record without its result", base.replace(', "result": %s' % json.dumps(T12[2]), ""), "a call record without")
    unscorable("a call record numbered out of order", base.replace('"n": 1', '"n": 2'), "a call record without")
    unscorable("a turn with fewer call records than calls", base.replace('"tool_calls": 1', '"tool_calls": 2'), "made 2 tool calls but has 1")
    unscorable("a record from another turn", base.replace('"ev": "state", "turn": 1', '"ev": "state", "turn": 2'), "outside its turn")
    unscorable("a second state record in a turn", "".join(lines[:4] + [lines[3]] + lines[4:]), "not one record")
    unscorable("an unknown version", base.replace('"v": 2', '"v": 7', 1), "not a record of version")
    unscorable("a line that is not JSON", base + "garbage\n", "not JSON")
    unscorable("records that stop early although the app exited normally", records(c, *good, end=False), "exited normally")
    unscorable("a last line cut short although the app was not stopped", base + '{"v": 2, "ev": "tu', "middle of a line")
    unscorable("a new conversation the case does not have", records(c, good[0], {"new": True}, good[1]), "a new conversation where")
    # A run stopped by the time limit may end in the middle of a line: that is where it stopped.
    got = run_of(c, text=records(c, good[0], {}) + '{"v": 2, "ev": "ca', timeout=True, seconds=50.0)
    check("records: a timed-out run's cut last line is where it stopped", not got["unscorable"] and got["turns"][1].get("timed_out")
          and abs(got["turns"][1]["total"] - 48.0) < 0.01, got)
    check("records: a good run reads cleanly", run_of(c, *good)["unscorable"] == [], run_of(c, *good)["unscorable"])
    # "/new" where the case has it.
    c2 = case("memory_recall")
    two = run_of(c2, {"calls": [("memory_save", {"text": "allergic to shellfish"}, '{"saved": 1}')], "answer": "Noted.",
                      "state": state(memory=["allergic to shellfish"])}, {"new": True}, {"answer": "No, it has shellfish."})
    check("records: a new conversation where the case has one", two["unscorable"] == [] and len(two["turns"]) == 2, two["unscorable"])


@test
def legacy_transcripts_still_parse():
    """Printed transcripts of older builds, with "[call]"/"[state]" lines or only "->"/"<-" lines."""
    text = ("> [user] set a timer for 12 minutes\n[tool +1.0s] Timer\n[call] %s\n[first words +1.5s]\n[state] %s\n[answer 2.0s] Timer set.\n> "
            % (json.dumps({"id": "call_0", "name": "timer", "args": '{"minutes": 12}', "result": T12[2]}), json.dumps(state(timers=[("Timer", 720)]))))
    t = ev.turns_from_output(text, case("timer"))
    check("legacy: calls and state from the printed records", t[0]["tools"] == [("timer", '{"minutes": 12}')] and t[0]["state"]["reminders"]
          and t[0]["calls"][0]["id"] == "call_0", t)
    text = "> [user] set a timer for 12 minutes\n  -> timer {\"minutes\": 12}\n  <- %s\n[answer 2.0s] Timer set.\n> " % T12[2]
    t = ev.turns_from_output(text, case("timer"))
    check("legacy: calls from the history lines", t[0]["tools"] == [("timer", '{"minutes": 12}')] and t[0]["results"] == [T12[2]], t)


# ------------------------------------------------------------------ 2. missing or unreadable evidence is unscorable, never a pass
@test
def missing_evidence_unscorable():
    c = case("timer")
    run = [{"calls": [T12], "answer": "Timer set for 12 minutes.", "state": None}]
    trips("evidence: a turn whose records have no state is unscorable", c, run, "turn 1 ended without its saved state", outcome="unscorable")
    legacy = {"turns": [dict(_turnfields(), tools=[("timer", json.dumps(T12[1]))], results=[T12[2]], answer="Timer set for 12 minutes.")]}
    got = ev.verdict(c, legacy)
    check("evidence: a state check on a saved turn without a state is unscorable", got[0] == "unscorable" and
          got[1] == ["unscorable: no saved state was recorded for this turn"], got)
    yoga = suite_case("calendar-042")
    add = ("calendar_add", {"title": "Yoga", "start": "%s 18:00" % ev.next_weekday(2)[0]}, '{"saved": {"id": 100}}')
    gone = ("calendar_delete", {"id": 100}, '{"deleted": 100}')
    first = {"calls": [add], "answer": "Yoga is in your calendar.", "state": state(events=[("Yoga", at(ev.next_weekday(2)[0], "18:00"))])}
    unreadable = dict(state(), calendar=None, unreadable=["calendar"])
    trips("evidence: an unreadable calendar is not an empty one (no 'zero events' pass)", yoga,
          [first, {"calls": [gone], "answer": "Deleted.", "state": unreadable}], "could not read its calendar file", outcome="unscorable")
    malformed = dict(state(), calendar=[1, 2])
    trips("evidence: a malformed calendar is unscorable", yoga, [first, {"calls": [gone], "answer": "Deleted.", "state": malformed}],
          "not a list of records", outcome="unscorable")
    passes("evidence: the same with the calendar empty passes", yoga, first, {"calls": [gone], "answer": "Deleted.", "state": state()})
    # A saved result from before a field existed.
    got = ev.verdict(c, {"turns": [{"tools": [], "answer": "Timer set.", "total": 1.0, "first": 0.5, "error": None}]})
    check("evidence: a saved turn without its results is unscorable", got[0] == "unscorable" and "has no results" in got[1][0], got)
    got = ev.verdict(c, {"turns": [{"tools": [("timer", json.dumps(T12[1]))], "results": [T12[2]], "answer": "Timer set for 12 minutes.",
                                    "total": 1.0, "first": 0.5, "error": None}]})
    check("evidence: an old result without a saved state is unscorable, not passed", got[0] == "unscorable", got)
    # A definite failure is not hidden by missing evidence elsewhere.
    got = ev.verdict(c, {"turns": [dict(legacy["turns"][0], answer="Timer set for twenty minutes.")]})
    check("evidence: a wrong answer still fails when a state is missing", got[0] == "fail" and len(got[1]) == 2, got)
    check("evidence: the summary counts unscorable apart", ev.outcome_line([{"outcome": "pass"}, {"outcome": "fail"}, {"outcome": "unscorable"}])
          == "1/3 passed (33%), 1 failed, 1 unscorable", ev.outcome_line([{"outcome": "pass"}, {"outcome": "fail"}, {"outcome": "unscorable"}]))
    try:
        ev.state({"state": None})
        check("evidence: state() of a turn without one raises Unscorable", False)
    except ev.Unscorable:
        check("evidence: state() of a turn without one raises Unscorable", True)


# ------------------------------------------------------------------ timeouts and the app's exit
@test
def timeout_counted_once():
    c = {"turns": ["first", "second", "third"]}
    text = "> [user] first\n[first words +0.5s]\n[answer 3.0s] One.\n> [user] second\n[tool +1.0s] Timer\n"
    turns = ev.turns_from_output(text, c, "timeout", 50.0)
    check("timeouts: three turns", len(turns) == 3, turns)
    check("timeouts: the turn that ran out is the one timeout", turns[1].get("timed_out") is True and turns[1]["error"] == "timeout", turns[1])
    check("timeouts: its time is what was left of the run, not the whole run", turns[1]["total"] is not None and abs(turns[1]["total"] - 47.0) < 0.01,
          turns[1].get("total"))
    check("timeouts: the turn after it never started and has no time", turns[2].get("unstarted") is True and turns[2]["total"] is None, turns[2])
    s = ev.timing_summary([{"case": "x", "turns": turns}])
    check("timeouts: the summary counts 1 of 3 turns completed and one timeout", "completed 1/3" in s and "1 timeout," in s, s)
    c = bare("first", "second", "third")
    got = ev.verdict(c, run_of(c, {"answer": "One.", "total": 3.0}, {}, timeout=True, seconds=50.0))
    check("timeouts: the unstarted turn is reported", got[0] == "fail" and any("turn 3: not started" in p for p in got[1]), got)
    # A run that answered every turn, then did not exit.
    run = run_of(c, {"answer": "One."}, {"answer": "Two."}, {"answer": "Three."}, timeout=True, seconds=270.0)
    got = ev.verdict(c, run)
    check("timeouts: a timeout after the last answer still counts", got == ("fail", ["the run timed out after its last answer: the app did not exit"]),
          got)
    s = ev.timing_summary([{"case": "x", "turns": run["turns"], "process": run["process"]}])
    check("timeouts: ... in the summary too, beside every latency figure", "completed 3/3 turns, 1 timeout," in s and
          "total (3 turns; completed 3/3 turns)" in s and "first words (3 turns; completed 3/3 turns)" in s, s)
    got = verdict(c, {"answer": "One."}, {"answer": "Two."}, {"answer": "Three."}, exit=-11)
    check("timeouts: an app that crashed after its last answer fails", got == ("fail", ["the app ended with status -11 after its last answer"]), got)


# ------------------------------------------------------------------ the generic rules: claims, must, tried
@test
def claims_and_must():
    trips("claim: 'I've set' with no tool call", bare("thanks"), [{"answer": "I've set a timer for you."}], "claims an action but called no tool")
    trips("claim: 'I've added' after a change that failed, beside a read that worked", bare("x"),
          [{"calls": [("calendar_list", {}, '{"events": []}'), ("breathe", {"minutes": 2}, "ERROR: could not start")],
            "answer": "I've set up a breathing exercise."}], "claims an action but no change worked")
    trips("claim: a change waiting for a yes is not done", bare("x"),
          [{"calls": [("smart_home", {"action": "on", "target": "Fan heater"}, HEATER_CONFIRM)], "answer": "I've turned on the fan heater."}],
          "claims an action but no change worked")
    trips("claim: a JSON error result is a failure", bare("x"),
          [{"calls": [("music", {"action": "play"}, '{"error": "nothing in the library matches"}')], "answer": "Now playing some jazz."}],
          "claims an action but no change worked")
    passes("claim: a read alone, then 'now playing in cinemas', is no claim of a change", bare("x"),
           {"calls": [("web_search", {"query": "films"}, '{"results": []}')], "answer": "Dune is now playing at the Odeon."})
    passes("claim: quoting a confirmation is allowed where the case says so", case("review_hypothetical_timer"),
           {"answer": "I'd say: timer set for 10 minutes."})
    trips("must: a change that failed is not done", case("stressed"),
          [{"calls": [("breathe", {"minutes": 2}, "ERROR: could not start")], "answer": "Sorry, that did not work."}], "no call to breathe worked")
    trips("tried: the call made counts, whatever came of it", bare("x", tried=["music"]), [{"answer": "Nothing is playing."}], "did not call music")
    passes("tried: a failed call is enough", bare("x", tried=["music"]),
           {"calls": [("music", {"action": "next"}, "ERROR: nothing is playing")], "answer": "Nothing is playing right now."})
    check("calls(): a change that failed is not counted", ev.calls(one_turn(calls=[("timer", {"minutes": 1}, ERROR)]), "timer") == [])
    check("calls(): a change waiting for a yes is not counted",
          ev.calls(one_turn(calls=[("smart_home", {"action": "on"}, HEATER_CONFIRM)]), "smart_home") == [])
    check("calls(): a read that failed counts (an outage is not the model's)",
          ev.calls(one_turn(calls=[("music", {"action": "search", "query": "x"}, "ERROR: nothing matches")]), "music") ==
          [{"action": "search", "query": "x"}])
    check("calls(): a web read that failed does not count: every case is on the stand-in (W6)",
          ev.calls(one_turn(calls=[("web_search", {"query": "x"}, "ERROR: over quota")]), "web_search") == [])
    t = one_turn(calls=[("web_search", {"query": "x"}, "ERROR: the stand-in has nothing at /x")])
    t["strict_reads"] = True
    check("calls(): on the stand-in web, a read that failed does not count", ev.calls(t, "web_search") == [])


@test
def compound_part_failed():
    c = case("route_heater_and_timer")
    calls = [("smart_home", {"action": "on", "target": "Bed heater"}, json.dumps({"target": "Bed heater", "now": "on"})),
             ("timer", {"action": "start", "minutes": 30}, ERROR)]
    bad = {"calls": calls, "answer": "The bed heater is on and your 30 minute timer is set.", "state": state()}
    check("compound: a request whose timer failed fails", verdict(c, bad)[0] == "fail", verdict(c, bad))
    good = {"calls": calls[:1] + [("timer", {"action": "start", "minutes": 30}, started("Timer", 30))], "state": state(timers=[("Timer", 1800)]),
            "answer": "The bed heater is on and your 30 minute timer is set."}
    passes("compound: both parts done passes", c, good)
    jazz = suite_case("compound-005")
    music_failed = [("music", {"action": "play", "genre": "jazz"}, "ERROR: the music library is empty"),
                    ("device", {"setting": "volume", "value": 30}, '{"volume": 30}')]
    fails_with("compound (A4): jazz that failed beside a volume that worked fails on the music", jazz,
               [{"calls": music_failed, "answer": "Your volume is at 30."}], "no call to music worked")
    passes("compound (A4): both working passes", jazz,
           {"calls": [("music", {"action": "play", "genre": "jazz"}, '{"now_playing": "So What by Miles Davis", "queued_tracks": 4}'),
                      music_failed[1]], "answer": "Jazz is on, and your volume is at 30."})


# ------------------------------------------------------------------ timers
@test
def timer_checks():
    c = case("timer")
    passes("timer: started and saved passes", c, {"calls": [T12], "answer": "Timer set for 12 minutes.", "state": state(timers=[("Timer", 720)])})
    trips("timer: the call worked but nothing is saved", c, [{"calls": [T12], "answer": "Timer set for 12 minutes."}], "no 720-second timer is saved")
    trips("timer: the wrong length", c, [{"calls": [("timer", {"action": "start", "minutes": 21}, started("Timer", 21))], "answer": "Timer set for 12 minutes.",
                                          "state": state(timers=[("Timer", 1260)])}], "timer should start for 720 seconds")
    c = case("review_self_correction")
    trips("timers_saved: a timer too many", c, [{"calls": [("timer", {"action": "start", "minutes": 50}, started("Timer", 50))],
                                                 "answer": "Timer set for 50 minutes.", "state": state(timers=[("Timer", 3000), ("Timer", 900)])}],
          "the saved timers should be [3000] seconds long")
    c = case("timer_cancel_named")
    run = [{"calls": [("timer", {"action": "start", "minutes": 45, "label": "study"}, started("Study", 45))], "answer": "Study timer set.",
            "state": state(timers=[("Study", 2700)])},
           {"calls": [("timer", {"action": "start", "minutes": 10, "label": "pasta"}, started("Pasta", 10))], "answer": "Pasta timer set.",
            "state": state(timers=[("Study", 2700), ("Pasta", 600)])},
           {"calls": [("timer", {"action": "cancel", "label": "study"}, json.dumps({"cancelled": ["Study"], "still_running": ["Pasta"]}))],
            "answer": "The study timer is cancelled.", "state": state(timers=[("Pasta", 600)])}]
    passes("cancelled: the study timer cancelled, pasta left, passes", c, *run)
    t = one_turn(calls=run[2]["calls"], state=state(timers=[("Study", 2700), ("Pasta", 600)]))
    trips_turn("cancelled: the cancel says so but the study timer is still saved", ev.chk_cancelled("stud", "pasta"), t,
               "the saved timers should hold the pasta timer")
    c = case("timer_left")
    timers = [{"id": i, "label": "A long timer label number %d for the casserole" % i, "time_left": "%d minutes" % (9 + i), "ends_at": "later"}
              for i in range(1, 7)]
    run = [{"calls": [("timer", {"action": "start", "minutes": 10}, started("Timer", 10))], "answer": "Your timer is set for 10 minutes.",
            "state": state(timers=[("Timer", 600)])},
           {"calls": [("timer", {"action": "list"}, json.dumps({"timers": timers}))], "answer": "About 10 minutes left.", "state": state(timers=[("Timer", 600)])}]
    passes("time left: a long timer list reaches the scorer whole (the printed line is cut at 300 bytes)", c, *run)
    c = case("reminder_relative")
    rem = ("reminder_set", {"text": "Take tablets", "in_minutes": 90}, '{"saved": {"id": 50}}')
    passes("countdown: a reminder 90 minutes away, saved, passes", c,
           {"calls": [rem], "answer": "I'll remind you in an hour and a half.", "state": state(reminders=[("Take tablets", NOW + datetime.timedelta(minutes=90))])})
    trips("countdown: the call worked but nothing is saved", c, [{"calls": [rem], "answer": "I'll remind you in an hour and a half."}],
          "nothing is saved to go off 5400 seconds from now")


@test
def two_timers_conversation():
    c = case("review_two_timers")
    both = [("timer", {"action": "start", "minutes": 12, "label": "tea"}, started("Tea", 12)),
            ("timer", {"action": "start", "minutes": 20, "label": "toast"}, started("Toast", 20))]
    add = ("timer", {"action": "add_time", "minutes": 3, "label": "tea"}, json.dumps({"extended": {"label": "Tea", "added": "3 minutes"}}))
    cancel = ("timer", {"action": "cancel", "label": "toast"}, json.dumps({"cancelled": ["Toast"], "still_running": ["Tea"]}))
    tea, toast = EPOCH + 720, EPOCH + 1200
    T = lambda tea_due=None, toast_due=None, now=None: state(timers=[x for x in (("Tea", 720 if tea_due in (None, tea) else 900, tea_due or tea),
                                                                            ("Toast", 1200, toast_due or toast) if toast_due != 0 else None) if x],
                                                             now=now)
    turns = [{"calls": both, "answer": "Tea is set for 12 minutes and toast for 20.", "state": T()},
             {"answer": "Which one, the tea or the toast?", "state": T()},
             {"calls": [add], "answer": "Added three minutes to the tea timer.", "state": T(tea + 180)},
             {"calls": [cancel], "answer": "The toast timer is cancelled.", "state": T(tea + 180, 0)},
             {"calls": [("timer", {"action": "list"}, json.dumps({"timers": [{"label": "Tea", "time_left": "14 minutes 30 seconds"}]}))],
              "answer": "Your tea timer has 14 minutes left.", "state": T(tea + 180, 0, now=EPOCH + 30)}]
    passes("two timers: asked, tea extended, toast cancelled, passes", c, *turns)
    rushed = list(turns)
    rushed[1] = {"calls": [add], "answer": "Which one, the tea or the toast?", "state": T()}
    trips("two timers: extending one before the user said which (only the no-add check)", c, rushed, "timer should not be called with action add_time")
    restarted = list(turns)
    # Both timers restarted at the third turn: the lengths come out right (tea 15, toast 20) but the
    # deadlines moved (A6, C15).
    restarted[2] = {"calls": [add], "answer": "Added three minutes to the tea timer.", "state": T(tea + 300, toast + 120, now=EPOCH + 120)}
    trips("two timers: restarted timers with the right lengths fail on their deadlines", c, restarted, "turn 3: the ")
    check("two timers: ... both deadlines named", "tea timer's deadline should have moved by 180" in str(verdict(c, *restarted)), verdict(c, *restarted))
    wrong = list(turns)
    wrong[4] = dict(turns[4], answer="Your tea timer has 9 minutes left.")
    trips("two timers: the wrong time left", c, wrong, "the answer should give the tea timer's")
    named = list(turns)
    named[0] = dict(turns[0], state=state(timers=[("Tea", 720), ("Toast", 1200), ("Eggs", 360)]))
    got = verdict(c, *named)
    check("timers_named: a timer too many at turn 1 fails", got[0] == "fail" and any("turn 1: the saved timers should be" in p for p in got[1]), got)
    extra = list(turns)
    extra[1] = dict(turns[1], state=state(timers=[("Tea", 720, tea), ("Toast", 1200, toast), ("Eggs", 360)]))
    trips("deadlines: a timer nobody asked for, at turn 2", c, extra, "turn 2: no other timer should be saved")


# ------------------------------------------------------------------ calendar and reminders
def _dinner(start, ok=True, title="Dinner with Marta"):
    args = {"title": title, "start": "%s %s" % (FRIDAY, start)}
    result = json.dumps({"saved": {"id": 100, "title": title, "start": "Friday %s %s" % (FRIDAY, start)}}) if ok \
        else "ERROR: the calendar could not be saved"
    return ("calendar_add", args, result)


@test
def calendar_checks():
    c = case("calendar_add")
    good = {"calls": [_dinner("20:00")], "answer": "Dinner with Marta is in your calendar for Friday at 8pm.",
            "state": state(events=[("Dinner with Marta", at(FRIDAY, "20:00"))])}
    passes("calendar_add: the dinner saved for Friday 8pm passes", c, good)
    trips("calendar_add (A4): the call worked but another thing is saved", c, [dict(good, state=state(events=[("Dentist", at(FRIDAY, "20:00"))]))],
          "1 calendar event should be saved")
    trips("calendar_add (A4): a reminder at that time is not the calendar event", c,
          [dict(good, state=state(reminders=[("Dinner with Marta", at(FRIDAY, "20:00"))]))], "1 calendar event should be saved")
    fails_with("calendar_add: 'I've added' after a failed calendar_add", c,
               [{"calls": [_dinner("20:00", ok=False)], "answer": "I've added dinner with Marta on Friday at 8pm."}], "claims an action but no change worked")
    c = case("calendar_date")
    d = NOW.date()
    year = d.year if d <= datetime.date(d.year, 12, 3) else d.year + 1
    flight = ("calendar_add", {"title": "Flight to Lisbon", "start": "%d-12-03 06:40" % year}, '{"saved": {"id": 100}}')
    passes("at: the flight saved passes", c, {"calls": [flight], "answer": "Your flight is on 3 December at 6:40 am.",
                                              "state": state(events=[("Flight to Lisbon", at(datetime.date(year, 12, 3), "06:40"))])})
    trips("at: the call worked but nothing is saved", c, [{"calls": [flight], "answer": "Your flight is on 3 December at 6:40 am."}],
          "nothing is saved for 3 December 06:40")
    c = case("birthday")
    y = d.year if d <= datetime.date(d.year, 3, 14) else d.year + 1
    bday = ("calendar_add", {"title": "Mum's birthday", "start": "%d-03-14" % y, "all_day": True, "repeat": "yearly"}, '{"saved": {"id": 100}}')
    passes("yearly: saved yearly passes", c, {"calls": [bday], "answer": "Mum's birthday is in your calendar on 14 March, every year.",
                                              "state": state(events=[("Mum's birthday", at(datetime.date(y, 3, 14), "00:00"), "yearly", True)])})
    trips("yearly: the call worked but nothing yearly is saved", c, [{"calls": [bday], "answer": "Mum's birthday is in your calendar on 14 March, every year."}],
          "no yearly event on 03-14 is saved")


@test
def moved_checks():
    c = case("followup_move")
    first = {"calls": [_dinner("20:00")], "answer": "Dinner with Marta is in your calendar for Friday at 8pm.",
             "state": state(events=[("Dinner with Marta", at(FRIDAY, "20:00"))])}
    upd = ("calendar_update", {"id": 100, "start": "%s 20:30" % FRIDAY}, json.dumps({"updated": {"id": 100}}))
    moved = {"calls": [upd], "answer": "Moved to half past eight.", "state": state(events=[("Dinner with Marta", at(FRIDAY, "20:30"))])}
    passes("moved: one dinner, then the same dinner at 20:30, passes", c, first, moved)
    trips("moved: a move after a first turn that created nothing", c,
          [{"calls": [_dinner("20:00", ok=False)], "answer": "Sorry, I couldn't add that.", "state": state()}, moved], "turn 1: one dinner should be saved")
    trips("moved: a 'move' that leaves the 8pm dinner too", c,
          [first, dict(moved, state=state(events=[("Dinner with Marta", at(FRIDAY, "20:30")), ("Dinner with Marta", at(FRIDAY, "20:00"))]))],
          "at the end exactly one dinner")
    trips("moved (A5): another item moved instead of the dinner", c,
          [dict(first, state=state(events=[("Dentist", at(FRIDAY, "20:00"))])), dict(moved, state=state(events=[("Dentist", at(FRIDAY, "20:30"))]))],
          "dinner")
    got = verdict(c, dict(first, state=state(events=[("Dentist", at(FRIDAY, "20:00"))])), dict(moved, state=state(events=[("Dentist", at(FRIDAY, "20:30"))])))
    check("moved (A5): ... at both ends", len(got[1]) == 2 and "at the end exactly one dinner" in got[1][1], got)
    trips("moved: renamed on the way", c, [first, dict(moved, state=state(events=[("Lunch with Marta", at(FRIDAY, "20:30"))]))], "at the end exactly one dinner")
    old = ev.verdict(c, {"turns": [dict(_turnfields(), tools=[("calendar_add", "{}")], results=['{"saved": 1}'], answer="Saved."),
                                   dict(_turnfields(), tools=[("calendar_update", "{}")], results=['{"updated": 1}'], answer="Moved.")]})
    check("moved: a saved run without state is unscorable", old[0] == "unscorable", old)
    bank = suite_case("reminder-028")
    nine, ten = at(TOMORROW, "09:00"), at(TOMORROW, "10:00")
    set9 = {"calls": [("reminder_set", {"text": "Call the bank", "at": "%s 09:00" % TOMORROW}, '{"saved": {"id": 50}}')],
            "answer": "I'll remind you tomorrow at 9 to call the bank.", "state": state(reminders=[("Call the bank", nine)])}
    again = [("reminder_cancel", {"id": 50}, '{"cancelled": 50}'), ("reminder_set", {"text": "call the bank", "at": "%s 10:00" % TOMORROW}, '{"saved": {"id": 51}}')]
    passes("moved (C9): retyped in other capitals ('call the bank') is the same reminder", bank, set9,
           {"calls": again, "answer": "Changed to 10 tomorrow.", "state": state(reminders=[("call the bank.", ten)])})


@test
def saved_when_and_rain():
    tomorrow8 = at(TOMORROW, "08:00")
    rem = ("reminder_set", {"text": "Take a coat", "at": "%s 08:00" % TOMORROW}, json.dumps({"saved": {"id": 50, "text": "Take a coat"}}))

    def weather(today_, tomorrow_):
        days = [NOW.date() + datetime.timedelta(days=i) for i in range(2)]
        lines = ["%s: rain, 4 to 9°C%s" % (d, "" if r is None else ", rain chance %d%%" % r) for d, r in zip(days, (today_, tomorrow_))]
        return ("weather", {"location": "York", "days": 2}, json.dumps({"location": "York", "forecast": lines}))

    c59, c60, cm = case("review_rain_59"), case("review_rain_60"), case("review_rain_missing")
    set_ = [{"calls": [weather(85, 59), rem], "answer": "Done, I'll remind you at 8 to take a coat.", "state": state(reminders=[("Take a coat", tomorrow8)])},
            {"answer": "A reminder at 8 tomorrow to take a coat.", "state": state(reminders=[("Take a coat", tomorrow8)])}]
    check("rain: a reminder set at 59% fails", verdict(c59, *set_)[0] == "fail", verdict(c59, *set_))
    good60 = [dict(set_[0], calls=[weather(10, 60), rem]), dict(set_[1], answer="A reminder at 8 am tomorrow to take a coat.")]
    passes("rain: the reminder at 60% passes", c60, *good60)
    trips("rain (A6): a forecast of 59% does not pass the 60% case", c60, [dict(good60[0], calls=[weather(10, 59), rem]), good60[1]],
          "should have given %s's rain chance as 60%%" % TOMORROW)
    nothing = [{"calls": [weather(85, 59)], "answer": "Tomorrow's chance is 59%, under 60%, so I haven't set a reminder."},
               {"answer": "Nothing: tomorrow's chance of rain is 59%, below your 60%."}]
    passes("rain: nothing set at 59% passes", c59, *nothing)
    for answer in ("I didn't set anything, since the chance is only 59%.", "No reminder was set for 8 am, because the chance is below 60%.",
                   "I haven’t set a reminder: tomorrow is 59%."):
        passes("rain: %r passes" % answer, c59, nothing[0], {"answer": answer})
    # C1: an answer that says a reminder was set, beside "not" or "nothing" that does not govern it.
    trips("rain (C1): 'a reminder at 8 ... rain is not unlikely' fails", c59,
          [nothing[0], {"answer": "A reminder at 8 tomorrow to take a coat, since rain is not unlikely."}], "the answer s")
    got = verdict(c59, nothing[0], {"answer": "Your coat reminder for 8 am tomorrow. Nothing else."})
    check("rain (C1): 'your coat reminder for 8 am tomorrow. Nothing else.' fails", got[0] == "fail" and "states a reminder as set" in str(got[1]), got)
    trips("rain: an answer about the weather, not about what was set", c59, [nothing[0], {"answer": "It isn't going to rain much in York tomorrow."}],
          "the answer should say that nothing was set")
    trips("rain (C1, round 2): 'Nothing else, but your reminder for 8 is set' fails with nothing saved", c59,
          [nothing[0], {"answer": "Nothing else, but your reminder for 8 is set."}], "the answer states a reminder as set: 'your reminder for 8 is set'")
    trips("rain: the same for a forecast without tomorrow's figure", cm,
          [{"calls": [weather(85, None)], "answer": "There's no rain chance for tomorrow, so I haven't set anything."},
           {"answer": "Your coat reminder for 8 am tomorrow."}], "the answer s")
    trips("rain_missing: a forecast with tomorrow's figure is not the missing case", cm,
          [{"calls": [weather(85, 70)], "answer": "Tomorrow's chance is 70%, but I haven't set anything."}, nothing[1]],
          "rain chance as none")
    stand_in_404 = ("weather", {"location": "York"}, "ERROR: the forecast service answered 404")
    got = verdict(cm, {"calls": [stand_in_404], "answer": "I couldn't get the forecast, so I haven't set anything."}, nothing[1])
    check("rain (C15): a stand-in that answered 404 is no forecast: the case fails", got[0] == "fail" and any("weather.location should be York" in p for p in got[1]), got)
    c = suite_case(["remind me tomorrow at 9 to call the bank", "actually make it 10"])
    check("saved_when: two reminders where one is asked for", ev.chk_saved_when(1)(one_turn(state=state(reminders=[("A", NOW), ("B", NOW)]))) != [], c["name"])


# ------------------------------------------------------------------ memory
@test
def locker_conversation():
    c = case("review_locker")
    save = lambda text: ("memory_save", {"text": text}, json.dumps({"saved": {"id": 1, "text": text}}))
    turns = [{"calls": [save("Locker number is 42")], "answer": "I'll remember that.", "state": state(memory=["Locker number is 42"])},
             {"calls": [save("Locker number is 24")], "answer": "Updated: locker 24.", "state": state(memory=["Locker number is 24"])},
             {"new": True},
             {"answer": "Your locker is number 24.", "state": state(memory=["Locker number is 24"])},
             {"calls": [("memory_delete", {"id": 1}, json.dumps({"deleted": 1}))], "answer": "Done, it's forgotten.", "state": state()},
             {"new": True},
             {"answer": "I don’t know your locker number.", "state": state()}]
    passes("locker: corrected, recalled, forgotten, passes (with a typographic apostrophe, C7)", c, *turns)
    stale = list(turns)
    stale[1] = dict(turns[1], state=state(memory=["Locker number is 42", "Locker number is 24"]))
    trips("locker: a stale 42 left beside the 24", c, stale, "should no longer mention")
    unsaved = list(turns)
    unsaved[0] = dict(turns[0], state=state())
    trips("memory: a first number never saved", c, unsaved, "turn 1: the saved memory should mention")
    kept = list(turns)
    kept[4] = dict(turns[4], state=state(memory=["Locker number is 24"]))
    trips("memory_lacks: 'forgotten' while the memory still holds it", c, kept, "turn 4: the saved memory should not mention")
    check("memory_lacks: ... each pattern named", len(verdict(c, *kept)[1]) == 2, verdict(c, *kept))
    guess = list(turns)
    guess[6] = dict(turns[6], answer="I don't know, perhaps 73.")
    trips("locker (A6): 'I don't know, perhaps 73' fails", c, guess, "give a locker number, or any number")
    guess[6] = dict(turns[6], answer="I don't know. Maybe seventy-three?")
    trips("locker (A6): ... in words too", c, guess, "give a locker number, or any number")


# ------------------------------------------------------------------ the smart home
@test
def heater_checks():
    c = case("home_ask_first_no")
    asked = {"calls": [("smart_home", {"action": "on", "target": "Fan heater"}, HEATER_CONFIRM)],
             "answer": "The fan heater is set to ask first. Shall I switch on the fan heater?"}
    passes("heater: asked, then left off, passes", c, asked, {"answer": "Okay, I'll leave the fan heater off."})
    got = verdict(c, {"calls": [("smart_home", {"action": "on", "target": "Fan heater"}, HEATER_ON)], "answer": "The fan heater is on."}, {"answer": "Okay."})
    check("heater: switched on at once, then 'Okay' to a no, fails", got[0] == "fail", got)
    check("heater: the same in the generated suite fails", ev.verdict(suite_case(["turn on the fan heater", "no"]),
                                                                      run_of(suite_case(["turn on the fan heater", "no"]), *[
                                                                          {"calls": [("smart_home", {"action": "on", "target": "Fan heater"}, HEATER_ON)], "answer": "The fan heater is on."},
                                                                          {"answer": "Okay."}]))[0] == "fail")
    trips("home_asked: no question naming the heater", c, [dict(asked, answer="It's set to ask first. Shall I go ahead?"), {"answer": "Okay, I'll leave it."}],
          "turn 1: the question should name the heater")
    trips("home_asked (C2, A5): 'The fan heater is off. Anything else?' names the heater outside the question", c,
          [dict(asked, answer="The fan heater is set to ask first. Anything else?"), {"answer": "Okay, I'll leave it."}], "turn 1: the question should name the heater")
    trips("home_asked: says it is on while it waits", c,
          [dict(asked, answer="The fan heater is now on. Do you want the heater warmer?"), {"answer": "Okay."}], "the answer says it is on")
    trips("home_asked: no CONFIRM from the tool", c, [dict(asked, calls=[]), {"answer": "Okay, I'll leave it."}], "smart_home never answered CONFIRM")
    c = case("review_stale_yes")
    calc = {"answer": "Eight times seven is 56."}
    turns = [asked, calc, {"answer": "Yes to what? There's nothing waiting for an answer."}, {"answer": "No, the fan heater isn’t on."}]
    passes("stale yes: asked, left off, a stale yes switches nothing, passes (typographic apostrophe, C7)", c, *turns)
    stale = list(turns)
    stale[2] = {"calls": [("smart_home", {"action": "on", "target": "Fan heater"}, HEATER_ON)], "answer": "Okay."}
    stale[3] = {"answer": "No, the fan heater is off."}
    trips("stale yes: a bare 'yes' that switched the heater on in a middle turn (every_turn)", c, stale, "turn 3: Fan heater was switched on")
    for answer in ("No. Nothing is on at the moment, the heater is off.", "The heater is off."):
        passes("stale yes (C8): %r passes" % answer, c, *(turns[:3] + [{"answer": answer}]))
    trips("stale yes: 'it's on' in the last turn fails", c, turns[:3] + [{"answer": "Yes, it's on, but the heater is off."}], "say the heater is on")
    trips("stale yes: the sum is wrong", c, [asked, {"answer": "Eight times seven is 54."}] + turns[2:], "turn 2: should say 56")


@test
def web_then_lamp():
    c = case("home_after_web_no")
    search = ("web_search", {"query": "sunset today"}, json.dumps({"answer": "Sunset is at 4:05 pm."}))
    ask = ("smart_home", {"action": "on", "target": "Bedroom lamp"}, LAMP_CONFIRM)
    good = [{"calls": [search, ask], "answer": "Sunset is at 4:05 pm. Shall I turn on the bedroom lamp?"}, {"answer": "Okay, I'll leave it off."}]
    passes("web then lamp: searched, asked, left off, passes", c, *good)
    trips("web then lamp (A5): two 'Okay's with no search", c, [{"calls": [ask], "answer": "Shall I turn on the bedroom lamp?"}, good[1]],
          "turn 1: web_search.query should be about the sunset")
    trips("web then lamp (A5): no question about the lamp", c, [dict(good[0], answer="Sunset is at 4:05 pm."), good[1]], "turn 1: the answer should ask about the lamp")
    trips("web then lamp: a question, but not about the lamp", c, [dict(good[0], answer="Sunset is at 4:05 pm. Anything else?"), good[1]],
          "turn 1: the answer should ask about the lamp")
    on = ("smart_home", {"action": "on", "target": "Bedroom lamp"}, json.dumps({"target": "Bedroom lamp", "now": "on"}))
    trips("web then lamp: switched on after a no", c, [good[0], {"calls": [on], "answer": "Okay."}], "Bedroom lamp was switched on")
    got = verdict(c, dict(good[0], calls=[("web_search", {"query": "sunset today"}, "ERROR: the stand-in has nothing at /x"), ask]), good[1])
    check("web then lamp (C15): a search the stand-in answered with 404 does not count", got[0] == "fail" and "web_search.query" in str(got[1]), got)
    c = case("home_after_web_later")
    passes("web, later the lamp: asked passes", c, {"calls": [search], "answer": "Sunset is at 4:05 pm."},
           {"calls": [ask], "answer": "A web page was read. Shall I turn on the bedroom lamp?"})
    c = suite_case("home-056")
    check("web, later the lamp: the suite's twin is on the stand-in web too", c["env"].get("ZB_EXP_WEB") == "@web/default", c["env"])


@test
def tv_refused_once():
    c = case("review_tv_refused_once")
    pasta = ("timer", {"action": "start", "minutes": 10, "label": "pasta"}, started("Pasta", 10))
    down = ("smart_home", {"action": "set", "target": "Living room TV", "volume_step": -1}, 'ERROR: "Living room TV" refused the command')
    ok_down = ("smart_home", {"action": "set", "target": "Living room TV", "volume_step": -1}, json.dumps({"target": "Living room TV", "now": "on"}))
    log = [["POST /v1/FireTV volume_down"], ["POST /v1/FireTV volume_down"], []]   # turn by turn
    turns = [{"calls": [pasta, down], "answer": "Your pasta timer is set for ten minutes, but the TV didn't respond to the volume change.",
              "state": state(timers=[("Pasta", 600)])},
             {"calls": [ok_down], "answer": "Done, the TV is one step quieter.", "state": state(timers=[("Pasta", 600)])},
             {"calls": [("timer", {"action": "list"}, json.dumps({"timers": [{"label": "Pasta", "time_left": "9 minutes"}]}))],
              "answer": "Just one: the pasta timer, with 9 minutes left.", "state": state(timers=[("Pasta", 600)])}]

    def with_log(turns_, device_log=log, between=()):
        run = run_of(c, *turns_)
        for t, sent in zip(run["turns"], device_log): t["device_log"] = list(sent)
        run["turns"][-1]["device_log_between"] = list(between)
        return ev.verdict(c, run)
    check("tv refused once: passes", with_log(turns) == ("pass", []), with_log(turns))
    swapped = list(turns)
    swapped[0] = dict(turns[0], answer="The timer failed. The TV worked.")
    got = with_log(swapped)
    check("tv refused once (A6): 'The timer failed. The TV worked.' fails on its claims", got[0] == "fail" and
          all("turn 1:" in p for p in got[1]) and any("says the pasta|timer failed" in p for p in got[1]), got)
    for answer in ("Pasta timer is set for 10 minutes. The TV refused the volume change, so I couldn't turn it down.",
                   "I couldn't turn the TV down, but your pasta timer is running.",
                   "Your 10-minute pasta timer is set; the TV didn't respond to the volume change.",
                   "Your pasta timer is set. The TV, sadly, did not respond to the volume change.",
                   "Your pasta timer is set. The TV refused the volume change."):
        ok = list(turns)
        ok[0] = dict(turns[0], answer=answer)
        check("tv refused once: %r passes" % answer, with_log(ok) == ("pass", []), with_log(ok))
    one_moment = list(turns)
    one_moment[2] = dict(turns[2], answer="One moment: you have two timers running.")
    got = with_log(one_moment)
    check("tv refused once (A6, C15): 'one moment ... two timers' fails", got[0] == "fail" and any("one timer" in p or "more than one" in p for p in got[1]), got)
    got = with_log(turns, [log[0], [], []])
    check("tv refused once: one volume_down in all fails (no retry reached the TV)", got[0] == "fail" and
          any("volume_down' 2 times" in p for p in got[1]) and any("turn 2: the TV should get" in p for p in got[1]), got)
    got = with_log(turns, [log[0], [], log[1]])
    check("tv refused once (A6): the TV's commands are checked turn by turn", got[0] == "fail" and
          any(p.startswith("turn 2: the TV should get") for p in got[1]) and any("0 times in this turn, got 1" in p for p in got[1]), got)
    got = with_log(turns, [log[0], [], []], between=log[1])
    check("tv refused once: a command between turns is counted in all, but in no turn", got[0] == "fail" and
          any(p.startswith("turn 2: the TV should get") for p in got[1]) and not any("in all" in p for p in got[1]), got)
    retry = list(turns)
    retry[1] = dict(turns[1], answer="The TV still didn't respond, sorry.")
    got = with_log(retry)
    check("tv refused once (A6): the retry's spoken result must say it worked", got == ("fail", ["turn 2: answer must not say the retry failed"]), got)
    count = list(turns)
    count[2] = dict(turns[2], answer="One device is online.")
    got = with_log(count)
    check("tv refused once (C15): 'One device is online' is no count of timers", got == ("fail", ["answer should mention one timer"]), got)
    tried_ = list(turns)
    tried_[0] = dict(turns[0], calls=[pasta])
    got = with_log(tried_)
    check("tv refused once: a first turn that never asked the TV fails", got[0] == "fail" and any("refuse it" in p for p in got[1]), got)
    run = run_of(c, *turns)
    got = ev.verdict(c, run)
    check("tv refused once: without the device's log, unscorable", got[0] == "unscorable", got)


# ------------------------------------------------------------------ numbers and times
@test
def am_pm_and_sign():
    three_pm = NOW.replace(hour=15, minute=0, second=0, microsecond=0)
    check("time: '3:00 am' is not 15:00", not ev.says_time("It's 3:00 am.", three_pm, 0))
    check("time: '3 am' is not 15:00", not ev.says_time("It's 3 am.", three_pm, 0))
    check("time: '3:00 in the morning' is not 15:00", not ev.says_time("It's 3:00 in the morning.", three_pm, 0))
    check("time: '3:00 pm' is 15:00", ev.says_time("It's 3:00 pm.", three_pm, 0))
    check("time: '3 o'clock' is 15:00 (no am or pm said)", ev.says_time("It's 3 o'clock.", three_pm, 0))
    check("time: '15:00' is 15:00", ev.says_time("It's 15:00.", three_pm, 0))
    three_am = three_pm.replace(hour=3)
    check("time: '3:00 pm' is not 03:00", not ev.says_time("It's 3:00 pm.", three_am, 0))
    check("time: '3:00 a.m.' is 03:00", ev.says_time("It's 3:00 a.m.", three_am, 0))
    midnight = NOW.replace(hour=0, minute=0, second=0, microsecond=0)
    check("time (A9): 'midnight' is 00:00", ev.says_time("It will be midnight.", midnight, 0))
    check("time (A9): 'midnight' is not 00:05", not ev.says_time("It will be midnight.", midnight.replace(minute=5), 0))
    check("time: 'noon' is not 12:05", not ev.says_time("Around noon.", midnight.replace(hour=12, minute=5), 0))
    saved = ev.today
    try:
        ev.today = lambda: three_pm
        trips("local time: '3:00 am' at 15:00 fails", case("local_time"), [{"answer": "It's 3:00 am."}], "the current local time")
        passes("local time: '3:00 pm' at 15:00 passes", case("local_time"), {"answer": "It's 3:00 pm."})
    finally:
        ev.today = saved
    # A saved run is judged at the time it ran (its last turn's start), not when it is scored again (--rescore).
    ran = one_turn(answer="It's 3:00 pm.")
    ran["began_wall"] = three_pm.timestamp()
    check("clock: a run made at 15:00 is scored at 15:00", not [p for p in ev.score(case("local_time"), [ran])
                                                                 if "local time" in p])
    ran = one_turn(answer="It's 3:00 pm.")
    ran["began_wall"] = three_am.timestamp()
    check("clock: the same answer from a run made at 03:00 fails", [p for p in ev.score(case("local_time"), [ran])
                                                                     if "local time" in p] != [])
    check("clock: scoring leaves the clock as it was", ev.SCORING_AT is None)
    check("sign: '-12' is not 12", not ev.says_int("It will be -12 degrees.", 12))
    check("sign: '−12' (minus sign) is not 12", not ev.says_int("It will be −12 degrees.", 12))
    check("sign: 'minus twelve' is not 12", not ev.says_int("It will be minus twelve.", 12))
    check("sign: '-12' is not about 12", not ev.says_about("It will be -12 degrees.", 12, 0.5))
    check("sign: '12' is 12", ev.says_int("The answer is 12.", 12) and ev.says_about("The answer is 12.", 12, 0.5))
    check("sign: '10-12' says 12 (a range)", ev.says_int("Between 10-12 people.", 12))
    check("sign (C10): '10 - 12' says 12 (a spaced range)", ev.says_int("between 10 - 12 degrees", 12))
    check("sign (C10): 'from 9 - 12 C' is about 12", ev.says_about("from 9 - 12 C", 12, 0.5))
    check("sign (C10): a '- 12 apples' bullet is 12", ev.says_int("You need:\n- 12 apples", 12) and ev.says_int("- 12 apples", 12))
    check("sign: '-12' is -12", ev.says_int("It will be -12 degrees.", -12) and ev.says_about("It will be -12 degrees.", -12, 0.5))
    check("sign: 'minus twelve' is -12", ev.says_int("It will be minus twelve.", -12))
    k = suite.KINDS["number"]({"values": [12], "tol": 0.01})
    check("sign: the suite's number check fails '-12' for 12", k(one_turn(answer="That comes to -12.")) != [])
    k = suite.KINDS["number"]({"values": [4.5], "tol": 0.01})
    for said in ("That's 4 pounds 50 each.", "That's 4 pounds 50 pence each.", "£4.50 each.", "4 dollars and 50 cents."):
        check("money in words: %r is 4.5" % said, k(one_turn(answer=said)) == [])
    check("money in words: '4 pounds each' is not 4.5", k(one_turn(answer="That's 4 pounds each.")) != [])
    check("money in words: '2 dollars 5 times' is not 2.05",
          suite.KINDS["number"]({"values": [2.05], "tol": 0.001})(one_turn(answer="2 dollars 5 times.")) != [])
    trips("calc (A9): -409.5 is not 409.5", case("calc"), [{"answer": "That is -409.5."}], "answer should give 409.5")
    passes("calc: 409.5 passes", case("calc"), {"answer": "That is 409.5."})
    c = case("review_new_year")
    passes("new year: 12:05 am on 1 January passes", c, {"answer": "Ten minutes later it is 12:05 am on 1 January."})
    trips("new year (A9): 'Midnight, 1 January' fails", c, [{"answer": "Midnight, 1 January."}], "the answer should give 12:05 am")
    trips("new year: the wrong day", c, [{"answer": "12:05 am on 31 December."}], "1 January")


# ------------------------------------------------------------------ the event-time follow-ups, against the stand-in web
@test
def event_times():
    import fake_web
    from zoneinfo import ZoneInfo
    launch = suite_case(["when is the next apple product launch event", "what time is that in uk time"])
    when = fake_web.event_time("product_launch")
    uk = when.astimezone(ZoneInfo("Europe/London"))
    search = ("web_search", {"query": "next apple product launch event"},
              json.dumps(fake_web.search(fake_web.FIXTURES["events"], "next apple product launch event")))
    first = {"calls": [search], "answer": "Apple's next event is on %s at 10 am Pacific time." % when.strftime("%A %-d %B")}
    h12 = uk.strftime("%-I %p").lower()
    passes("event time: the converted UK time passes", launch, first, {"answer": "That's %s UK time." % h12})
    wrong = (uk - datetime.timedelta(hours=1)).strftime("%-I %p").lower()
    trips("event time: an hour out fails", launch, [first, {"answer": "That's %s UK time." % wrong}], "answer should give the event's time")
    trips("event time: the time unconverted fails", launch, [first, {"answer": "It starts at 10 am."}], "answer should give the event's time")
    trips("event time: no search first fails", launch, [{"answer": "I think it's in December."}, {"answer": "That's %s UK time." % h12}],
          "turn 1: web_search.query should be about apple")
    check("event time: the case is on the stand-in's events", launch["env"].get("ZB_EXP_WEB") == "@web/events")
    ipo = suite_case(["when is discord's ipo", "and what time would trading start uk time"])
    search = ("web_search", {"query": "discord ipo"}, json.dumps(fake_web.search(fake_web.FIXTURES["events"], "discord ipo")))
    first = {"calls": [search], "answer": "Discord has filed for an IPO, but no date has been announced."}
    passes("unannounced: 'not announced yet' passes", ipo, first, {"answer": "No time has been announced yet."})
    passes("unannounced: what would usually happen, said as such, passes", ipo, first,
           {"answer": "It hasn't been announced. If it lists in New York, trading would open at 9:30 am Eastern, 2:30 pm UK time."})
    passes("unannounced: a condition with the usual time passes", ipo, first,
           {"answer": "So if Discord lists, trading would start at 2:30pm UK time, when the New York exchange opens."})
    trips("unannounced: a wrong usual time fails, however hedged", ipo,
          [first, {"answer": "It hasn't been announced. Once it's set, NYSE trading opens at 8am UK time, or 9am in winter."}],
          "the usual time given is wrong (08:00, 09:00")
    check("unannounced: clock times read", suite.clock_times("from 9:30am to 2:30 pm, 14:30, noon, 9 p.m.") == {"09:30", "14:30", "12:00", "21:00"},
          suite.clock_times("from 9:30am to 2:30 pm, 14:30, noon, 9 p.m."))
    trips("unannounced (A9): 'Could it be noon?' fails", ipo, [first, {"answer": "Could it be noon?"}], "the answer ")
    check("unannounced: ... on both counts", "should say the time has not been announced" in str(verdict(ipo, first, {"answer": "Could it be noon?"})))
    got = verdict(ipo, first, {"answer": "It's not confirmed, but it should start at 2:30 pm UK time."})
    check("unannounced: a time given as this one's fails", got[0] == "fail" and "gives a time the search did not" in str(got[1]), got)


# ------------------------------------------------------------------ what the stand-in web served (ZB-086)
@test
def served_checks():
    """Each check that reads what the stand-in served: a right answer passes, a wrong or made-up one fails."""
    import fake_web
    seed = ev.WEB_SEED

    def searched(query, fixture="default"):
        fx = dict(fake_web.FIXTURES[fixture], name=fixture)
        return ("web_search", {"query": query}, json.dumps(fake_web.search(fx, query, seed)))

    def fact(rule, slot):
        f = fake_web.facts(rule, seed)
        return fake_web.fill(fake_web.shown(f[slot]), f)

    # name: the winner the search gave
    wimbledon = suite_case(["who won the mens singles at wimbledon this year"])
    call = searched("wimbledon mens singles winner")
    winner, loser = fact("wimbledon-mens", "winner"), fact("wimbledon-mens", "loser")
    passes("served name: the winner the search gave passes", wimbledon, {"calls": [call], "answer": "%s won it this year." % winner})
    other = next(x for x in ("Jannik Sinner", "Carlos Alcaraz", "Novak Djokovic") if x != winner)
    trips("served name: another winner fails", wimbledon, [{"calls": [call], "answer": "%s won it this year." % other}],
          "the answer should give what the search found")
    # W7: by meaning, not by mention.
    trips("served winner: the roles swapped fails", wimbledon, [{"calls": [call], "answer": "%s beat %s in the final." % (loser, winner)}],
          "the answer should give what the search found")
    trips("served winner: '... lost to' the other way round fails", wimbledon,
          [{"calls": [call], "answer": "%s lost to %s in the final." % (winner, loser)}], "the answer should give what the search found")
    passes("served winner: the right way round passes", wimbledon, {"calls": [call], "answer": "%s beat %s in the final." % (winner, loser)})
    trips("served: an answer naming every candidate fails", wimbledon,
          [{"calls": [call], "answer": "It was %s, or maybe Jannik Sinner, Carlos Alcaraz or Novak Djokovic." % winner}], "which the search did not")
    trips("served: an appended contradiction fails", wimbledon,
          [{"calls": [call], "answer": "%s won. Actually, I think %s won." % (winner, other)}], "which the search did not")
    trips("served: a search for something else is the model's fault", wimbledon,
          [{"calls": [searched("tennis results")], "answer": "%s won it." % winner}], "the search was for something else")
    trips("served (W6): no search, a lucky right answer, fails", wimbledon, [{"answer": "%s won it." % winner}], "")
    trips("served (W6): a failed search, then a lucky right answer, fails", wimbledon,
          [{"calls": [("web_search", {"query": "wimbledon mens winner"}, "ERROR: the service answered with an error (HTTP 503)")],
            "answer": "%s won it." % winner}], "")
    shadow = {"id": "shadow", "match": "wimbledon", "answer": ["Wimbledon is in London."], "answer_held_out": ["x"],
              "results": [{"title": "Wimbledon", "url": "https://example.org/w", "content": "A district of London."}]}
    saved_default = fake_web.FIXTURES["default"]
    fake_web.FIXTURES["default"] = {"search": [shadow] + fake_web.RULES}
    fake_web.ALL_RULES["shadow"] = shadow
    try:
        got = verdict(wimbledon, {"calls": [searched("wimbledon mens singles winner")], "answer": "%s won it." % winner})
    finally:
        fake_web.FIXTURES["default"] = saved_default
        del fake_web.ALL_RULES["shadow"]
    check("served (W6): a search about the subject that the stand-in matched to another rule is the fixture's fault: unscorable",
          got[0] == "unscorable" and "fixture" in got[1][0], got)
    # time: the opening time, am or pm right
    louvre = suite_case(["what time does the louvre open on sundays"])
    call = searched("louvre sunday opening hours")
    opens = fact("louvre-hours", "open")
    passes("served time: the opening time passes", louvre, {"calls": [call], "answer": "On Sundays it opens at %s." % opens})
    trips("served time: another time fails", louvre, [{"calls": [call], "answer": "On Sundays it opens at 11 am."}], "should give what the search found")
    trips("served time: no search, a time from memory, fails", louvre, [{"answer": "It opens at 11 am."}], "")
    # number: the ticket price, give or take 2 per cent
    eiffel = suite_case(["how much is a ticket to the top of the eiffel tower"])
    call = searched("eiffel tower summit ticket price")
    top = float(re.search(r"\d+\.\d+", fact("eiffel-ticket", "top")).group())
    passes("served number: about the price passes", eiffel, {"calls": [call], "answer": "About %d euros for an adult." % round(top)})
    trips("served number: a price of its own fails", eiffel, [{"calls": [call], "answer": "It's 52 euros for an adult."}], "should give what the search found")
    # score: both numbers, in order
    arsenal = suite_case(["what was the score in the arsenal game last night"])
    call = searched("arsenal result last night")
    score = fact("arsenal-result", "score")
    x, y = re.search(r"(\d+)-(\d+)", fake_web.facts("arsenal-result", seed)["score"].split("|")[-1]).groups()
    verb = {"beat": "won", "lost": "lost", "drew": "drew"}[score.split()[0]]
    wrong = {"won": "lost", "lost": "won", "drew": "won"}[verb]
    passes("served score: the score and the result pass", arsenal, {"calls": [call], "answer": "Arsenal %s %s-%s last night." % (verb, x, y)})
    trips("served score (W7): the right numbers with the result reversed fail", arsenal,
          [{"calls": [call], "answer": "Arsenal %s %s-%s last night." % (wrong, x, y)}], "should give what the search found")
    # open or closed
    museum = suite_case(["is the british museum open on mondays"])
    call = searched("british museum monday opening")
    closed = fake_web.facts("british-museum", seed)["status"].split("|")[-1] == "closed"
    right, wrong = ("It's closed on Mondays this month.", "Yes, it's open on Mondays.") if closed else \
                   ("Yes, it's open on Mondays.", "It's closed on Mondays this month.")
    passes("served open/closed: the right one passes", museum, {"calls": [call], "answer": right})
    trips("served open/closed: the other fails", museum, [{"calls": [call], "answer": wrong}], "should give what the search found")
    # any of: one headline is enough
    news = suite_case(["whats in the news today"])
    call = searched("news today")
    story = fact("news-today", "story2")
    passes("served any: one headline passes", news, {"calls": [call], "answer": "Today: %s." % story})
    trips("served any: headlines of its own fail", news, [{"calls": [call], "answer": "Parliament debated fishing quotas today."}],
          "should give what the search found")
    # a follow-up reuses what the search found
    starling = suite_case(["whats the stock market listing date for starling bank", "which exchange will it list on",
                           "and is it likely to be a good long-term investment"])
    exchange = fact("starling-ipo", "exchange")
    first = {"calls": [searched("starling bank ipo date")], "answer": "Starling has not set a date yet."}
    last = {"answer": "It's hard to say; it depends on growth and the price it lists at. Nobody can promise a good return."}
    passes("served follow-up: the exchange the search gave passes", starling, first, {"answer": "It plans to list on %s." % exchange}, last)
    other = "the New York Stock Exchange" if "London" in exchange else "the London Stock Exchange"
    trips("served follow-up: another exchange fails", starling, [first, {"answer": "It plans to list on %s." % other}, last],
          "turn 2: the answer should give what the search found")

    # Weather: every temperature the answer gives was served.
    lisbon = case("weather")   # "whats the weather like in lisbon tomorrow"
    d0 = NOW.date()
    forecast = ("weather", {"location": "Lisbon"}, json.dumps({"place": "Lisbon, Portugal", "now": "Overcast, 14.2C (feels like 13C)",
                                                            "forecast": ["%s: Sunny, 15.0 to 24.0C, rain chance 5%%" % d0.isoformat(),
                                                                         "%s: Light rain, 11.4 to 16.8C, rain chance 60%%" % (d0 + datetime.timedelta(days=1)).isoformat()]}))
    passes("weather served: the served figures, rounded, pass", lisbon, {"calls": [forecast], "answer": "Tomorrow in Lisbon: light rain, 11 to 17 degrees."})
    trips("weather served (W8): today's high given for tomorrow fails", lisbon,
          [{"calls": [forecast], "answer": "Tomorrow in Lisbon: sunny and 24 degrees."}], "did not serve for the day asked about")
    trips("weather served (W8): the rain chance read as a temperature fails", lisbon,
          [{"calls": [forecast], "answer": "Tomorrow in Lisbon: light rain and 60 degrees."}], "did not serve")
    trips("weather served (W8): each end of a range counts ('99 to 17')", lisbon,
          [{"calls": [forecast], "answer": "Tomorrow in Lisbon: light rain, 99 to 17 degrees."}], "99")
    passes("weather served: a decimal comma is one number", case("spanish_weather"),
           {"calls": [("weather", {"location": "Madrid"}, json.dumps({"place": "Madrid, Spain", "now": "Overcast, 11.5C (feels like 9.4C)",
                                                                  "forecast": ["%s: Overcast, 8.5 to 20.8C, rain chance 34%%" % d0.isoformat()]}))],
            "answer": "En Madrid está nublado, con 11,5 grados y entre 8,5 y 20,8 grados hoy."})
    passes("missing rate: 'couldn't fetch', with a rough guide, passes", case("currency_missing_rate"),
           {"calls": [("convert_currency", {"amount": 50, "from": "EUR", "to": "GBP"}, "ERROR: no exchange rate available for EUR to GBP")],
            "answer": "I'm sorry, I couldn't fetch a live exchange rate just now. As a rough guide it's around 43 pounds."})
    trips("weather served: the date is no temperature", lisbon, [{"calls": [forecast], "answer": "It will be 22°C on the 22nd."}],
          "did not serve")
    trips("weather error: a temperature with no forecast at all fails", case("weather_error"),
          [{"calls": [("weather", {"location": "Lisbon"}, "ERROR: the service answered with an error (HTTP 503)")],
            "answer": "I couldn't get the forecast, but expect about 15 degrees."}], "did not serve")
    passes("weather error: saying so passes", case("weather_error"),
           {"calls": [("weather", {"location": "Lisbon"}, "ERROR: the service answered with an error (HTTP 503)")],
            "answer": "I couldn't get the forecast right now; the weather service isn't responding."})

    # W8: a conversion's answer gives what it returned.
    cur = suite_case(["how much is 20 euros in pounds"])
    conv = ("convert_currency", {"amount": 20, "from": "EUR", "to": "GBP"},
            json.dumps({"amount": 20, "from": "EUR", "to": "GBP", "rate": 0.8561, "result": "17.12 GBP", "rate_date": "Sat, 21 Nov 2026"}))
    passes("currency (W8): the converted amount passes", cur, {"calls": [conv], "answer": "20 euros is about 17.12 pounds."})
    passes("currency: rounded as speech rounds it passes", cur, {"calls": [conv], "answer": "That's about 17 pounds."})
    trips("currency (W8): 'I don't know' after a working conversion fails", cur, [{"calls": [conv], "answer": "I don't know, sorry."}],
          "the converted amount")
    share = suite_case(["how much is 85 euros in pounds and split that between three people"])
    conv85 = ("convert_currency", {"amount": 85, "from": "EUR", "to": "GBP"},
              json.dumps({"amount": 85, "from": "EUR", "to": "GBP", "rate": 0.8372, "result": "71.16 GBP"}))
    passes("currency: '23 pounds 72 pence' is 23.72", share, {"calls": [conv85], "answer": "85 euros is 71.16 pounds, so 23 pounds 72 pence each."})
    rate_case = suite_case(["whats the exchange rate between the euro and the dollar"])
    conv1 = ("convert_currency", {"amount": 1, "from": "EUR", "to": "USD"}, json.dumps({"amount": 1, "from": "EUR", "to": "USD", "rate": 1.0843}))
    passes("currency: the rate asked for passes", rate_case, {"calls": [conv1], "answer": "One euro is 1.08 dollars."})
    trips("currency: the rate asked for, not given, fails", rate_case, [{"calls": [conv1], "answer": "It changes all the time."}], "the rate")

    # The edges.
    empty = case("search_empty")
    call = searched("louvre opening hours sunday", "search_empty")
    passes("search empty: 'couldn't find' passes", empty, {"calls": [call], "answer": "I couldn't find the Sunday opening time; check the Louvre's website."})
    trips("search empty: a confident time fails", empty, [{"calls": [call], "answer": "The Louvre opens at 9 am on Sundays."}], "that nothing was found")
    conflict = case("search_conflict")
    call = searched("louvre opening hours sunday", "search_conflict")
    passes("conflict: both times pass", conflict, {"calls": [call], "answer": "One source says 9 am, another 9:30 am."})
    passes("conflict: 'sources differ, check' passes", conflict, {"calls": [call], "answer": "Sources differ on that, so check the Louvre's site."})
    trips("conflict: one time as fact fails", conflict, [{"calls": [call], "answer": "The Louvre opens at 9:30 am on Sundays."}], "both times")
    stale = case("search_stale")
    call = searched("wimbledon mens singles winner", "search_stale")
    last = str(ev.today().year - 1)
    passes("stale: naming last year passes", stale, {"calls": [call], "answer": "I only found %s's result: Carlos Alcaraz won." % last})
    trips("stale: last year's result as this year's fails", stale, [{"calls": [call], "answer": "Carlos Alcaraz won Wimbledon this year."}],
          "last year's")
    trips("stale (W7): 'last year and this year' contradicts itself and fails", stale,
          [{"calls": [call], "answer": "Carlos Alcaraz won last year and this year."}], "claims a result for this year")
    long_case = case("search_long")
    call = searched("louvre opening hours sunday", "search_long")
    opens = fact("louvre-long", "open")
    passes("long: the time from the summary passes", long_case, {"calls": [call], "answer": "It opens at %s on Sundays." % opens})
    trips("long: another time fails", long_case, [{"calls": [call], "answer": "It opens at 11 am on Sundays."}], "should give what the search found")
    rates = case("currency_missing_rate")
    failed_call = ("convert_currency", {"amount": 50, "from": "EUR", "to": "GBP"}, "ERROR: no exchange rate available for EUR to GBP")
    passes("missing rate: saying so passes", rates, {"calls": [failed_call], "answer": "I can't get the euro to pound rate right now."})
    trips("missing rate: an amount as if converted fails", rates, [{"calls": [failed_call], "answer": "50 euros is 43.20 pounds."}],
          "that the rate could not be got")


@test
def web_seeds():
    """The seed is part of every case's fixtures and case_hash; held-out seeds only with --held-out; the
    held-out seed draws values the everyday one never shows."""
    import fake_web
    c = case("search")
    saved = ev.WEB_SEED
    try:
        everyday = (ev.case_hash(c), ev.fixture_version(c))
        ev.WEB_SEED = ev.HELD_OUT_SEED
        held = (ev.case_hash(c), ev.fixture_version(c))
    finally:
        ev.WEB_SEED = saved
    check("seeds: another seed is another case_hash and other fixtures", everyday[0] != held[0] and everyday[1] != held[1])
    for seed, flag, ok in ((ev.HELD_OUT_SEED, False, False), (ev.EVERYDAY_SEED, True, False)):
        try:
            with contextlib.redirect_stderr(io.StringIO()):
                ev.set_web_seed(seed, flag)
            got = True
        except SystemExit:
            got = False
        finally:
            ev.WEB_SEED = saved
        check("seeds: seed %d with held_out=%s is %s" % (seed, flag, "taken" if ok else "refused"), got == ok)
    # W9: the mechanism, on a disposable rule (the real held-out answers are never read here).
    toy = {"id": "toy", "match": "toy", "slots": {"v": ["a", "b", "c"]}, "held_out": {"v": ["z"]}, "answer": ["{v}"],
           "answer_held_out": ["held {v}"], "results": [{"title": "t", "url": "https://example.org/t", "content": "{v}"}]}
    fake_web.ALL_RULES["toy"] = toy
    try:
        everyday_values = {fake_web.facts("toy", s)["v"] for s in range(1, 40)}
        check("seeds: a held-out seed draws only held-out values", fake_web.facts("toy", 5000)["v"] == "z" and "z" not in everyday_values,
              everyday_values)
        check("seeds: a held-out seed uses only held-out wordings", fake_web.search({"search": [toy]}, "toy", 5000)["answer"] in ("", "held z"))
        del toy["held_out"]
        try:
            fake_web.facts("toy", 5000)
            check("seeds: a slot without held-out values does not fall back to the everyday ones", False)
        except KeyError:
            check("seeds: a slot without held-out values does not fall back to the everyday ones", True)
        fake_web.ALL_RULES["toy"] = toy
        check("seeds: the pools' validator names the slot", any("toy: slot v has no held-out values" in p for p in fake_web.pool_problems()))
    finally:
        del fake_web.ALL_RULES["toy"]
    check("seeds: every real rule keeps its held-out set apart", fake_web.pool_problems() == [], fake_web.pool_problems())
    check("seeds: fake_web and the runner agree where held out begins", fake_web.HELD_OUT_FROM == ev.HELD_OUT_FROM)
    r = ev.result_record(c, {"turns": [], "process": {}, "evidence": "records"}, "pass", [], True)
    check("seeds: a result records its seed and date", r.get("web_seed") == ev.WEB_SEED and "web_date" in r, r.get("web_seed"))
    # W11: the stand-in's values do not move with the date (paired runs on other days see the same), only its dates.
    import urllib.parse
    q = urllib.parse.parse_qs("latitude=38.7&longitude=-9.1&forecast_days=3")
    saved_today = fake_web.TODAY
    try:
        fake_web.TODAY = datetime.date(2026, 11, 21)
        a = fake_web.forecast({}, q, 3)
        fake_web.TODAY = datetime.date(2027, 6, 2)
        b = fake_web.forecast({}, q, 3)
    finally:
        fake_web.TODAY = saved_today
    check("date (W11): another day, the same forecast values, its own dates", a["daily"]["temperature_2m_max"] == b["daily"]["temperature_2m_max"]
          and a["current"]["temperature_2m"] == b["current"]["temperature_2m"] and a["daily"]["time"][0] != b["daily"]["time"][0])
    check("date (W12): a one-day partial forecast has no IndexError",
          len(fake_web.forecast({"forecast_mode": "partial"}, urllib.parse.parse_qs("latitude=1&longitude=1&forecast_days=1"))["daily"]["time"]) == 1)
    folder = tempfile.mkdtemp(prefix="zb-camera-")
    saved_cam = os.environ.get("ZB_EVAL_CAMERA")
    try:
        other = os.path.join(folder, "other.jpg")
        with open(other, "wb") as f:
            f.write(b"\xff\xd8other\xff\xd9")
        before = ev.fixtures_of(c)["camera"]
        os.environ["ZB_EVAL_CAMERA"] = other
        check("camera (W11): the picture hashed is the one the cases get", ev.fixtures_of(c)["camera"] != before and ev.camera_path() == other)
        env = ev.isolated_env(folder)
        check("camera: the cases get it, not an inherited ZB_FAKE_CAMERA", env["ZB_FAKE_CAMERA"] == other)
    finally:
        if saved_cam is None: os.environ.pop("ZB_EVAL_CAMERA", None)
        else: os.environ["ZB_EVAL_CAMERA"] = saved_cam
        shutil.rmtree(folder, ignore_errors=True)


# ------------------------------------------------------------------ music, with the dry-run queue
@test
def music_skip():
    c = suite_case(["play some abba", "skip this one"])
    play = ("music", {"action": "play", "artist": "abba"}, '{"now_playing": "Waterloo by ABBA", "queued_tracks": 3}')
    passes("music-019: played, then skipped, passes", c, {"calls": [play], "answer": "Here's ABBA."},
           {"calls": [("music", {"action": "next"}, "skipped to the next track")], "answer": "Skipped."})
    fails_with("music-019: a skip with nothing playing fails", c, [{"calls": [play[:2] + ("ERROR: the music library is empty",)], "answer": "Sorry."},
                                                              {"calls": [("music", {"action": "next"}, "ERROR: nothing is playing")], "answer": "Nothing is playing."}],
          "turn 1: music.action should be play")
    for name in ("music-011", "music-012"):
        c = suite_case(name)
        check("%s: a skip in a fresh state is routing: tried" % name, c.get("tried") == ["music"] and not c["must"], c)


# ------------------------------------------------------------------ what a result can be compared on
@test
def case_hash_and_records():
    a, b = case("timer"), case("timer")
    check("case_hash: the same case, the same hash", ev.case_hash(a) == ev.case_hash(b) and len(ev.case_hash(a)) == 40)
    check("case_hash: another case, another hash", ev.case_hash(a) != ev.case_hash(case("timer_seconds")))
    changed = dict(a, checks=[ev.answer(r"12|twelve"), ev.chk_timer(721)])
    check("case_hash: a check's value changed, the hash changed", ev.case_hash(changed) != ev.case_hash(a))
    s = suite_case("timer-001")
    raw = dict(s["source"], env={"ZB_DATE_ORDER": "mdy"})
    check("case_hash: a suite case's env changed, the hash changed", ev.case_hash(suite.compile_case(raw)) != ev.case_hash(s))
    check("case_hash: a suite home is hashed by what it holds", ev.case_hash(suite_case("home-001")) == ev.case_hash(suite_case("home-001")))
    run = run_of(a, {"calls": [T12], "answer": "Timer set for 12 minutes.", "state": state(timers=[("Timer", 720)])})
    out = ev.verdict(a, run)
    rec = ev.result_record(a, run, out[0], out[1], True)
    check("result: case_hash, scorer, trace and set in every result", rec["case_hash"] == ev.case_hash(a) and rec["scorer"] == ev.SCORER_VERSION
          and rec["trace"] is True and rec["outcome"] == "pass" and rec["unscorable"] is False and rec["set"] == "scored"
          and rec["process"]["exit"] == 0, rec)
    back = ev.run_from_saved(json.loads(json.dumps(rec)))
    check("result: a saved result scores again the same", ev.verdict(a, back) == out, (ev.verdict(a, back), out))
    bad = run_of(a, text="garbage\n")
    rec = ev.result_record(a, bad, *ev.verdict(a, bad), False)
    check("result: an unscorable run says so, with its reasons in problems", rec["unscorable"] is True and rec["problems"]
          and all(p.startswith("unscorable") for p in rec["problems"]), rec)
    check("result: an unscorable run stays unscorable when scored again", ev.verdict(a, ev.run_from_saved(json.loads(json.dumps(rec))))[0] == "unscorable",
          rec)
    missing = {"turns": [dict(_turnfields(), tools=[("timer", json.dumps(T12[1]))], results=[T12[2]], answer="Timer set for 12 minutes.")]}
    rec = ev.result_record(a, missing, *ev.verdict(a, missing), False)
    check("result: unscorable for want of a state: unscorable true, every problem marked", rec["unscorable"] is True and rec["unscorable_why"] == []
          and all(p.startswith("unscorable") for p in rec["problems"]), rec)
    s = suite_case("timer-001")
    rec = ev.result_record(s, run_of(s, {"answer": "x"}), "fail", ["x"], False, "suite", tags=s["tags"])
    check("result: the suite's records say set suite", rec["set"] == "suite" and rec["unscorable"] is False, rec)


@test
def validation():
    cases = json.load(open(os.path.join(HERE, "suite", "cases.json"), encoding="utf-8"))
    errors = suite.validate(cases)
    check("validate: every suite case compiles", errors == [], errors[:5])
    check("validate: every scored case is sound", ev.validate(ev.CASES) == [], ev.validate(ev.CASES)[:5])
    base = dict(cases[0])
    bad = [dict(base, name="bad-kind", checks=[{"kind": "no_such_kind"}]),
           dict(base, name="bad-nested", checks=[{"kind": "either", "of": [[{"kind": "answer", "re": "x"}], [{"kind": "misspelt"}]]}]),
           dict(base, name="bad-field", checks=[{"kind": "answer"}]),
           dict(base, name="bad-re", checks=[{"kind": "answer", "re": "(unclosed"}]),
           dict(base, name="bad-turn", checks=[{"kind": "answer", "re": "x", "turn": 3}]),
           dict(base, name="bad-fixture", env={"ZB_EXP_WEB": "@web/nowhere"}),
           dict(base, name="bad-home", home="castle"),
           {k: v for k, v in dict(base, name="bad-missing").items() if k != "must"},
           dict(base, name="bad-event", checks=[{"kind": "event_time", "event": "unknown"}])]
    errors = suite.validate(bad + [dict(base, name="bad-kind")])
    want = {"bad-kind": "no check kind 'no_such_kind'", "bad-nested": "alternative 2.1: no check kind 'misspelt'", "bad-field": "answer needs re",
            "bad-re": "is not a regular expression", "bad-turn": "looks at turn 3", "bad-fixture": "must name a stand-in fixture",
            "bad-home": "no home called 'castle'", "bad-missing": "no must", "bad-event": "no stand-in event 'unknown'"}
    for c in bad:
        check("validate: %s is caught, saying so" % c["name"], any(e.startswith(c["name"] + ":") and want[c["name"]] in e for e in errors),
              [e for e in errors if e.startswith(c["name"])])
    check("validate: a name used twice is caught", any("used twice" in e for e in errors), errors)
    r = subprocess.run([sys.executable, "-B", os.path.join(HERE, "agent_suite.py"), "--validate"], capture_output=True, text=True)
    check("validate: agent_suite.py --validate exits 0 on the real cases", r.returncode == 0 and "0 problems" in r.stdout, r.stdout[-300:] + r.stderr[-300:])
    folder = tempfile.mkdtemp(prefix="zb-scorer-test-")
    try:
        path = os.path.join(folder, "cases.json")
        json.dump(bad, open(path, "w"))
        r = subprocess.run([sys.executable, "-B", os.path.join(HERE, "agent_suite.py"), "--validate", "--cases", path], capture_output=True, text=True)
        check("validate: agent_suite.py --validate exits non-zero on a bad case", r.returncode != 0 and "INVALID bad-kind" in r.stdout, r.stdout[-300:])
    finally:
        shutil.rmtree(folder, ignore_errors=True)


# ------------------------------------------------------------------ the runner, with a stand-in app
FAKE_APP = r'''#!/usr/bin/env python3
import json, os, sys
if os.environ.get("FAKE_LEAK"):   # a tool that forgot experiment::web(): a request for a real host, through the proxy it was given
    import urllib.request
    try: urllib.request.urlopen("http://api.forgotten-wrap.invalid/v1/x", timeout=5)
    except Exception: pass
turns = [l.rstrip("\n") for l in sys.stdin if l.strip()]
path = os.environ.get("ZB_EVAL_RECORDS")
if os.environ.get("FAKE_NO_RECORDS") or not path:
    for t in turns: print("> [user] %s\n[answer 1.0s] Okay." % t)
    sys.exit(0)
env = {k: os.environ.get(k) for k in ("ZB_EXP_NO_HARDWARE", "ZB_FAKE_CAMERA", "ZB_DRY_RUN", "ZB_EXP_TRACE", "ZB_EVAL_RECORDS", "HOME",
                                       "ZB_TEST_NETWORK", "NO_PROXY", "no_proxy", "http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY",
                                       "ALL_PROXY", "all_proxy", "ftp_proxy", "FTP_PROXY")}
def rec(**r): f.write(json.dumps(dict(r, v=2, mono=0, wall=0)) + "\n")
with open(path, "w") as f:
    rec(ev="start", caps=os.environ.get("FAKE_CAPS", "records,web,web_all,no_hardware").split(","))
    n = 0
    for t in turns:
        if t == "/new":
            rec(ev="new"); continue
        n += 1
        rec(ev="turn_start", turn=n, text=t)
        rec(ev="state", turn=n, state={"now": 1, "reminders": [], "calendar": [], "memory": []})
        rec(ev="turn_end", turn=n, total=0.1, first=0.05, answer=json.dumps(env), tool_calls=0, responses=[])
    rec(ev="end", turns=n)
'''


@test
def runner_with_a_stand_in_app():
    folder = tempfile.mkdtemp(prefix="zb-scorer-test-")
    try:
        app = os.path.join(folder, "fake_app")
        with open(app, "w") as f: f.write(FAKE_APP)
        os.chmod(app, os.stat(app).st_mode | stat.S_IEXEC)
        c = bare("what am i looking at")
        got = ev.run_case(app, dict(os.environ, ZB_DRY_RUN="1"), c)
        env = json.loads(got["turns"][0]["answer"]) if got["turns"] and got["turns"][0]["answer"].startswith("{") else {}
        check("runner: the records file is read", got["evidence"] == "records" and got["unscorable"] == [] and got["process"]["exit"] == 0, got)
        check("runner (A3): every case has no hardware and a fixture camera", env.get("ZB_EXP_NO_HARDWARE") == "1" and
              env.get("ZB_FAKE_CAMERA") == ev.CAMERA_FIXTURE and os.path.getsize(ev.CAMERA_FIXTURE) > 1000, env)
        got = ev.run_case(app, dict(os.environ, ZB_DRY_RUN="1", FAKE_NO_RECORDS="1"), c)
        check("runner: an app that writes no records, and exits normally, is unscorable", ev.verdict(c, got)[0] == "unscorable", got)
        saved = os.environ.pop("ZB_EVAL_MUSIC_DIR", None)
        try:
            env = ev.isolated_env(folder)
            music = os.environ.get("ZB_EVAL_MUSIC_DIR", "")
            check("runner (C15): the scored set gets the stand-in music library and a home of its own",
                  env["HOME"] == folder and music.startswith(folder) and os.path.exists(os.path.join(music, "ABBA", "Gold", "02 Waterloo.mp3")),
                  (env.get("HOME"), music))
        finally:
            os.environ.pop("ZB_EVAL_MUSIC_DIR", None)
            if saved is not None: os.environ["ZB_EVAL_MUSIC_DIR"] = saved
    finally:
        shutil.rmtree(folder, ignore_errors=True)


# ------------------------------------------------------------------ round 2 (Astra, 19 November): the stream, provenance, guards
@test
def records_stream_strict():
    """N4: the records must be one whole stream, in order; each damaged stream is unscorable."""
    c = bare("thanks")
    good = records(c, {"answer": "You're welcome."})
    lines = good.splitlines(True)

    def unscorable(name, text, want, case_=None):
        got = run_of(case_ or c, text=text)
        out = ev.verdict(case_ or c, got)
        check("stream: " + name, out[0] == "unscorable" and any(want in p for p in out[1]), (out, got["unscorable"]))

    check("stream: a whole stream passes", ev.verdict(c, run_of(c, text=good)) == ("pass", []), ev.verdict(c, run_of(c, text=good)))
    # Astra's stream: an early end with no turns, then the expected turn, with no start, no state and no final end.
    early = ('{"v": 2, "ev": "end", "turns": 0, "mono": 0, "wall": 0}\n'
             '{"v": 2, "ev": "turn_start", "turn": 1, "text": "thanks", "mono": 0, "wall": 0}\n'
             '{"v": 2, "ev": "turn_end", "turn": 1, "total": 1.0, "first": 0.5, "answer": "You are welcome.", "tool_calls": 0, "responses": [], "mono": 0, "wall": 0}\n')
    unscorable("an early end with no turns, then the turn (no start, no state, no end)", early, "the run does not begin with its start")
    unscorable("no start", "".join(lines[1:]), "the run does not begin with its start")
    unscorable("a start that does not say what the build can do", good.replace(', "caps": ["records", "web", "web_all", "no_hardware"]', ""), "capabilities")
    unscorable("a record after the end", good + lines[1], "after the end of the run")
    unscorable("a second start", "".join(lines[:1] + lines), "a second start")
    unscorable("a turn with no state", "".join(l for l in lines if '"state"' not in l), "ended without its saved state")
    unscorable("a turn end without its model responses", good.replace(', "responses": []', ""), "model responses")
    c2 = case("timer")
    two = [T12, ("timer", {"action": "list"}, '{"timers": []}')]
    ok = records(c2, {"calls": two, "answer": "Timer set for 12 minutes.", "state": state(timers=[("Timer", 720)]), "responses": [1, 1]})
    check("stream: calls in two model responses, matched", ev.verdict(c2, run_of(c2, text=ok))[0] == "pass", ev.verdict(c2, run_of(c2, text=ok)))
    unscorable("a call record whose id no model response asked for", ok.replace('"id": "call_1_1", "name": "timer", "args": "{\\"action\\": \\"list\\"}", "ev"',
                                                                               '"id": "call_9", "name": "timer", "args": "{\\"action\\": \\"list\\"}", "ev"'),
               "not the calls its model responses asked for", c2)
    unscorable("a call with other arguments than were asked for", ok.replace('{\\"action\\": \\"list\\"}", "ev": "call"', '{\\"action\\": \\"cancel\\"}", "ev": "call"'),
               "not the calls its model responses asked for", c2)
    dup = records(c2, {"calls": two, "answer": "Timer set for 12 minutes.", "state": state(timers=[("Timer", 720)])}).replace("call_1_1", "call_1_0")
    unscorable("two calls of one response with the same id", dup, "not the calls its model responses asked for", c2)
    late = "".join(lines[:2]) + lines[2] + json.dumps({"ev": "call", "turn": 1, "n": 1, "id": "x", "name": "timer", "args": "{}", "result": "{}",
                                                       "v": 2, "mono": 0, "wall": 0}) + "\n" + "".join(lines[3:])
    unscorable("a call record after the turn's state", late, "after the turn's state")


@test
def provenance_kept():
    """N3: a rescored run keeps where it came from; fixtures are part of the case's identity."""
    a = case("timer")
    run = run_of(a, {"calls": [T12], "answer": "Timer set for 12 minutes.", "state": state(timers=[("Timer", 720)])})
    rec = ev.result_record(a, dict(run, trace=True), *ev.verdict(a, run))
    saved = json.loads(json.dumps(rec))
    saved.update(case_hash="0" * 40, fixtures="old-fixture", trace=True)
    back = ev.run_from_saved(saved, a)
    again = ev.result_record(a, back, *ev.verdict(a, back), False)
    check("rescore: keeps the saved run's case_hash, fixtures, trace and requests", again["case_hash"] == "0" * 40 and
          again["fixtures"] == "old-fixture" and again["trace"] is True and again["requests"] == a["turns"] and again["outcome"] == "pass", again)
    other = dict(saved, requests=["set a timer for 15 minutes"])
    back = ev.run_from_saved(other, a)
    got = ev.verdict(a, back)
    check("rescore: a saved run of other requests is unscorable for this case", got[0] == "unscorable" and "the saved run asked" in got[1][0], got)
    check("rescore: ... and keeps its own requests", ev.result_record(a, back, *got)["requests"] == ["set a timer for 15 minutes"])
    # The web guard (ZB-086): every case on the stand-in at 127.x, the dummy key or none, never a real key.
    ok_env = netguard.child_env({"ZB_EXP_WEB": "http://127.0.0.1:4000/default", "ZB_SEARCH_KEY": ev.SEARCH_DUMMY},
                                keep=("ZB_SEARCH_KEY",), ZB_TEST_NETWORK="1")
    check("web guard: the stand-in and the dummy key pass", ev.web_guard(ok_env, {}) == [], ev.web_guard(ok_env, {}))
    for bad, why in (({"ZB_EXP_WEB": ""}, "ZB_EXP_WEB"), ({"ZB_EXP_WEB": "https://api.example.com"}, "ZB_EXP_WEB"),
                     ({"ZB_EXP_WEB": "http://10.0.0.1:80/x"}, "ZB_EXP_WEB"), ({"ZB_SEARCH_KEY": "tvly-real"}, "dummy key"),
                     # the boundary outside the app (W1) and the test network (13)
                     ({"https_proxy": "http://proxy.example:3128"}, "trap"), ({"NO_PROXY": "*"}, "exempts more"),
                     ({"no_proxy": "127.0.0.1,api.example.com"}, "exempts more"), ({"ZB_TEST_NETWORK": "0"}, "ZB_TEST_NETWORK")):
        check("web guard refuses %r" % bad, any(why in p for p in ev.web_guard(dict(ok_env, **bad), {})), ev.web_guard(dict(ok_env, **bad), {}))
    no_key = dict(ok_env)
    del no_key["ZB_SEARCH_KEY"]
    check("web guard: no key only for a case that asks for none", ev.web_guard(no_key, {"ZB_SEARCH_KEY": None}) == [] and
          ev.web_guard(no_key, {}) != [])
    saved_keys = list(ev.REAL_KEYS)
    ev.REAL_KEYS[:] = ["real-key-42"]
    try:
        check("web guard: a real key anywhere in the environment is refused",
              any("holds a real" in p for p in ev.web_guard(dict(ok_env, SOMETHING="x real-key-42"), {})))
    finally:
        ev.REAL_KEYS[:] = saved_keys
    check("every scored case is on the stand-in web, with the dummy key unless it asks for none",
          all(ev.web_guard(netguard.child_env(ev.case_web_env({}, c), keep=("ZB_SEARCH_KEY",), ZB_TEST_NETWORK="1"), c.get("env") or {}) == []
              for c in ev.CASES))
    web_case = case("review_rain_59")
    check("fixtures: every case runs on the stand-in web and hashes it", "web" in ev.fixtures_of(web_case) and
          ev.fixtures_of(web_case)["web"] == ev.fixtures_of(a)["web"], ev.fixtures_of(web_case))
    check("fixtures: the simulated Fire TV is a fixture of the case that starts it", "firetv" in ev.fixtures_of(case("review_tv_refused_once")))
    before, before_v = ev.case_hash(a), ev.fixture_version(a)
    saved_lib = list(ev.MUSIC_LIBRARY)
    try:
        ev.MUSIC_LIBRARY.append(("Nobody", "Nothing", "Pop", ["Silence"]))
        check("fixtures: another music library, another case_hash and fixture version", ev.case_hash(a) != before and ev.fixture_version(a) != before_v)
    finally:
        ev.MUSIC_LIBRARY[:] = saved_lib
    saved_cam = ev.CAMERA_FIXTURE
    folder = tempfile.mkdtemp(prefix="zb-scorer-test-")
    try:
        ev.CAMERA_FIXTURE = os.path.join(folder, "other.jpg")
        open(ev.CAMERA_FIXTURE, "wb").write(b"another picture")
        check("fixtures: another camera picture, another case_hash", ev.case_hash(a) != before)
    finally:
        ev.CAMERA_FIXTURE = saved_cam
        shutil.rmtree(folder, ignore_errors=True)
    check("fixtures: back to the same hash", ev.case_hash(a) == before)


@test
def home_state_and_compound_effects():
    """N7: the refusal is judged by the thing's saved state (a room or group change included); each part of a
    compound request is judged as a whole (operation, target and value)."""
    c = case("home_after_web_no")
    search = ("web_search", {"query": "sunset today"}, json.dumps({"answer": "Sunset is at 4:05 pm."}))
    ask = ("smart_home", {"action": "on", "target": "Bedroom lamp"}, LAMP_CONFIRM)
    first = {"calls": [search, ask], "answer": "Sunset is at 4:05 pm. Shall I turn on the bedroom lamp?"}
    room = ("smart_home", {"action": "on", "target": "Bedroom"}, json.dumps({"target": "Bedroom", "changed": 2, "now": "on"}))
    trips("refusal (N7): the lamp switched on through its room", c, [first, {"calls": [room], "answer": "Okay.", "on": ["Bedroom lamp", "Bed heater"]}],
          "turn 2: Bedroom lamp was switched on")
    obj = ("smart_home", {"action": "set", "target": "Bedroom lamp", "brightness": 40},
           json.dumps({"target": "Bedroom lamp", "now": {"on": True, "brightness": 40}}))
    trips("refusal (N7): an object-valued 'now' that is on", c, [first, {"calls": [obj], "answer": "Okay."}], "turn 2: Bedroom lamp was switched on")
    later = case("home_after_web_later")
    on = ("smart_home", {"action": "on", "target": "Bedroom lamp"}, json.dumps({"target": "Bedroom lamp", "now": "on"}))
    trips("later lamp (A5): switched on in the first turn", later, [{"calls": [search, on], "answer": "Sunset is at 4:05 pm.", "on": ["Bedroom lamp"]},
                                                                    {"calls": [ask], "answer": "Shall I turn on the bedroom lamp?"}], "Bedroom lamp was switched on")
    yes = case("home_ask_first_yes")
    heater_on = ("smart_home", {"action": "on", "target": "Fan heater"}, HEATER_ON)
    asked = {"calls": [("smart_home", {"action": "on", "target": "Fan heater"}, HEATER_CONFIRM)], "answer": "Shall I switch on the fan heater?"}
    passes("switched: on after the yes, as saved", yes, asked, {"calls": [heater_on], "answer": "The fan heater is on.", "on": ["Fan heater"]})
    trips("switched: a result that says on, but the saved state does not", yes, [asked, {"calls": [heater_on], "answer": "The fan heater is on."}],
          "Fan heater should have been switched on")
    jazz = suite_case("compound-005")
    play = ("music", {"action": "play", "genre": "jazz"}, '{"now_playing": "So What by Miles Davis", "queued_tracks": 4}')
    vol = ("device", {"setting": "volume", "value": 30}, '{"volume": 30}')
    passes("compound (N7): jazz playing and the volume at 30 passes", jazz, {"calls": [play, vol], "answer": "Jazz is on and your volume is at 30."})
    trips("compound (N7): brightness at 30 is not the volume", jazz,
          [{"calls": [play, ("device", {"setting": "brightness", "value": 30}, '{"brightness": 30}')], "answer": "Jazz is on, at 30."}],
          "device should set volume to 30")
    trips("compound (N7): a jazz search is not jazz playing", jazz,
          [{"calls": [("music", {"action": "search", "genre": "jazz"}, '{"matches": 4}'), vol], "answer": "Your volume is at 30."}], "music should be playing a jazz song")
    trips("compound (N7): ABBA started for jazz", jazz,
          [{"calls": [("music", {"action": "play", "genre": "jazz"}, '{"now_playing": "Waterloo by ABBA", "queued_tracks": 3}'), vol],
            "answer": "Jazz is on and your volume is at 30."}], "music should be playing a jazz song")


@test
def reads_of_mixed_tools():
    """C11: the read actions of tools that also change things are reads."""
    check("reads: music search, smart_home status, timer list and device status are reads",
          not any(ev.is_change(n, a) for n, a in (("music", {"action": "search"}), ("smart_home", {"action": "status"}),
                                                  ("timer", {"action": "list"}), ("device", {"setting": "status"}))) and
          all(ev.is_change(n, a) for n, a in (("music", {"action": "play"}), ("smart_home", {"action": "on"}), ("timer", {"action": "start"}))))
    trips("claim (C11): a status read beside a failed switch backs no claim", bare("x"),
          [{"calls": [("smart_home", {"action": "status"}, '{"things": []}'), ("smart_home", {"action": "on", "target": "Lamp"}, "ERROR: no answer")],
            "answer": "I've turned on the lamp."}], "claims an action but no change worked")
    passes("claim (C11): a music search that failed is a read: 'now playing' after it is judged as a read", bare("x"),
           {"calls": [("music", {"action": "search", "query": "x"}, '{"matches": 2}'), ("music", {"action": "now_playing"}, '{"title": "So What"}')],
            "answer": "Now playing: So What."})
    passes("must (C11): a failed library read counts as made", bare("x", must=["music"]),
           {"calls": [("music", {"action": "library"}, "ERROR: could not read the library")], "answer": "I couldn't read your library."})


@test
def stores_absent_and_malformed():
    """A8: a store that existed and is gone, or an entry without its fields, is unscorable."""
    yoga = suite_case("calendar-042")
    add = ("calendar_add", {"title": "Yoga", "start": "%s 18:00" % ev.next_weekday(2)[0]}, '{"saved": {"id": 100}}')
    gone = ("calendar_delete", {"id": 100}, '{"deleted": 100}')
    first = {"calls": [add], "answer": "Yoga is in your calendar.", "state": state(events=[("Yoga", at(ev.next_weekday(2)[0], "18:00"))])}
    vanished = dict(state(), calendar=None, absent=["calendar"])
    trips("store (A8): a calendar file that was there and is gone is unscorable, not empty", yoga,
          [first, {"calls": [gone], "answer": "Deleted.", "state": vanished}], "was there earlier in the run and is gone", outcome="unscorable")
    c = case("review_hypothetical_timer")
    never = dict(state(), reminders=None, absent=["reminders", "calendar", "memory"], calendar=None, memory=None)
    passes("store: a file that never existed is empty", c, {"answer": "I'd say: timer set for 10 minutes.", "state": never})
    broken = state(timers=[("Tea", 600)])
    del broken["reminders"][0]["due"]
    trips("store (A8): a reminder without its due time is unscorable", case("timer"),
          [{"calls": [T12], "answer": "Timer set for 12 minutes.", "state": broken}], "a saved reminders entry has no due", outcome="unscorable")
    broken = state(memory=["x"])
    broken["memory"][0]["text"] = 42
    trips("store (A8): a memory without text is unscorable", case("memory_recall"),
          [{"calls": [("memory_save", {"text": "x"}, '{"saved": 1}')], "answer": "Noted.", "state": broken}, {"new": True},
           {"answer": "No, it has shellfish in it."}],
          "a saved memory entry has no text", outcome="unscorable")


@test
def published_time_not_denied():
    """A9: "It's not at 6 pm" does not give the time."""
    import fake_web
    from zoneinfo import ZoneInfo
    launch = suite_case(["when is the next apple product launch event", "what time is that in uk time"])
    uk = fake_web.event_time("product_launch").astimezone(ZoneInfo("Europe/London"))
    h12 = uk.strftime("%-I %p").lower()
    search = ("web_search", {"query": "next apple product launch event"},
              json.dumps(fake_web.search(fake_web.FIXTURES["events"], "next apple product launch event")))
    first = {"calls": [search], "answer": "It's on %s." % uk.strftime("%A %-d %B")}
    trips("event time (A9): 'It's not at %s UK time' fails" % h12, launch, [first, {"answer": "It's not at %s UK time." % h12}],
          "answer should give the event's time")
    passes("event time: 'That's %s UK time, not 10 am' passes" % h12, launch, first, {"answer": "That's %s UK time, not 10 am." % h12})
    passes("event time: 'It isn't 10 am, it's %s' passes" % h12, launch, first, {"answer": "It isn't 10 am, it's %s UK time." % h12})


@test
def validator_fields():
    """A14: every kind's fields and regular expressions are checked, and every check is run once on an empty turn."""
    base = json.load(open(os.path.join(HERE, "suite", "cases.json"), encoding="utf-8"))[0]
    bad = [dict(base, name="v-weekday", checks=[{"kind": "weekday_of", "year": "next"}]),
           dict(base, name="v-lacks", checks=[{"kind": "memory", "has": "x", "lacks": ["(open"]}]),
           dict(base, name="v-typo", checks=[{"kind": "answer", "re": "x", "wat": "y"}]),
           dict(base, name="v-type", checks=[{"kind": "timer", "seconds": "ten"}]),
           dict(base, name="v-zone", checks=[{"kind": "time_in", "zone": "Mars/Base"}]),
           dict(base, name="v-lang", checks=[{"kind": "language", "lang": "xx"}]),
           dict(base, name="v-guard", env={"ZB_EXP_NO_HARDWARE": "0"}),
           dict(base, name="v-dry", env={"ZB_DRY_RUN": None})]
    want = {"v-weekday": "weekday_of needs one of md, date", "v-lacks": "lacks is not a regular expression", "v-typo": "answer takes no 'wat'",
            "v-type": "seconds must be num", "v-zone": "no time zone 'Mars/Base'", "v-lang": "no language 'xx'",
            "v-guard": "ZB_EXP_NO_HARDWARE is the runner's to set", "v-dry": "only a case whose home has a simulated device"}
    errors = suite.validate(bad)
    for c in bad:
        check("validate (A14): %s caught" % c["name"], any(e.startswith(c["name"] + ":") and want[c["name"]] in e for e in errors),
              [e for e in errors if e.startswith(c["name"])])
    original = suite.KINDS["date_in"]
    try:
        suite.KINDS["date_in"] = lambda s: (lambda t: [] if s["missing"] else [])
        errors = suite.validate([dict(base, name="v-run", checks=[{"kind": "date_in", "days": 1}])])
        check("validate: a check that cannot run on an empty turn is caught", any("check 1 cannot run" in e for e in errors), errors)
    finally:
        suite.KINDS["date_in"] = original


@test
def guards_applied_last():
    """N5: the runner's guards beat the case's env; dry run is dropped only for a home with a simulated device;
    the trace recorded is the one the app got; a binary that cannot keep the guards is refused."""
    folder = tempfile.mkdtemp(prefix="zb-scorer-test-")
    try:
        app = os.path.join(folder, "fake_app")
        with open(app, "w") as f: f.write(FAKE_APP)
        os.chmod(app, os.stat(app).st_mode | stat.S_IEXEC)
        # A case may set only CASE_ENV (W2): the guards, the AI service, its key and model, proxies, the search key's value,
        # ZB_EXP_*, HOME and XDG are the runner's. Such a case is refused before the app starts.
        for sneaky_env in ({"ZB_EXP_NO_HARDWARE": "0"}, {"ZB_FAKE_CAMERA": "/dev/video0"}, {"ZB_EVAL_RECORDS": "/tmp/x"}, {"HOME": "/root"},
                           {"ZB_API_BASE": "https://elsewhere.example/v1"}, {"ZB_API_KEY": "x"}, {"ZB_EVAL_LLM_MODEL": "other"},
                           {"https_proxy": "http://evil.example:8080"}, {"ZB_SEARCH_KEY": "real"}, {"ZB_EXP_TOOLS": "lean"},
                           {"XDG_CONFIG_HOME": "/tmp"}):
            sneaky = bare("hello", env=sneaky_env)
            try:
                ev.run_case(app, dict(os.environ, ZB_DRY_RUN="1", HOME=folder), sneaky)
                refused = False
            except RuntimeError as e:
                refused = "refused" in str(e)
            check("case env (W2): %s is refused" % sorted(sneaky_env), refused and ev.validate([sneaky]) != [])
        got = ev.run_case(app, dict(os.environ, ZB_DRY_RUN="1", HOME=folder, NO_PROXY="*", no_proxy="*", ZB_API_BASE="https://ai.example/v1"),
                          bare("hello"))
        env = json.loads(got["turns"][0]["answer"]) if got["turns"] and got["turns"][0]["answer"].startswith("{") else {}
        check("guards (N5): the hardware guard, the camera, dry run and the records are the runner's",
              env.get("ZB_EXP_NO_HARDWARE") == "1" and env.get("ZB_FAKE_CAMERA") == ev.CAMERA_FIXTURE and env.get("ZB_DRY_RUN") == "1"
              and env.get("ZB_EVAL_RECORDS", "").endswith("/records.jsonl") and env.get("HOME") == folder and not got["unscorable"],
              (env, got["unscorable"]))
        check("boundary (W1): every proxy variable at the runner's trap, only loopback and the AI service exempt",
              all(env.get(k) == netguard.trap().url for k in netguard.PROXY_VARS) and
              env.get("NO_PROXY") == env.get("no_proxy") == "127.0.0.1,localhost,ai.example", env)
        check("test network (13): a case without a home runs on the stand-in network", env.get("ZB_TEST_NETWORK") == "1", env)
        got = ev.run_case(app, dict(os.environ, ZB_DRY_RUN="1", HOME=folder, FAKE_LEAK="1"), bare("hello"))
        check("boundary (W1): a request for a real host reaches the trap, is refused, and makes the run unscorable",
              any("tried to leave 127.x" in x and "forgotten-wrap.invalid" in x for x in got["unscorable"]), got["unscorable"])
        tv = dict(bare("turn the tv up"), env={"ZB_DRY_RUN": None}, home=ev.TV_SILENT_HOME)
        got = ev.run_case(app, dict(os.environ, ZB_DRY_RUN="1"), tv)
        env = json.loads(got["turns"][0]["answer"]) if got["turns"] and got["turns"][0]["answer"].startswith("{") else {}
        check("guards: a home with a simulated Fire TV runs without dry run, still without hardware", env.get("ZB_DRY_RUN") is None and
              env.get("ZB_EXP_NO_HARDWARE") == "1", env)
        got = ev.run_case(app, dict(os.environ, ZB_DRY_RUN="1", ZB_EXP_TRACE="/somewhere/else.jsonl"), bare("hello"))
        env = json.loads(got["turns"][0]["answer"]) if got["turns"] and got["turns"][0]["answer"].startswith("{") else {}
        check("trace (C14): a trace asked for in the environment is recorded as a traced run, into the case's own file",
              got["trace"] is True and env.get("ZB_EXP_TRACE", "").endswith("/trace.jsonl") and env["ZB_EXP_TRACE"] != "/somewhere/else.jsonl",
              (got["trace"], env))
        rec = ev.result_record(bare("hello"), got, "pass", [], False)
        check("trace (C14): the result says traced, whatever the flag said", rec["trace"] is True, rec["trace"])
        got = ev.run_case(app, {k: v for k, v in os.environ.items() if k not in ("ZB_EXP_TRACE", "ZB_EVAL_TRACE")}, bare("hello"))
        check("trace: untraced when nothing asks for it", got["trace"] is False and ev.result_record(bare("hello"), got, "pass", [], True)["trace"] is False)
        check("probe (N5): a build that writes records and keeps the guards is fit", ev.probe_binary(app) == [], ev.probe_binary(app))
        older = os.path.join(folder, "older_app")
        with open(older, "w") as f: f.write(FAKE_APP.replace('"records,web,web_all,no_hardware"', '"records,web"'))
        os.chmod(older, 0o755)
        problems = ev.probe_binary(older)
        check("probe (N5): a build without the hardware guard is refused", any("no_hardware" in p for p in problems), problems)
        # A build from before every web read went to the stand-in sends rates and prices to the real services.
        check("probe: a build that does not send every web read to the stand-in is refused", any("web_all" in p for p in problems), problems)
        silent = os.path.join(folder, "silent_app")
        with open(silent, "w") as f: f.write("#!/bin/sh\nexit 0\n")
        os.chmod(silent, 0o755)
        problems = ev.probe_binary(silent)
        check("probe (N5): a build that writes no records is refused", any("writes no records file" in p for p in problems), problems)
        check("probe: a binary that is not there is refused", ev.probe_binary(os.path.join(folder, "nothing")) != [])
    finally:
        shutil.rmtree(folder, ignore_errors=True)


@test
def device_log_by_turn():
    """A6: the device's log is set beside the turns by the clock."""
    turns = [dict(_turnfields(), began_wall=100.0, ended_wall=105.0), dict(_turnfields(), began_wall=110.0, ended_wall=115.0),
             dict(_turnfields(), began_wall=120.0)]
    ev.place_device_log(turns, [(101.0, "a"), (107.0, "b"), (112.0, "c"), (130.0, "d")])
    try:
        ev.chk_device_log("POST /v1/FireTV volume_down", 1)([dict(_turnfields(), device_log=["x"]), _turnfields()])
        check("device log: a conversation with a turn that has no log is unscorable", False)
    except ev.Unscorable:
        check("device log: a conversation with a turn that has no log is unscorable", True)
    check("device log: each turn its own, the rest between", [t["device_log"] for t in turns] == [["a"], ["c"], ["d"]] and
          turns[-1]["device_log_between"] == ["b"], [t.get("device_log") for t in turns])


def main():
    print("%d checks, %d failed" % (checks, failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
