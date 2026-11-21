#!/usr/bin/env python3
"""Runs a set of spoken-style requests through the command-line agent and scores them.

Each case names the tools that should (or should not) be called, what the tool arguments
and the answer must contain, and optional follow-up turns. Expected dates, times and
amounts are computed when the case runs (from today's date or from what the tools
returned), never hard-coded, so a confident wrong number fails. The script reports
pass/fail, tool calls, timings and averages, so changes to prompts, tool descriptions or
the loop can be compared run against run.

Usage:
    . path/to/env.sh            # sets ZB_API_BASE and ZB_API_KEY (the AI service; nothing else)
    tests/agent_eval.py [--bin build/cli/zero_bot] [--repeat N] [--only substring[,substring]]
                        [--fast 0|1] [--jobs N] [--json out.json] [--rescore out.json] [--trace]

--trace records the experiments' trace (src/agent/experiment.h, cli build) of every turn into
--json: model calls with their timings and token counts, tools, guards. tests/exp_report.py
summarises such files. --validate checks the cases as written and runs nothing.

Side effects are disabled (ZB_DRY_RUN): no music plays, no infrared is sent and no smart-home
device is switched. No case reaches a real web service: every web read a tool makes (search, forecasts,
place names, exchange rates, prices) goes to the stand-in web (tests/mocks/fake_web.py, ZB_EXP_WEB, set
last by run_case), search gets a dummy key, an inherited ZB_SEARCH_KEY never reaches the app, and a run
refuses to start when a case or the binary could still reach a real one (web_guard, probe_binary). Every case starts from an empty calendar, memory and settings, in a home folder
of the run's own with a stand-in music library (MUSIC_LIBRARY), a fixture photo for the camera and
ZB_EXP_NO_HARDWARE (camera and infrared refuse); the smart-home cases start from a made-up home on
the stand-in test network (ZB_TEST_NETWORK). A case that must really reach a device (a TV that does
not answer, on 127.0.0.1) runs without ZB_DRY_RUN, with stand-ins that do nothing for the programs
that would change this computer (volume, sound, camera).

The evidence is the cli build's records file (ZB_EVAL_RECORDS: each turn's start and end, the answer,
every tool call whole, the saved state, errors), checked as it is read; nothing printed can pass for
it. Each run ends "pass", "fail" or "unscorable": evidence that is missing or unreadable (no state for
a check that needs one, a store the app could not read, records out of order, a saved result from
before a field existed) is never a pass. Every result carries case_hash, scorer (SCORER_VERSION) and
trace, so that a comparison knows which results it may set side by side.
"""
import argparse, concurrent.futures, datetime, json, os, re, shutil, socket, subprocess, sys, tempfile, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netguard   # noqa: E402  the network boundary of every child process (tests/netguard.py)

# ---------------------------------------------------------------- helpers for checks
ONES = "zero one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen " \
       "seventeen eighteen nineteen".split()
TENS = "_ _ twenty thirty forty fifty sixty seventy eighty ninety".split()

def words(n):
    """English words for 0..999, the way a speech-friendly answer might spell a number."""
    if n < 20: return ONES[n]
    if n < 100: return TENS[n // 10] + ("" if n % 10 == 0 else "[- ]" + ONES[n % 10])
    return ONES[n // 100] + " hundred" + ("" if n % 100 == 0 else " (and )?" + words(n % 100))

def negative(text, start):
    """True when the number that begins at text[start] carries a minus sign: "-12", "−12", "minus 12"
    (not the "-" of a range such as "10-12" or "10 - 12", nor a "- 12" bullet at the start of a line)."""
    before = text[max(0, start - 12):start]
    if re.search(r"\bminus\s+$", before, re.I): return True
    m = re.search(r"(?:^|[\s(\[:=~/])([-−])(\s?)$", before)
    if not m: return False
    ahead = before[:m.start(1)]
    if re.search(r"\d\s*$", ahead): return False            # 10 - 12, 9 -12: a range
    if m.group(2) and re.search(r"(?:^|\n)\s*$", ahead): return False   # "- 12 apples": a bullet
    return True

def says_int(text, n):
    """True when the answer states the whole number n, as digits or as words, with its sign: "-12" or
    "minus twelve" is not 12, and 12 is not -12."""
    for m in re.finditer(r"(?<![\d.,])%d(?![\d]|[.,]\d)" % abs(n), text):
        if negative(text, m.start()) == (n < 0): return True
    if abs(n) < 1000:
        for m in re.finditer(r"\b" + words(abs(n)) + r"\b", text, re.I):
            if negative(text, m.start()) == (n < 0): return True
    return False

def numbers(text):
    """The numbers in the answer, negative where a minus sign says so."""
    out = []
    for m in re.finditer(r"\d[\d,]*\.?\d*", text):
        x = m.group().replace(",", "")
        if x.strip("."): out.append(-float(x) if negative(text, m.start()) else float(x))
    return out

def says_about(text, value, tol):
    """True when some number in the answer is within tol of value (sign included)."""
    return any(abs(n - value) <= tol for n in numbers(text))

# What follows a clock time and says which half of the day it is in.
HALF_OF_DAY = re.compile(r"\s*(?:(a\.?\s?m\b\.?)|(p\.?\s?m\b\.?)|((?:in the|this|tomorrow) morning)|"
                         r"((?:in the|this|tomorrow) (?:afternoon|evening)|tonight)|(at night))", re.I)

def right_half(text, end, hour):
    """False when an "am", "pm", "in the morning" and so on right after a time disagrees with the 24-hour `hour`."""
    m = HALF_OF_DAY.match(text, end)
    if not m: return True
    if m.group(1) or m.group(3): return hour < 12
    if m.group(2) or m.group(4): return hour >= 12
    return hour >= 18 or hour < 5   # "at night"

# Said of a time, saying it is not that time: "it's not at 6 pm", "rather than 6 pm".
DENIED = re.compile(r"\b(not|never|no|isn'?t|wasn'?t|aren'?t|won'?t|rather than|instead of)\b[^,.;:!?]*$", re.I)

def denied(text, start):
    """True when the words just before text[start], in its own clause, deny it ("not at 6 pm")."""
    return bool(DENIED.search(re.split(r"[,.;:!?]", text[:start])[-1]))

def says_time(text, when, slack_min=2, affirmed=False):
    """True when the answer states the clock time `when` (24-hour or 12-hour), give or take slack. An am or
    pm that is said must be the right one: "3:00 am" is not 15:00. With affirmed, a time its own clause
    denies ("it's not at 6 pm") does not count."""
    ok = lambda m: not (affirmed and denied(text, m.start()))
    for d in range(-slack_min, slack_min + 1):
        t = when + datetime.timedelta(minutes=d)
        h12 = t.hour % 12 or 12
        for pat in (r"\b0?%d[:.]%02d\b" % (t.hour, t.minute), r"\b%d[:.]%02d\b" % (h12, t.minute)):
            if any(right_half(text, m.end(), t.hour) and ok(m) for m in re.finditer(pat, text)): return True
        if t.hour % 12 == 0 and t.minute == 0 and any(ok(m) for m in re.finditer(r"midnight" if t.hour == 0 else r"noon|midday", text, re.I)):
            return True
        if t.minute == 0:
            for m in re.finditer(r"\b(%d|%s)\s*(o['’]?clock\b|(?=a\.?\s?m\b|p\.?\s?m\b|(?:in the|this|tomorrow) (?:morning|afternoon|evening)|tonight|at night))" % (h12, ONES[h12]), text, re.I):
                if right_half(text, m.end(), t.hour) and ok(m): return True
    return False

# The time a check means by "now": while a run is scored, the moment its last turn began (from its records),
# so scoring later (--rescore) judges "what time is it" and "tomorrow" as when the run happened. Runs without
# that time (saved before the records file) use the clock, so rescore them on the day they ran.
SCORING_AT = None
def clock(): return SCORING_AT if SCORING_AT is not None else time.time()
def today(): return datetime.datetime.fromtimestamp(clock())
def now_in(zone):
    from zoneinfo import ZoneInfo
    return datetime.datetime.fromtimestamp(clock(), ZoneInfo(zone))

def next_weekday(wd):
    """Dates 'friday' can reasonably mean today: the next one, or a week on if today is that day."""
    d = today().date()
    ahead = (wd - d.weekday()) % 7
    return [d + datetime.timedelta(days=ahead)] + ([d + datetime.timedelta(days=7)] if ahead == 0 else [])

def stamps(days, hhmm): return ["%s %s" % (d.isoformat(), hhmm) for d in days]

def tomorrow(): return today().date() + datetime.timedelta(days=1)

class Unscorable(Exception):
    """A check needs evidence the run does not have: no saved state, a store the app could not read, a saved
    result from before a field existed. The case is then neither passed nor failed on that check."""


def failed(result):
    """A tool result saying the call did not work, as the app's tools::failed reads it (no result at all, from
    a run cut short, counts as failed too)."""
    return result is None or str(result).startswith("ERROR") or str(result).startswith('{"error"')

def worked(result):
    """The call did what it was asked: no error, and not waiting for the user's yes ("CONFIRM: ...")."""
    return not failed(result) and not str(result).startswith("CONFIRM")

def _args(a):
    try: v = json.loads(a)
    except ValueError: return {}
    return v if isinstance(v, dict) else {}

def _made(turn, name):
    """(arguments, result) of every call to `name` (or any of a|b) in this turn."""
    res = turn.get("results", [])
    return [(_args(a), res[i] if i < len(res) else None) for i, (n, a) in enumerate(turn["tools"]) if n in name.split("|")]

def attempts(turn, name):
    """Parsed arguments of every call to `name` (or any of a|b) in this turn, whatever came of it: for
    checks that something was not even tried."""
    return [a for a, _ in _made(turn, name)]

# Tools that change something. A call to one counts only if it worked; a call that reads (a forecast, a
# search, a rate) counts as made whatever the service answered, since a service that times out or is over
# its quota is not the model's doing, and the answer's checks catch a made-up reply. In a case on the
# stand-in web (env ZB_EXP_WEB) every read must have worked too: there a failed read is a broken fixture
# (a 404 from the stand-in), and must not let a check pass for want of anything to look at.
CHANGES = {"timer", "reminder_set", "reminder_cancel", "calendar_add", "calendar_update", "calendar_delete", "memory_save",
           "memory_delete", "smart_home", "music", "radio", "device", "tv_remote", "breathe", "set_timezone"}

# The actions of those tools that only read: a search of the music library or the smart home's status
# changes nothing, so it counts as made whatever came of it, like any other read.
READS = {"timer": {"list"}, "music": {"search", "now_playing", "library"}, "smart_home": {"status"}, "device": {"status"}}

def is_change(name, args=None):
    """Whether a call to `name` with these arguments (parsed or as sent) changes something."""
    if name not in CHANGES: return False
    a = args if isinstance(args, dict) else _args(args or "")
    return str(a.get("action", a.get("setting", ""))).strip().lower() not in READS.get(name, ())

def counts(turn, name, result, args=None):
    """Whether a call to `name` with this result counts as made (see CHANGES and READS)."""
    # A read counts though it failed (a search with no result is still a search), except a web read, which on the
    # stand-in must work (a failed one is a broken fixture or a failure the case expects, and then it uses "tried"),
    # and any read in a case that names a stand-in fixture.
    return worked(result) or (not is_change(name, args) and not turn.get("strict_reads") and name not in WEB_READS)

def calls(turn, name):
    """Parsed arguments of the calls to `name` (or any of a|b) in this turn that did what they asked: a
    change whose result is an ERROR, or that waits for a yes, did not."""
    return [a for (a, r), n in zip(_made(turn, name), [n for n, _ in turn["tools"] if n in name.split("|")])
            if counts(turn, n, r, a)]

def tried(turn, name):
    """The calls to `name` in this turn and how each went, for a problem's text."""
    return [a if worked(r) else "%s -> %s" % (a, str(r)[:120]) for a, r in _made(turn, name)]

def results(turn, name):
    """Tool results (parsed as JSON where possible) for calls to `name` in this turn."""
    out = []
    for (n, _), r in zip(turn["tools"], turn["results"]):
        if n in name.split("|"):
            try: out.append(json.loads(r))
            except ValueError: out.append(r)
    return out

def arg(turn, name, key, ok, what):
    """Problem text unless some call to `name` that worked has argument `key` accepted by ok()."""
    got = [c.get(key) for c in calls(turn, name)]
    if any(v is not None and ok(v) for v in got): return []
    return ["%s.%s should be %s, got %s" % (name, key, what, tried(turn, name))]

# ---------------------------------------------------------------- the state a turn left
# The cli build records, after each request, what the calendar, reminders (timers among them) and memory
# files hold, and the smart home's things ("home"). A file that does not exist is named in "absent": it
# reads as empty only if it never existed in the run (the parser marks one that disappeared as
# "vanished"); a file the app could not read is named in "unreadable". A check that needs the state of a
# turn without one (a saved run from before the record, a build without it), a store that could not be
# read or that vanished, or an entry without the fields every entry has, is unscorable: never a pass.
def state(turn):
    st = turn.get("state")
    if st is None: raise Unscorable("no saved state was recorded for this turn")
    if not isinstance(st, dict): raise Unscorable("the saved state is not a record: %r" % (st,))
    return st

# What every saved entry has, with its type: one without them is not taken for a valid entry.
STORE_FIELDS = {"reminders": {"text": str, "due": int}, "calendar": {"title": str, "start": int}, "memory": {"text": str},
                "home": {"name": str, "state": dict}}

def _store(st, name):
    if name in (st.get("unreadable") or []): raise Unscorable("the app could not read its %s file" % name)
    if name in (st.get("vanished") or []): raise Unscorable("the %s file was there earlier in the run and is gone" % name)
    if name in (st.get("absent") or []): return []
    if name not in st: raise Unscorable("the saved state has no %s" % name)
    items = st[name]
    if not isinstance(items, list) or not all(isinstance(x, dict) for x in items):
        raise Unscorable("the saved %s is not a list of records" % name)
    for x in items:
        for key, kind in STORE_FIELDS.get(name, {}).items():
            if not isinstance(x.get(key), kind) or isinstance(x.get(key), bool):
                raise Unscorable("a saved %s entry has no %s: %s" % (name, key, json.dumps(x)[:120]))
    return items

def local(ts):
    try: return datetime.datetime.fromtimestamp(int(ts)).strftime("%Y-%m-%d %H:%M")
    except (TypeError, ValueError, OverflowError, OSError): return "?"

def saved_timers(st): return [r for r in _store(st, "reminders") if r.get("kind") == "timer"]
def saved_reminders(st): return [r for r in _store(st, "reminders") if r.get("kind") != "timer"]
def saved_events(st): return list(_store(st, "calendar"))
def saved_memory(st): return [str(m.get("text", "")) for m in _store(st, "memory")]

def timer_length(r):
    """A saved timer's whole length in seconds (time added included)."""
    try: return int(r.get("length") or 0) or int(r["due"]) - int(r["started"])
    except (KeyError, TypeError, ValueError): return -1

def shown_state(st, what="all"):
    """The saved things, briefly, for a problem's text."""
    parts = []
    if what in ("all", "timers"): parts.append("timers %s" % [(r.get("text"), timer_length(r)) for r in saved_timers(st)])
    if what in ("all", "when"):
        parts.append("reminders %s" % [(r.get("text"), local(r.get("due"))) for r in saved_reminders(st)])
        parts.append("events %s" % [(e.get("title"), local(e.get("start")), e.get("repeat") or "") for e in saved_events(st)])
    if what in ("all", "memory"): parts.append("memory %s" % saved_memory(st))
    return "; ".join(parts)

def saved_at(turn, ok, weekly=False):
    """Whether a reminder or calendar event is saved at one of the times in `ok` ("YYYY-MM-DD HH:MM"),
    repeating weekly if asked."""
    st = state(turn)
    whens = [] if weekly else [local(r.get("due")) for r in saved_reminders(st)]
    whens += [local(e.get("start")) for e in saved_events(st) if not weekly or e.get("repeat") == "weekly"]
    return any(w in ok for w in whens)

def at_check(t, tools, ok, what, weekly=False):
    """A call to `tools` that worked for one of the times in ok, and something saved at that time."""
    if not any(str(c.get("at", c.get("start")))[:16] in ok and (not weekly or c.get("repeat") == "weekly") for c in calls(t, tools)):
        return ["%s should be for %s, got %s" % (tools, what, tried(t, tools))]
    if not saved_at(t, ok, weekly):
        return ["nothing is saved for %s: %s" % (what, shown_state(state(t), "when"))]
    return []

def has(s): return lambda v: s in str(v).lower()
def num_is(x): return lambda v: abs(float(v) - x) < 0.01
def one_of(values): return lambda v: str(v)[:16] in values

def answer(pattern, what=None):
    return lambda turn: [] if re.search(pattern, turn["answer"], re.I) else ["answer should mention " + (what or "/" + pattern + "/")]

def not_answer(pattern, what):
    return lambda turn: ["answer must not " + what] if re.search(pattern, turn["answer"], re.I) else []

# ---------------------------------------------------------------- checks that need computing
def chk_christmas(t):
    d = today().date()
    xmas = datetime.date(d.year, 12, 25)
    if xmas < d: xmas = datetime.date(d.year + 1, 12, 25)
    n = (xmas - d).days
    return [] if says_int(t["answer"], n) else ["answer should say %d days" % n]

def chk_weekday(t):
    want = datetime.date(today().year + 1, 3, 3).strftime("%A")
    return [] if re.search(want, t["answer"], re.I) else ["answer should say " + want]

def chk_currency_split(t):
    res = [r for r in results(t, "convert_currency") if isinstance(r, dict) and "rate" in r]
    if not res: return ["no usable convert_currency result"]
    share = 85 * res[0]["rate"] / 3
    return [] if says_money(t["answer"], share, money_tol(share), "GBP") else ["answer should give a third of the converted amount, %.2f" % share]

def chk_two_currencies(t):
    got = {str(r.get("to")): r for r in results(t, "convert_currency") if isinstance(r, dict)}
    problems = []
    for code in ("EUR", "JPY"):
        if code not in got: problems.append("no convert_currency call to " + code)
        elif not says_money(t["answer"], 200 * got[code]["rate"], money_tol(200 * got[code]["rate"]), code):
            problems.append("answer should give %.2f %s" % (200 * got[code]["rate"], code))
    return problems

def chk_tip(t):
    res = [r for r in results(t, "convert_currency") if isinstance(r, dict) and "rate" in r]
    if not res: return ["no usable convert_currency result"]
    want = 64 * 1.15 * res[0]["rate"]
    return [] if says_money(t["answer"], want, money_tol(want), "GBP") else ["answer should give %.2f pounds" % want]

def chk_stock(t):
    res = [r for r in results(t, "stock_price") if isinstance(r, dict) and r.get("price")]
    if not res: return ["no usable stock_price result"]
    # A spoken price is rounded ("about 62,500" for 62,497.44): half a per cent, at least 1.
    price = res[0]["price"]
    return [] if says_about(t["answer"], price, max(1.0, abs(price) * 0.005)) else ["answer should give the price %.2f" % price]

def chk_tokyo(t):
    from zoneinfo import ZoneInfo
    return [] if says_time(t["answer"], now_in("Asia/Tokyo")) else ["answer should give the current time in Tokyo"]

def chk_local_time(t):
    return [] if says_time(t["answer"], today()) else ["answer should give the current local time"]

def chk_flight(t):
    d = today().date()
    year = d.year if d <= datetime.date(d.year, 12, 3) else d.year + 1
    return at_check(t, "calendar_add", ["%d-12-03 06:40" % year], "3 December 06:40")

def _when(x):
    """A saved reminder's or event's time and name."""
    return (local(x.get("due", x.get("start"))), str(x.get("text", x.get("title", ""))))

STOPWORDS = {"a", "an", "the", "my", "to", "for", "with", "and", "of", "at", "on", "in", "me", "remind", "reminder", "about"}

def content_words(text):
    """The words of a name that say what it is, casefolded: "Call the bank" and "call the bank." are the same."""
    return {w for w in re.findall(r"[^\W_]+", str(text).casefold()) if w not in STOPWORDS}

def chk_moved_item(first, then, what, title=None):
    """A whole-conversation check: the first turn saved one reminder or event at a time in first(), and at
    the end exactly one is saved, at a time in then(), the same one: moved, not added beside the old one
    (nor added only after a first turn that saved nothing). "The same" is by name, casefolded content
    words, and the name must be the one asked for: every content word of `title` (default `what`), so
    that moving some other item does not count. Needs the saved state of the first and last turns."""
    want = content_words(title or what)
    def named(name): return want <= content_words(name)
    def check(turns):
        a, z = turns[0], turns[-1]
        problems, name = [], None
        items = [_when(x) for x in saved_reminders(state(a)) + saved_events(state(a))]
        if len(items) != 1 or items[0][0] not in first() or not named(items[0][1]):
            problems.append("turn 1: one %s should be saved for %s, saved %s" % (what, first(), items))
        else: name = items[0][1]
        items = [_when(x) for x in saved_reminders(state(z)) + saved_events(state(z))]
        if len(items) != 1 or items[0][0] not in then() or not named(items[0][1]) or \
           (name is not None and content_words(items[0][1]) != content_words(name)):
            problems.append("at the end exactly one %s%s should be saved, for %s; saved %s" % (what, " called %r" % name if name else "", then(), items))
        return problems
    check.whole = True
    return check

chk_moved = chk_moved_item(lambda: stamps(next_weekday(4), "20:00"), lambda: stamps(next_weekday(4), "20:30"), "dinner", "dinner marta")

def chk_alarm(t):
    return at_check(t, "reminder_set|calendar_add", stamps([tomorrow()], "08:00"), "tomorrow 08:00")

def timer_seconds(c):
    """The length a timer call asks for, in seconds (minutes and seconds may both be given)."""
    try: return float(c.get("hours") or 0) * 3600 + float(c.get("minutes") or 0) * 60 + float(c.get("seconds") or 0)
    except (TypeError, ValueError): return -1

def timer_calls(t, action):
    return [c for c in calls(t, "timer") if str(c.get("action", "")).lower() == action]

def chk_timer(seconds, label=None):
    """A timer started for this long, with a label mentioning `label` when one is given: the call worked,
    and such a timer is saved."""
    def check(t):
        starts = timer_calls(t, "start")
        if not any(abs(timer_seconds(c) - seconds) < 1 and (label is None or label in str(c.get("label", "")).lower())
                   for c in starts):
            return ["timer should start for %d seconds%s, got %s" % (seconds, " with a label about " + label if label else "", tried(t, "timer"))]
        st = state(t)
        if not any(abs(timer_length(r) - seconds) < 2 and (label is None or label in str(r.get("text", "")).lower())
                   for r in saved_timers(st)):
            return ["no %d-second timer%s is saved: %s" % (seconds, " about " + label if label else "", shown_state(st, "timers"))]
        return []
    return check

def chk_timer_action_none(action):
    return lambda t: ["timer should not be called with action %s yet, got %s" % (action, tried(t, "timer"))] if timer_calls(t, action) else []

def chk_timer_action(action):
    return lambda t: [] if timer_calls(t, action) else ["timer should be called with action %s, got %s" % (action, tried(t, "timer"))]

def chk_timers_saved(*lengths):
    """Exactly these timers (lengths in seconds, time added included) are saved after the turn."""
    def check(t):
        st = state(t)
        got = sorted(timer_length(r) for r in saved_timers(st))
        want = sorted(lengths)
        if len(got) == len(want) and all(abs(g - w) < 2 for g, w in zip(got, want)): return []
        return ["the saved timers should be %s seconds long, saved: %s" % (want or "none", shown_state(st, "timers"))]
    return check

def chk_time_left(t):
    """The answer gives the time left that timer list returned (in whole minutes, either way)."""
    lists = [r for r in results(t, "timer") if isinstance(r, dict) and r.get("timers")]
    if not lists: return ["no timer list result with a running timer"]
    left = str(lists[0]["timers"][0].get("time_left", ""))
    m = re.match(r"(\d+) minutes?", left)
    mins = int(m.group(1)) if m else 0
    return [] if any(says_int(t["answer"], n) for n in (mins, mins + 1)) else ["answer should give the time left (%s)" % left]

def chk_cancelled(want, keep):
    """A timer cancel that stopped the `want` timer while the `keep` one was also running, and left that one running."""
    def check(t):
        done = [r for r in results(t, "timer") if isinstance(r, dict) and r.get("cancelled")]
        gone = [str(x).lower() for r in done for x in r["cancelled"]]
        if not any(want in x for x in gone): return ["timer should cancel the %s timer, cancelled %s" % (want, gone)]
        if any(keep in x for x in gone): return ["the %s timer should keep running" % keep]
        # The choice only means something with both running: the result lists what still runs.
        left = [str(x).lower() for r in done for x in r.get("still_running", [])]
        if not any(keep in x for x in left): return ["the %s timer should still be running after the cancel, still running: %s" % (keep, left)]
        st = state(t)
        names = [str(r.get("text", "")).lower() for r in saved_timers(st)]
        if any(want in x for x in names) or not any(keep in x for x in names):
            return ["the saved timers should hold the %s timer and not the %s one: %s" % (keep, want, shown_state(st, "timers"))]
        return []
    return check

def saved_countdown(t, seconds):
    """Whether a timer of this length, or a reminder due this long after the request, is saved."""
    st = state(t)
    if any(abs(timer_length(r) - seconds) < 2 for r in saved_timers(st)): return True
    asked = int(st.get("now", 0)) - (t.get("total") or 0)   # about when the request began
    return any(abs(int(r.get("due", 0)) - asked - seconds) < 180 + (t.get("total") or 0) for r in saved_reminders(st))

def chk_countdown(seconds):
    """Counted down from now: a timer of that length, or a reminder that far away (either is right), that
    worked and is saved."""
    def check(t):
        made = any(abs(timer_seconds(c) - seconds) < 1 for c in timer_calls(t, "start"))
        for c in calls(t, "reminder_set"):
            try:
                if c.get("in_minutes") is not None and abs(float(c["in_minutes"]) * 60 - seconds) < 1: made = True
            except (TypeError, ValueError): pass
            if c.get("at"):
                try: due = datetime.datetime.strptime(str(c["at"])[:16], "%Y-%m-%d %H:%M")
                except ValueError: continue
                if abs((due - today()).total_seconds() - seconds) < 180: made = True
        if not made:
            return ["a timer or reminder should go off %d seconds from now, got %s" % (seconds, tried(t, "timer|reminder_set"))]
        if not saved_countdown(t, seconds):
            return ["nothing is saved to go off %d seconds from now: %s" % (seconds, shown_state(state(t)))]
        return []
    return check

# "In an hour and a half" counts down from now, so a timer is as right as a reminder.
chk_ninety = chk_countdown(5400)

def chk_umbrella(t):
    return at_check(t, "reminder_set|calendar_add", stamps(next_weekday(4), "18:00"), "Friday 18:00")

def chk_spanish(t):
    return [] if re.search(r"\b(grados|hoy|está|hace|lluvia|nublado|despejado|cielo|temperatura|máxima)\b", t["answer"], re.I) \
        else ["answer should be in Spanish"]

# ---------------------------------------------------------------- the cases
def chk_numeric_date(month, day):
    def check(t):
        d = today().date()
        year = d.year if d <= datetime.date(d.year, month, day) else d.year + 1
        return at_check(t, "calendar_add", ["%d-%02d-%02d 10:00" % (year, month, day)], "%02d-%02d 10:00 (month-day)" % (month, day))
    return check

def chk_yearly(month_day, year=None):
    """A calendar_add for an all-day yearly event on month_day ("03-14"), in `year` if one was given."""
    def check(t):
        got = calls(t, "calendar_add")
        for c in got:
            start = str(c.get("start"))[:10]
            if c.get("repeat") == "yearly" and c.get("all_day") is True and start[5:] == month_day and \
               (year is None or start[:4] == str(year) or c.get("since_year") == year):
                st = state(t)
                if not any(e.get("repeat") == "yearly" and local(e.get("start"))[5:10] == month_day for e in saved_events(st)):
                    return ["no yearly event on %s is saved: %s" % (month_day, shown_state(st, "when"))]
                return []
        return ["calendar_add should be all_day, repeat yearly, on %s%s; got %s" % (month_day, " %d" % year if year else "",
                                                                                     tried(t, "calendar_add"))]
    return check

def chk_weekly_tuesday(t):
    return at_check(t, "calendar_add", stamps(next_weekday(1), "07:00"), "next Tuesday 07:00, repeating weekly", weekly=True)

def chk_dad_age(t):
    d = today().date()
    year = d.year if d <= datetime.date(d.year, 6, 2) else d.year + 1
    return [] if says_int(t["answer"], year - 1955) else ["answer should give the age %d" % (year - 1955)]

def case(name, request, must=(), must_not=(), checks=(), env=None, steer=None, home=None, radio=None, quotes_ok=False, tried=(),
         held_out=None):
    """request: one utterance or a list (a conversation; "/new" starts a fresh one, memories are kept).
    must / must_not / checks apply to the last turn; speech-suitability rules apply to every turn.
    must: tools the last turn must call; a change (CHANGES) must also have worked, since the case is about
    its effect. tried: tools it must call whatever came of the call, for routing and for the cases about a
    failure or a question first (a change that waits for the user's yes has not worked yet).
    home: a smart-home file to start from, saved for the stand-in test network (see EVAL_HOME).
    radio: a radio station file to start from (see EVAL_RADIO); without one there are no stations.
    quotes_ok: the answer may quote a confirmation without having acted ("what would you say if..."):
    the rule against claiming an action without a tool call is left out; the saved state is checked instead."""
    # A forecast's figures are what the stand-in served: any temperature the answer gives must be one of them.
    if any("weather" in m.split("|") for m in must): checks = list(checks) + [chk_weather_served]
    out = {"name": name, "turns": [request] if isinstance(request, str) else list(request), "must": list(must),
           "must_not": list(must_not), "checks": list(checks), "env": env or {}, "steer": steer, "home": home,
           "radio": radio, "quotes_ok": quotes_ok, "tried": list(tried)}
    if held_out: out["held_out_turns"] = [held_out] if isinstance(held_out, str) else list(held_out)   # held_out: the
    return out   # held-out question forms, asked instead on a held-out seed (case_turns)

# ---------------------------------------------------------------- a made-up home for the smart-home cases
# Saved for the network ZB_TEST_NETWORK=1 stands in for (its id is checked in tests/unit). Every
# device has kind "tapo" only so that a driver built into the app claims it; with ZB_DRY_RUN (set
# for every case) no device is ever contacted, and status is answered from these saved states.
TEST_NETWORK = "net-58678b031ae6e411"
POWER, BRIGHT, WHITE, COLOUR, TEMP, POSITION, TRANSPORT, VOLUME, SOURCE = 1, 2, 4, 8, 32, 64, 256, 512, 2048
VOLUME_STEP, KEYS = 1024, 4096

def _thing(tid, name, kind, caps, room="", state=None, ask_first=False):
    return {"id": tid, "name": name, "type": kind, "caps": caps, "room": room, "state": state or {}, "ask_first": ask_first,
            "enabled": True, "ref": {}}

def _home(*things):
    devices = [{"id": "tapo:eval%d" % i, "kind": "tapo", "name": t["name"], "last_ip": "127.0.0.1", "port": 9, "enabled": True,
                "things": [t]} for i, t in enumerate(things)]
    return {"version": 2, "secrets": {}, "networks": {TEST_NETWORK: {"label": "Test network", "gateway_mac": "02:00:00:00:00:00",
                                                                       "allowed_at": 1, "last_scan": 1, "devices": devices}}}

EVAL_HOME = _home(
    _thing("1", "Bedroom lamp", "light", POWER | BRIGHT | WHITE | COLOUR, "Bedroom", {"on": False}),
    _thing("2", "Bed heater", "plug", POWER, "Bedroom", {"on": False}),     # used every night: no question first
    _thing("3", "Fan heater", "plug", POWER, "", {"on": False}, ask_first=True),
    _thing("4", "Floor lamp", "light", POWER | BRIGHT | WHITE | COLOUR, "Lounge", {"on": True, "brightness": 60}),
    _thing("5", "Lamp", "light", POWER | BRIGHT, "Lounge", {"on": False}),
    _thing("6", "Lamp", "light", POWER | BRIGHT, "Hall", {"on": False}),
    _thing("7", "Hall thermostat", "thermostat", TEMP, "Hall", {"mode": "heat", "target_c": 19, "current_c": 18.5}),
    _thing("8", "Kitchen speaker", "speaker", TRANSPORT | VOLUME, "Kitchen", {"playing": "playing", "volume": 30}),
    _thing("9", "Kitchen light", "light", POWER | BRIGHT, "Kitchen", {"on": True, "brightness": 100}),
    _thing("10", "Living room TV", "tv", POWER | TRANSPORT | VOLUME | SOURCE, "Living room", {"on": False}))

def _same(got, want):
    if isinstance(want, (int, float)):
        try: return abs(float(got) - want) < 0.01
        except (TypeError, ValueError): return False
    return str(got).strip().lower() == str(want).lower()

def home_call(action, targets=None, **values):
    """Problem text unless some smart_home call in the turn has this action, one of these targets, and these values."""
    def check(t):
        for c in calls(t, "smart_home"):
            target = str(c.get("target", c.get("device", ""))).lower()
            if _same(c.get("action"), action) and (not targets or target in [x.lower() for x in targets]) and \
               all(_same(c.get(k), v) for k, v in values.items()):
                return []
        return ["smart_home should be called with action=%s%s%s, got %s" % (action, " and a target in %s" % targets if targets else "",
                                                                            " and %s" % values if values else "", tried(t, "smart_home"))]
    return check

def home_never_on(t):
    """Nothing switched on: "everything" and unknown things must never be guessed at."""
    bad = [c for c in attempts(t, "smart_home") if str(c.get("action", "")).lower() in ("on", "set")]
    return ["switched something on without a clear target: %s" % bad] if bad else []

# Saying that something is on now ("the fan heater is on", "it's now switched on"), not "it's on the list".
SAYS_ON = r"(?<!not )(?<!n't )\b(is|it'?s|are) (now )?(switched |turned )?on\b(?! (the|your|a|an|my|its|their|this|that)\b)"

def home_asked_about(word=None):
    """The change waited for a yes: the tool answered CONFIRM, nothing was switched, the answer does not
    say it is on, and the user was asked, in a question (a sentence that ends with "?") naming the thing
    (`word`, such as "heater") when one is given."""
    def check(t):
        got = results(t, "smart_home")
        a = t["answer"]
        problems = [] if any(isinstance(r, str) and r.startswith("CONFIRM") for r in got) else ["smart_home never answered CONFIRM"]
        if any(isinstance(r, dict) and r.get("now") == "on" for r in got): problems.append("something was switched on without a yes")
        questions = re.findall(r"[^.!?]*\?", a)
        if not questions: problems.append("the answer should ask the user")
        elif word and not any(re.search(r"\b%s" % re.escape(word), q, re.I) for q in questions):
            problems.append("the question should name the %s" % word)
        if re.search(SAYS_ON, a, re.I): problems.append("the answer says it is on, but the change waits for a yes")
        return problems
    return check

home_asked = home_asked_about()

def thing_on(st, name):
    """Whether the smart home's saved state has the thing `name` switched on (as the app saves it after any
    change: to the thing, its room, a group or everything)."""
    things = [x for x in _store(st, "home") if x["name"].lower() == name.lower()]
    if not things: raise Unscorable("the saved home has no %s" % name)
    return any(x["state"].get("on") is True for x in things)

def _now_on(now):
    return now == "on" or (isinstance(now, dict) and now.get("on") is True)

def home_switched(name):
    """The thing was switched on in this turn: a smart_home change worked, and the saved state shows it on."""
    def check(t):
        if any(isinstance(r, dict) and _now_on(r.get("now")) for r in results(t, "smart_home")) and thing_on(state(t), name): return []
        return ["%s should have been switched on, got %s" % (name, results(t, "smart_home"))]
    return check

def home_not_switched(name):
    """The user declined: whatever the model tried, and whatever it named (the thing, its room, a group,
    everything), the thing is not on after this turn (as every_turn(...): after no turn of the conversation)."""
    def check(t):
        named = [r for r in results(t, "smart_home") if isinstance(r, dict) and r.get("target") == name and _now_on(r.get("now"))]
        if named or thing_on(state(t), name):
            return ["%s was switched on although the user declined: %s" % (name, named or "the saved state shows it on")]
        return []
    return check

def home_all_off(t):
    """Everything was switched off in this turn: the question the model put in its own words, and the yes, counted."""
    return [] if any(isinstance(r, dict) and r.get("target") == "all" and r.get("now") == "off" for r in results(t, "smart_home")) \
        else ["everything should have been switched off after the yes, got %s" % results(t, "smart_home")]

# A Fire TV as it reports itself: power, playback, volume keys only, apps, remote keys.
TV_HOME = _home(
    _thing("1", "Living room TV", "tv", POWER | TRANSPORT | VOLUME_STEP | SOURCE | KEYS, "Living room", {"on": True}),
    _thing("2", "Floor lamp", "light", POWER | BRIGHT, "Living room", {"on": True, "brightness": 60}))

def home_step(sign):
    """A volume_step change on the TV, louder (+1) or quieter (-1)."""
    def check(t):
        steps = [c for c in calls(t, "smart_home") if str(c.get("action", "")).lower() == "set" and c.get("volume_step") is not None]
        try: ok = any(int(c["volume_step"]) * sign > 0 for c in steps)
        except (TypeError, ValueError): ok = False
        return [] if ok else ["smart_home should set volume_step %s 0, got %s" % (">" if sign > 0 else "<", tried(t, "smart_home"))]
    return check

def home_no_action(*actions):
    """No smart_home call in the turn with one of these actions ("turn the TV down" is its volume, not "off")."""
    def check(t):
        bad = [c for c in attempts(t, "smart_home") if str(c.get("action", "")).lower() in actions]
        return ["smart_home should not %s here, got %s" % (" or ".join(actions), bad)] if bad else []
    return check

def no_call(tool):
    return lambda t: ["should not call %s" % tool] if attempts(t, tool) else []

def on_turn(n, check):
    """The check applied to turn n (0 is the first) instead of the last."""
    def f(t): return check(t)
    f.turn = n
    return f

def chk_memory(has, lacks=()):
    """The saved memory after the turn mentions `has` and none of `lacks` (a stale record a lucky answer
    could hide)."""
    def check(t):
        st = state(t)
        mem = " | ".join(saved_memory(st)).lower()
        problems = [] if re.search(has, mem) else ["the saved memory should mention /%s/: %s" % (has, saved_memory(st))]
        return problems + ["the saved memory should no longer mention /%s/: %s" % (x, saved_memory(st)) for x in lacks if re.search(x, mem)]
    return check

def chk_memory_lacks(*patterns):
    """None of these is in the saved memory."""
    def check(t):
        st = state(t)
        mem = " | ".join(saved_memory(st)).lower()
        return ["the saved memory should not mention /%s/: %s" % (x, saved_memory(st)) for x in patterns if re.search(x, mem)]
    return check

def every_turn(check):
    """The check applied to every turn of the conversation: a later turn cannot hide an earlier mistake."""
    def f(t): return check(t)
    f.turn = "all"
    return f

# The same TV, paired over the network, but not answering, for what the assistant does after a
# network error. It is a Fire TV (the driver really runs: these cases set LIVE) at a port on
# 127.0.0.1 that either takes connections and closes them unanswered ("silent": the TV is on the
# network but its remote control service is not replying) or refuses them ("closed": the TV did not
# answer). The token and key are made up; nothing is ever sent beyond this computer.
LIVE = {"ZB_DRY_RUN": None}
_ports, _ports_lock = {}, threading.Lock()

# The stand-in web (tests/mocks/fake_web.py): every web service a tool reads (search, forecasts, place
# names, exchange rates, prices), through the cli build's ZB_EXP_WEB, for every case: run_case sets it
# last, with the fixture a case names ("@web/NAME" in its env; "default" otherwise) and a dummy search key
# (SEARCH_DUMMY; the stand-in takes no other). One stand-in serves the whole run.
def web(fixture="default"):
    return {"ZB_EXP_WEB": "@web/" + fixture}

SEARCH_DUMMY = "stand-in"
WEB = web()
# Which variant of every web answer the stand-in serves (tests/mocks/fake_web.py --seed): one fixed everyday
# seed for every day-to-day run, so that a variant and its baseline see the same world, and a held-out seed
# that only the release profile runs (--held-out), which nothing is ever tuned on. ZB_EVAL_SEED or the
# runners' --web-seed choose another. The seed is part of every case's fixtures, so of its case_hash: results
# of different seeds are never paired.
EVERYDAY_SEED = 1
HELD_OUT_SEED = 7919
HELD_OUT_FROM = 1000          # fake_web.HELD_OUT_FROM: seeds from here on draw the held-out variants
WEB_SEED = int(os.environ.get("ZB_EVAL_SEED") or EVERYDAY_SEED)

def set_web_seed(seed, held_out=False):
    """The seed for this run (before any case starts). A held-out seed only with held_out (the release
    profile's own run), and held_out only with one."""
    global WEB_SEED
    if held_out and os.environ.get("ZB_HELD_OUT_RUN") not in ("release", "calibrate"):
        sys.exit("refused: the held-out run is the release profile's (tests/run.py --profile release, or --groups held-out "
                 "--calibrate N to set its floors); it sets ZB_HELD_OUT_RUN. Never tune on the held-out answers")
    if (seed >= HELD_OUT_FROM) != bool(held_out):
        sys.exit("seed %d %s: held-out seeds (%d and up) are for the release profile's held-out run only (--held-out)"
                 % (seed, "is held out" if seed >= HELD_OUT_FROM else "is not held out", HELD_OUT_FROM))
    if _web: sys.exit("the seed is set before the stand-in web starts")
    WEB_SEED = seed
_web = {}
WEB_LOG = None   # a file for the stand-in's request log (tests/tools_cli.py no_real_web)
WEB_DATE = None  # the run's date, frozen when the stand-in starts (fake_web.py --date) and recorded in each turn and result
# Keys for web services that a run must never pass on (read before isolated_env removes them): web_guard
# refuses a case whose environment holds one anywhere.
WEB_KEYS = ("ZB_SEARCH_KEY",)
REAL_KEYS = [v for v in (os.environ.get(k) for k in WEB_KEYS) if v and v != SEARCH_DUMMY]
STAND_IN = re.compile(r"http://127\.(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)(?:\.(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)){2}(?::\d{1,5})?(?:/[\w.-]*)*")

def case_web_env(env, c):
    """ENV with the web as run_case sets it for case c: the stand-in, and the dummy key unless c asks for none."""
    env = dict(env, ZB_EXP_WEB=web_base() + "/" + web_fixture(c))
    for k in WEB_KEYS:
        if (c.get("env") or {}).get(k, SEARCH_DUMMY) is None: env.pop(k, None)
        else: env[k] = SEARCH_DUMMY
    return env

def preflight(env, cases, validate_errors):
    """Why a live run must not start: a case as written could reach a real web service (VALIDATE_ERRORS, the
    runner's own validation of the cases), or the environment every case starts from holds a real key."""
    problems = list(validate_errors)
    for c in cases:
        e = netguard.child_env(case_web_env(env, c), keep=("ZB_API_BASE", "ZB_API_KEY", "ZB_SEARCH_KEY"),
                               allow_origin=env.get("ZB_API_BASE", ""))
        problems += ["%s: %s" % (c["name"], p) for p in web_guard(e, c.get("env") or {})]
    return problems

WEB_TOOLS = {"web_search", "weather", "convert_currency", "stock_price"}   # the held-out "web" floor's tools
WEB_TAGS = {"search", "weather", "currency", "stock"}
# Reads that go to a web service (the stand-in): a failed one never counts as made. world_time and set_timezone
# look a city up there when given a place rather than a time zone.
WEB_READS = WEB_TOOLS | {"world_time"}

def web_backed(c):
    """Whether a case rests on a web service: it must call (or try) one of WEB_TOOLS, names a stand-in fixture, or
    (a suite case) carries one of WEB_TAGS. The held-out run's "web" floor is over these."""
    tools = {t for m in list(c.get("must", [])) + list(c.get("tried", [])) for t in m.split("|")}
    return bool(tools & WEB_TOOLS) or "ZB_EXP_WEB" in (c.get("env") or {}) or bool(set(c.get("tags") or []) & WEB_TAGS)

def case_turns(c):
    """What the case asks: its held-out question forms ("held_out_turns") on a held-out seed, else its turns."""
    raw = c.get("source", c)
    if WEB_SEED >= HELD_OUT_FROM and (c.get("held_out_turns") or raw.get("held_out_turns")):
        return list(c.get("held_out_turns") or raw.get("held_out_turns"))
    return list(c["turns"])

def web_fixture(c):
    """The stand-in fixture a case names ("@web/NAME" in its env), or "default"."""
    v = (c.get("env") or {}).get("ZB_EXP_WEB")
    return v[5:] if isinstance(v, str) and v.startswith("@web/") and v[5:] else "default"

def web_guard(env, case_env=None):
    """Why this environment could still reach a real web service or carry a real key ([] when it cannot): the
    stand-in must be set to an http://127.x address, the search key must be the dummy or absent (absent only
    when the case asks for no key), and no variable may hold a real key."""
    problems = []
    if not STAND_IN.fullmatch(env.get("ZB_EXP_WEB") or ""):
        problems.append("ZB_EXP_WEB is %r, not the stand-in web on 127.x" % env.get("ZB_EXP_WEB"))
    for k in WEB_KEYS:
        if k in env and env[k] != SEARCH_DUMMY: problems.append("%s is not the stand-in's dummy key" % k)
        elif k not in env and case_env is not None and case_env.get(k, "") is not None:
            problems.append("%s is missing (only a case that asks for no key goes without)" % k)
    for name, value in env.items():
        if any(key in str(value) for key in REAL_KEYS): problems.append("%s holds a real web-service key" % name)
    trap = netguard.trap().url
    if any(env.get(k) != trap for k in netguard.PROXY_VARS):
        problems.append("the proxy variables do not all point at the runner's trap")
    allowed = set(netguard.LOCAL_HOSTS) | {netguard.host_of(env.get("ZB_API_BASE"))}
    for k in netguard.NO_PROXY_VARS:
        if not set(x for x in (env.get(k) or "").split(",") if x) <= allowed:
            problems.append("%s exempts more than 127.0.0.1, localhost and the AI service" % k)
    if env.get("ZB_TEST_NETWORK") != "1": problems.append("ZB_TEST_NETWORK is not set: the real network would be identified")
    if env.get("ZB_EXP_NET") != (netguard.origin_of(env.get("ZB_API_BASE", "")) or "loopback"):
        problems.append("ZB_EXP_NET does not pin the app to 127.x and exactly the AI service's origin")
    return problems

def web_base():
    with _ports_lock:
        if "base" not in _web:
            global WEB_DATE
            WEB_DATE = WEB_DATE or datetime.date.today().isoformat()
            cmd = [sys.executable, "-I", os.path.join(os.path.dirname(os.path.abspath(__file__)), "mocks", "fake_web.py"),
                   "--seed", str(WEB_SEED), "--date", WEB_DATE] + (["--log", WEB_LOG] if WEB_LOG else [])
            # A mock's own environment: nothing of the runner's, so no key of any kind reaches it.
            p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=netguard.mock_env())
            line = p.stdout.readline().split()
            if line[:2] != ["WEB", "listening"]: raise RuntimeError("the stand-in web did not start: %r" % line)
            _web.update(proc=p, base="http://127.0.0.1:%s" % line[2])
        return _web["base"]

def _tv_port(how):
    with _ports_lock:
        if how not in _ports:
            s = socket.socket()
            s.bind(("127.0.0.1", 0))   # bound but not listening: connections are refused, and the port stays ours
            if how == "silent":
                s.listen(16)
                def serve(s=s):
                    while True:
                        try: s.accept()[0].close()
                        except OSError: return
                threading.Thread(target=serve, daemon=True).start()
            _ports[how] = (s, s.getsockname()[1])
        return _ports[how][1]

def _offline_tv_home(how):
    def make():
        port = _tv_port(how)
        home = _home(_thing("2", "Floor lamp", "light", POWER | BRIGHT, "Living room", {"on": True, "brightness": 60}))
        tv = _thing("tv", "Living room TV", "tv", POWER | TRANSPORT | VOLUME_STEP | SOURCE | KEYS, "Living room", {"on": True})
        tv["ref"] = {"volume": True, "power": True}
        home["networks"][TEST_NETWORK]["devices"].insert(0, {
            "id": "fire_tv:eval", "kind": "fire_tv", "name": "Living room TV", "last_ip": "127.0.0.1", "port": port,
            "info": {"dial_port": str(port)}, "auth": {"token": "made-up-token", "key_pin": "sha256//" + "A" * 43 + "="},
            "enabled": True, "things": [tv]})
        return home
    return make

TV_SILENT_HOME, TV_CLOSED_HOME = _offline_tv_home("silent"), _offline_tv_home("closed")

def _refusing_tv_home():
    """A paired Fire TV that answers (tests/mocks/firetv/mock_firetv.py, in this process, on 127.0.0.1) and
    refuses its first remote-control command, then takes the rest; the cases using it set LIVE. Returns
    the home and a function giving the TV's log of commands ((wall-clock time, "METHOD path action")), which
    the runner sets beside each turn's start and end ("device_log" of each turn; "device_log_between" in the
    last turn for any that fell between turns)."""
    import types
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "mocks", "firetv"))
    sys.dont_write_bytecode = True   # no __pycache__ beside the mock
    import mock_firetv
    tv = mock_firetv.Tv(types.SimpleNamespace(state="awake", caps="tv", cert="a", mac=mock_firetv.DEFAULT_MAC, wake_delay=0.9,
                                              boot_delay=7.0))
    tv.tokens.add("evaltoken")
    tv.cfg.update(faults={"POST /v1/FireTV": "500"}, fault_times={"POST /v1/FireTV": 1})
    home = _home(_thing("2", "Floor lamp", "light", POWER | BRIGHT, "Living room", {"on": True, "brightness": 60}))
    t = _thing("tv", "Living room TV", "tv", POWER | TRANSPORT | VOLUME_STEP | SOURCE | KEYS, "Living room", {"on": True})
    t["ref"] = {"volume": True, "power": True}
    home["networks"][TEST_NETWORK]["devices"].insert(0, {
        "id": "fire_tv:eval", "kind": "fire_tv", "name": "Living room TV", "last_ip": "127.0.0.1", "port": tv.api.port,
        "info": {"dial_port": str(tv.dial.port)}, "auth": {"token": "evaltoken", "key_pin": tv.pin()}, "enabled": True, "things": [t]})
    return home, lambda: [(tv.started + e["t"], "%s %s %s" % (e["method"], e["path"], e["query"].get("action", ""))) for e in tv.stats["requests"]]

def chk_device_log(sent, n):
    """A whole-conversation check: the device was sent exactly n requests of this kind ("POST /v1/FireTV
    volume_down"), counting the one it refused, in all (a request between turns included)."""
    def check(turns):
        if any(t.get("device_log") is None for t in turns): raise Unscorable("the device's log of commands was not recorded")
        log = [x for t in turns for x in t["device_log"]] + list(turns[-1].get("device_log_between") or [])
        got = sum(1 for x in log if x == sent)
        return [] if got == n else ["the TV should get %r %d time%s in all, got %d: %s" % (sent, n, "" if n == 1 else "s", got, log)]
    check.whole = True
    return check

def device_sent(sent, n):
    """In this turn the device was sent exactly n requests of this kind (its log, set beside the turn's
    start and end by the clock)."""
    def check(t):
        if t.get("device_log") is None: raise Unscorable("the device's log was not set beside this turn")
        got = sum(1 for x in t["device_log"] if x == sent)
        return [] if got == n else ["the TV should get %r %d time%s in this turn, got %d: %s" % (sent, n, "" if n == 1 else "s", got, t["device_log"])]
    return check
# Saying that it did not work.
FAILED = (r"couldn'?t|could not|can'?t|cannot|unable|didn'?t|did not|isn'?t|is not|wasn'?t|not (replying|responding|answering|reachable)|"
          r"no (answer|response|reply)|offline|unreachable|trouble|problem|failed|off or unplugged")
# Offering the infrared remote: "I can try the infrared remote instead", "point the top edge of the device at the TV".
OFFERS_IR = r"infra.?red|\bIR\b|top (edge )?of (the|this|my) device|point(ing)? (the|this|my) device|remote instead"
NO_IR_OFFER = not_answer(OFFERS_IR, "offer the infrared remote for a TV the smart home lists")

# Twelve sockets: more than eight things, so switching everything off asks first.
SOCKETS_HOME = _home(*[_thing("s%d" % i, "Socket %d" % i, "plug", POWER, "", {"on": True}) for i in range(1, 13)])

FRI_8PM = lambda: stamps(next_weekday(4), "20:00")
CANNOT = r"can'?t|cannot|not able|unable|don'?t have|no way|not something|isn'?t something|not set up|only"

# ---------------------------------------------------------------- made-up radio stations
# Addresses on 127.0.0.1 that nothing answers: with ZB_DRY_RUN (set for every case) no station is
# ever checked or played. The names are made up; none is a real station's.
EVAL_RADIO = {"stations": [
    {"name": "Harbour FM", "nicknames": ["Harbour"], "url": "http://127.0.0.1:9/harbour.mp3"},
    {"name": "Northern Radio 2", "nicknames": ["Radio 2"], "url": "http://127.0.0.1:9/two.m3u8",
     "low_data_url": "http://127.0.0.1:9/two-low.m3u8"},
    {"name": "Night Jazz 24", "nicknames": ["the jazz station", "Jazz 24"], "url": "http://127.0.0.1:9/jazz.aac"},
    {"name": "City Talk", "url": "http://127.0.0.1:9/talk.mp3"}]}

def radio_play(station):
    """Problem text unless the turn asked radio to play exactly `station`."""
    def check(t):
        for c in calls(t, "radio"):
            if str(c.get("action", "play")).lower() == "play" and c.get("station") == station: return []
        return ["radio should play %s, got %s" % (station, tried(t, "radio"))]
    return check

def radio_never_plays(t):
    played = [c for c in attempts(t, "radio") if str(c.get("action", "play")).lower() == "play"]
    return ["radio should not play anything, got %s" % played] if played else []

def stops_player(t):
    if any(str(c.get("action", "")).lower() == "stop" for c in calls(t, "radio|music")): return []
    return ["radio or music should stop, got %s" % calls(t, "radio|music")]

# Saying that radio is not available: "no radio stations are set up", "I can't play radio stations".
RADIO_NOT_SET_UP = (r"not set up|no (radio )?stations|(can'?t|cannot|unable to|not able to|don'?t|do not|no way to)\b.{0,40}"
                    r"\b(radio|stations?|stream|live)|radio (isn'?t|is not|aren'?t|has not|hasn'?t)|without (any )?(radio )?stations|"
                    r"\b(not|no) (the |any )?(radio|live radio|radio stations?|radio streams?)\b")
UNSURE = r"search|can'?t (check|look|confirm)|cannot|not set up|unable|not sure|may have changed|might have changed|" \
         r"check (the|their)|double.check|verify|usually|typically|normally|generally|i believe|from memory|as far as i know"

# ---------------------------------------------------------------- checks of the saved state, for the review's conversations
def chk_timers_named(**want):
    """Exactly these timers are saved, by a word of their label and their whole length in seconds
    (tea=900, toast=1200)."""
    def check(t):
        st = state(t)
        got = [(str(r.get("text", "")).lower(), timer_length(r)) for r in saved_timers(st)]
        ok = len(got) == len(want) and all(sum(1 for name, n in got if word in name and abs(n - secs) < 2) == 1
                                           for word, secs in want.items())
        return [] if ok else ["the saved timers should be %s, saved: %s" % (want, shown_state(st, "timers"))]
    return check

def chk_timer_left_said(word):
    """The answer gives the time left on the `word` timer, in whole minutes either way, worked out from the
    saved state (or, without one, from the turn's timer list)."""
    def check(t):
        lefts = []
        if t.get("state") is not None:
            st = state(t)
            lefts = [(int(r["due"]) - int(st["now"])) / 60.0 for r in saved_timers(st) if word in str(r.get("text", "")).lower()]
        else:
            for r in results(t, "timer"):
                for x in (r.get("timers", []) if isinstance(r, dict) else []):
                    m = re.match(r"(\d+) minutes?", str(x.get("time_left", "")))
                    if word in str(x.get("label", "")).lower() and m: lefts.append(float(m.group(1)))
        if not lefts: return ["no %s timer is left to report on" % word]
        ok = {int(lefts[0]), int(lefts[0]) + 1, round(lefts[0])}
        return [] if any(says_int(t["answer"], n) for n in ok) else ["the answer should give the %s timer's %s minutes left" % (word, "/".join(map(str, sorted(ok))))]
    return check

def chk_saved_when(n, ok=None, mention=None, events_only=False):
    """Exactly n reminders and calendar events are saved (calendar events only, with events_only); with ok,
    each at one of those times, and with mention, each mentioning it."""
    def check(t):
        st = state(t)
        items = [_when(x) for x in ([] if events_only else saved_reminders(st)) + saved_events(st)]
        bad = len(items) != n or any((ok and w not in ok()) or (mention and mention not in name.lower()) for w, name in items)
        return ["%d %s%s should be saved%s%s, saved: %s" % (n, "calendar event" if events_only else "reminder", "" if n == 1 else "s",
                                                            " for %s" % ok() if ok else "", " about %s" % mention if mention else "", items)] if bad else []
    return check

def chk_deadlines(*plan):
    """A whole-conversation check of the saved timers' deadlines: plan gives, for each turn, the timers that
    must be saved after it, by a word of their label, each with how far its deadline has moved since turn 1
    (in seconds); a timer not named must not be saved. So a timer restarted, or one moved that should not
    have been, shows even when the lengths come out right."""
    def check(turns):
        problems, base = [], {}
        for i, want in enumerate(plan):
            st = state(turns[i])
            got = [(str(r.get("text", "")).lower(), int(r.get("due") or 0)) for r in saved_timers(st)]
            for word, moved in want.items():
                mine = [due for name, due in got if word in name]
                if len(mine) != 1:
                    problems.append("turn %d: one %s timer should be saved, saved %s" % (i + 1, word, shown_state(st, "timers")))
                    continue
                if i == 0: base[word] = mine[0]
                elif word in base and abs(mine[0] - base[word] - moved) > 2:
                    problems.append("turn %d: the %s timer's deadline should have moved by %d s since turn 1, it moved by %d s"
                                    % (i + 1, word, moved, mine[0] - base[word]))
            extra = [name for name, _ in got if not any(w in name for w in want)]
            if extra: problems.append("turn %d: no other timer should be saved, saved %s" % (i + 1, extra))
        return problems
    check.whole = True
    return check

def asks_about(word):
    """The answer asks the user something, in a sentence that ends with "?" and names `word`."""
    return lambda t: [] if any(re.search(r"\b%s" % re.escape(word), q, re.I) for q in re.findall(r"[^.!?]*\?", t["answer"])) \
        else ["the answer should ask about the %s" % word]

def chk_forecast_seen(days, rain):
    """The forecast the weather tool gave (a call that worked) holds the stand-in's figure for that day
    (0 today, 1 tomorrow): its rain chance, or none at all when rain is None. Without it the follow-up's
    checks mean nothing."""
    def check(t):
        day = (today().date() + datetime.timedelta(days=days)).isoformat()
        seen = []
        for c, r in _made(t, "weather"):
            if not worked(r): continue
            try: forecast = json.loads(r).get("forecast", [])
            except (ValueError, AttributeError): continue
            for line in forecast:
                if str(line).startswith(day + ":"):
                    m = re.search(r"rain chance (\d+)%", str(line))
                    seen.append(int(m.group(1)) if m else None)
        if rain in seen and len(set(seen)) == 1: return []
        return ["the weather tool should have given %s's rain chance as %s, gave %s" % (day, "none" if rain is None else "%d%%" % rain, seen or tried(t, "weather"))]
    return check

# ---------------------------------------------------------------- what the stand-in web served
def _fake_web():
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "mocks"))
    sys.dont_write_bytecode = True   # no __pycache__ beside the mocks
    import fake_web
    return fake_web

def web_day(t):
    """The run's frozen date (the stand-in's --date), as the turn recorded it; today for runs from before."""
    try: return datetime.date.fromisoformat(t["web_date"])
    except (KeyError, TypeError, ValueError): return today().date()

def annotate_served(c, turns):
    """Each turn's "served": every web search of the conversation so far (this turn's and the earlier ones'), with
    its query, whether it worked, the stand-in rule its query matched and that rule's facts for the run's seed and
    date: what the model saw. Worked out from the recorded calls, so a rescore sees the same."""
    fw = _fake_web()
    fixture = web_fixture(c)
    fx = dict(fw.FIXTURES.get(fixture, {}), name=fixture)
    so_far = []
    # "/new" starts a fresh conversation: what was searched before it is no evidence after it.
    new_before = []
    seen_turns = 0
    for x in (case_turns(c) if c.get("turns") else []):
        if x == "/new": new_before.append(seen_turns)
        else: seen_turns += 1
    for i, t in enumerate(turns):
        if i in new_before: so_far = []
        for a, r in _made(t, "web_search"):
            q = str(a.get("query", ""))
            rule = None if fx.get("search_mode") else fw.search_rule(fx, q)
            entry = {"turn": i, "query": q, "worked": worked(r), "rule": rule["id"] if rule else None, "facts": {}}
            if rule and entry["worked"]:
                try: facts = fw.facts(rule["id"], t.get("web_seed", WEB_SEED), web_day(t))
                except KeyError as e: entry["facts_error"], facts = str(e), {}
                # Only what the model could see: a fact counts when the response it was given shows it.
                body = _visible(r)
                entry["facts"] = {k: v for k, v in facts.items() if fw.fill(fw.shown(v), facts).lower() in body.lower()}
                entry["hidden"] = sorted(k for k in facts if k not in entry["facts"])
            so_far.append(entry)
        t["served"] = list(so_far)

MODEL_RESULT_LIMIT = 6000   # src/agent/agent.cpp kMaxToolResult: bytes of a tool's result the model is given

def _visible(result):
    """A tool's result as the model saw it: cut where the loop cuts it (MODEL_RESULT_LIMIT bytes), as text ("\u20ac"
    and "€" alike)."""
    raw = str(result or "").encode("utf-8")
    if len(raw) > MODEL_RESULT_LIMIT:
        return raw[:MODEL_RESULT_LIMIT].decode("utf-8", "ignore")   # cut mid-way: the text up to there, as it was
    try: return json.dumps(json.loads(result), ensure_ascii=False)
    except (TypeError, ValueError): return str(result or "")

def search_texts(t):
    """Everything the turn's web searches gave the model (summaries, titles, snippets), as one text each."""
    out = []
    for c, r in _made(t, "web_search"):
        if not worked(r): continue
        try: out.append(json.dumps(json.loads(r), ensure_ascii=False))   # "€" and "€" alike
        except (TypeError, ValueError): out.append(str(r))
    return out

def _alternatives(value):
    return [a for a in str(value).split("|") if a and "{" not in a]

def _names(value):
    return r"(?:%s)" % "|".join(re.escape(x) for x in sorted(_alternatives(value), key=len, reverse=True))

def _clock(value):
    """A served time ("9:30 am|09:30") as a datetime today."""
    m = re.search(r"(\d{1,2})(?::(\d\d))?\s*(am|pm)", str(value), re.I) or re.search(r"(\d{1,2}):(\d\d)()", str(value))
    h = int(m.group(1)) % 12 + (12 if m.group(3).lower() == "pm" else 0) if m.group(3) else int(m.group(1))
    return today().replace(hour=h, minute=int(m.group(2) or 0), second=0, microsecond=0)

def _first_number(value):
    return float(re.search(r"\d+(?:\.\d+)?", str(value)).group())

MONTHS = ["january", "february", "march", "april", "may", "june", "july", "august", "september", "october", "november", "december"]

def said(answer, value, how, facts=None, slot=None):
    """Whether the answer states a fact the stand-in served, VALUE being "shown|other|ways" (fake_web.facts):
    "name" any of its ways, as words; "time" its clock time (am or pm right, not denied); "number" its first number,
    give or take 2 per cent (at least 0.5); "date" its weekday or its day and month; "score" its two numbers, with
    the result the right way round (a win is not a loss); "open_closed" open or closed as its last way says, and
    not the other; "winner" the name, and not as the side that lost (FACTS' "loser" beating it)."""
    a = answer
    if how in ("name", "winner"):
        # Said, and not denied in its own clause ("it wasn't Sinner" names Sinner only to deny it).
        if not any(not denied(a, m.start()) for m in re.finditer(r"(?<!\w)%s(?!\w)" % _names(value), a, re.I)): return False
        if how == "winner" and facts and facts.get("loser"):
            w, l = _names(value), _names(facts["loser"])
            reversed_ = (r"(?<!\w)%s(?!\w)\s+(?:\w+\s+){0,2}?(?:beat|beats|defeated|defeats|overcame|won|wins|took|lifted|claimed)\b" % l,
                         r"(?<!\w)%s(?!\w)\s+(?:\w+\s+){0,2}?(?:lost|loses|was beaten|fell|went down)\b" % w,
                         r"\b(?:beating|defeating|over|against)\s+(?:the\s+)?%s(?!\w)" % w)
            if any(re.search(p, a, re.I) for p in reversed_): return False
        return True
    if how == "time":
        return says_time(a, _clock(value), 0, affirmed=True)
    if how == "number":
        x = _first_number(value)
        return says_about(a, x, max(0.5, x * 0.02))
    if how == "date":
        d = re.match(r"(\w+day) (\d{1,2}) (\w+)", str(value).split("|")[0])
        if not d: return False
        wd, day, month = d.group(1), int(d.group(2)), d.group(3)
        return bool(re.search(r"\b%s\b" % wd, a, re.I) or re.search(r"\b%d(?:st|nd|rd|th)?(?:\s+of)?\s+%s\b|\b%s\s+%d(?:st|nd|rd|th)?\b"
                                                                   % (day, month, month, day), a, re.I))
    if how == "score":
        shown = str(value).split("|")[0]
        x, y = re.search(r"(\d+)\s*-\s*(\d+)", str(value).split("|")[-1]).groups()
        num = lambda n: r"(?:%s|%s)" % (n, words(int(n)))
        pair = lambda p, q: r"\b%s\s*(?:-|–|—|to|:|\s)\s*%s\b" % (num(p), num(q))
        if not (re.search(pair(x, y), a, re.I) or re.search(pair(y, x), a, re.I)): return False
        rival = re.escape((facts or {}).get("opponent", "")) or r"(?!x)x"
        team = r"(?:%s)" % "|".join(re.escape(x) for x in ((facts or {}).get("_team") or "they").split("|") + ["they", "we"])
        win = r"(?:beat|beats|won|win|wins|defeated|defeats|overcame|edged|thrashed|saw off)"
        lose = r"(?:lost|lose|loses|losing|went down|fell|were beaten|was beaten|were defeated|was defeated)"
        near = r"(?<!\w)%s(?!\w)[^.,;!?]{0,25}?\b%s\b"
        sc = r"\d+\s*(?:-|–|—|to)\s*\d+"
        team_won = re.search(near % (team, win), a, re.I) or re.search(r"\b(?:victory|win)\s+(?:for|over)\b|%s\s+(?:to|for)\s+(?:the\s+)?%s(?!\w)" % (sc, team), a, re.I) \
            or re.search(r"\b(?:beating|defeating)\s+(?:the\s+)?%s(?!\w)" % rival, a, re.I)
        team_lost = re.search(near % (team, lose), a, re.I) or re.search(near % (rival, win), a, re.I) or \
            re.search(r"\b(?:loss|defeat)\s+(?:to|against)\s+(?:the\s+)?%s(?!\w)|\bbeaten by\b|%s\s+(?:to|for)\s+(?:the\s+)?%s(?!\w)" % (rival, sc, rival), a, re.I)
        drew = re.search(r"\b(?:drew|draw|drawn|tied|all square|shared the points|ended level|finished level)\b", a, re.I)
        if shown.startswith("beat"): return bool(team_won) and not team_lost and not drew
        if shown.startswith("lost"): return bool(team_lost) and not team_won and not drew
        if shown.startswith("drew"): return bool(drew) and not team_won and not team_lost
        return True
    if how == "open_closed":
        closed = r"\bclosed\b|\bnot open\b|\bisn'?t open\b|\bshut\b"
        if str(value).split("|")[-1] == "closed": return bool(re.search(closed, a, re.I))
        return bool(re.search(r"\bopen\b", a, re.I)) and not re.search(closed, a, re.I)
    raise ValueError("no way to read %r" % how)

def _says_other(answer, rule, slot, value, how, day, seed=None):
    """Another value this slot can take (fake_web.candidates), said beside the served one: the answer hedges or
    contradicts itself ("Sinner won. Or Alcaraz"), or gives what the search did not."""
    if how not in ("name", "winner", "time", "number"): return None
    fw = _fake_web()
    for other in fw.candidates(rule, slot, day, WEB_SEED if seed is None else seed):
        if fw.shown(other) == fw.shown(value) or set(_alternatives(other)) & set(_alternatives(value)): continue
        # Said, and not denied in its own clause ("London, not New York" names New York only to set it aside).
        if how in ("name", "winner") and any(not denied(answer, m.start()) for m in re.finditer(r"(?<!\w)%s(?!\w)" % _names(other), answer, re.I)):
            return fw.shown(other)
        if how == "time" and says_time(answer, _clock(other), 0, affirmed=True): return fw.shown(other)
        if how == "number" and abs(_first_number(other) - _first_number(value)) > max(0.5, _first_number(value) * 0.02) and \
                says_about(answer, _first_number(other), 0.05): return fw.shown(other)
    return None

def chk_served(rule, slots, how="name", any_of=False):
    """The answer gives what the stand-in web's search served for RULE (fake_web.facts for the run's seed and date):
    the value of each of SLOTS (or of any one, with any_of), read as HOW (said), and no other value the slot can take.
    The evidence is required: a search in this turn or an earlier one ("served", annotate_served) whose query
    matched RULE and worked. Without it the answer cannot rest on what was served: no search at all, or a search for
    something else, fails; a search about RULE's subject that the stand-in matched to another rule (or to none) is a
    fault of the fixture, and the run is unscorable."""
    slots = [slots] if isinstance(slots, str) else list(slots)
    def check(t):
        fw = _fake_web()
        served = t.get("served")
        if served is None:   # a turn scored without the annotation (a hand-made one): this turn's searches only
            annotate_served({"env": {}}, [t])
            served = t["served"]
        mine = [e for e in served if e["rule"] == rule and e["worked"]]
        if not mine:
            about = fw.ALL_RULES[rule].get("about") or fw.ALL_RULES[rule]["match"]
            on_subject = [e for e in served if re.search(about, e["query"], re.I)]
            if on_subject and any(e["worked"] for e in on_subject):
                raise Unscorable("fixture: the search %r is about %s, but the stand-in matched it to %s (widen its rules)"
                                 % (on_subject[0]["query"], rule, on_subject[0]["rule"] or "nothing"))
            if any(not e["worked"] for e in served):
                return ["the search failed, so the answer cannot rest on what it found"]
            if served:
                return ["the search was for something else (%r), not what was asked" % served[0]["query"]]
            return ["no search: the answer cannot rest on what the search found"]
        facts = {}
        for e in mine: facts.update(e["facts"])   # what any search for it showed
        unseen = [x for x in slots if x not in facts]
        if unseen and (any_of and len(unseen) == len(slots) or not any_of):
            # The search showed none of it: the right answer says so, and gives no value of its own.
            invented = [fw.shown(v) for x in unseen for v in fw.candidates(rule, x, web_day(t), t.get("web_seed", WEB_SEED))
                        if re.search(r"(?<!\w)%s(?!\w)" % _names(v), t["answer"], re.I)]
            if invented: return ["the search did not show the %s, but the answer gives one: %s" % ("/".join(unseen), invented[0])]
            if re.search(NOT_FOUND + r"|didn'?t (say|show|give|mention)|doesn'?t (say|show|give|mention)|no (word|mention)", t["answer"], re.I):
                return []
            return ["the search did not show the %s: the answer should say so" % "/".join(unseen)]
        facts = dict(facts, _team=fw.ALL_RULES[rule].get("team", ""))
        hits, problems = [], []
        for slot in slots:
            if slot not in facts: continue
            if said(t["answer"], facts[slot], how, facts, slot):
                # Another value of the slot said too (not for "any of" several topics, whose keywords overlap)
                other = None if any_of else _says_other(t["answer"], rule, slot, facts[slot], how, web_day(t), t.get("web_seed", WEB_SEED))
                if other: problems.append("the answer also gives %r, which the search did not" % other)
                else: hits.append(slot)
        if problems: return problems
        if (hits if any_of else len(hits) == len([x for x in slots if x in facts])): return []
        missing = [fw.fill(fw.shown(facts[x]), facts) for x in slots if x not in hits]
        return ["the answer should give what the search found%s: %s" % (" (one of)" if any_of else "", "; ".join(missing))]
    check.rule = rule   # the rule it reads (date_workload: a rule that counts days from today)
    return check

TEMPERATURE = re.compile(r"(-?\d+(?:[.,]\d+)?)\s*(?:°|degrees?\b|grados\b|graus\b|grad\b|degr[ée]s\b|gradi\b|graden\b)"
                         r"(?:\s*(?:(f\b|fahrenheit)|c\b|celsius|centigrade))?", re.I)
# A range of temperatures: "11 to 17 degrees", "between 8,5 y 20,8 grados", "6–17°C": both ends are temperatures.
TEMP_RANGE = re.compile(r"(-?\d+(?:[.,]\d+)?)\s*(?:°\s*[cf]?|degrees?)?\s*(?:to|-|–|—|and|y|und|et|e|en|a|bis|à)\s*(-?\d+(?:[.,]\d+)?)\s*"
                        r"(?:°|degrees?\b|grados\b|graus\b|grad\b|degr[ée]s\b|gradi\b|graden\b)(?:\s*(?:(f\b|fahrenheit)|c\b|celsius|centigrade))?", re.I)
DAY_WORDS = {0: r"\b(today|tonight|now|right now|at the moment|currently|outside|hoy|heute|aujourd'hui|oggi|hoje|vandaag)\b",
             1: r"\b(tomorrow|mañana|morgen|demain|domani|amanhã)\b"}
WEEKDAYS = ["monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"]

def _served_weather(t):
    """{date: (low, high)} and the current temperatures from the forecasts this turn served."""
    days, now = {}, []
    for c, r in _made(t, "weather"):
        if not worked(r): continue
        try: j = json.loads(r)
        except (TypeError, ValueError): continue
        m = re.search(r"(-?\d+(?:\.\d+)?)[CF] \(feels like (-?\d+(?:\.\d+)?)[CF]\)", str(j.get("now", "")))
        if m: now += [float(m.group(1)), float(m.group(2))]
        for line in j.get("forecast", []):
            d = re.match(r"(\d{4}-\d\d-\d\d): .*?, (-?[\d.]+|\?) to (-?[\d.]+|\?)[CF]", str(line))
            if d: days[d.group(1)] = tuple(float(x) for x in d.groups()[1:] if x != "?")
    return days, now

def _asked_days(request, day0):
    """The dates a weather request asks about (None: any day it was given)."""
    r = (request or "").lower()
    if re.search(DAY_WORDS[1], r): return [(day0 + datetime.timedelta(days=1)).isoformat()]
    if re.search(r"weekend", r):
        return [(day0 + datetime.timedelta(days=(wd - day0.weekday()) % 7)).isoformat() for wd in (5, 6)]
    for wd, name in enumerate(WEEKDAYS):
        if re.search(r"\b%s\b" % name, r): return [(day0 + datetime.timedelta(days=(wd - day0.weekday()) % 7)).isoformat()]
    if re.search(DAY_WORDS[0], r): return [day0.isoformat()]
    return None

# Weekday names in the languages the suite asks in (English first; Spanish, German, French, Italian, Portuguese, Dutch).
WEEKDAY_NAMES = [r"monday|lunes|montag|lundi|luned[iì]|segunda|maandag", r"tuesday|martes|dienstag|mardi|marted[iì]|ter[cç]a|dinsdag",
                 r"wednesday|mi[eé]rcoles|mittwoch|mercredi|mercoled[iì]|quarta|woensdag", r"thursday|jueves|donnerstag|jeudi|gioved[iì]|quinta|donderdag",
                 r"friday|viernes|freitag|vendredi|venerd[iì]|sexta|vrijdag", r"saturday|s[aá]bado|samstag|samedi|sabato|zaterdag",
                 r"sunday|domingo|sonntag|dimanche|domenica|zondag"]

def _day_mentions(text, d0):
    """(start, end, dates, today) for every day an answer names: today or now, tomorrow, a weekday (today's weekday may
    be today or a week on), the weekend, or a date written out ("27 November", "November 27th")."""
    out = [(m.start(), m.end(), {d0.isoformat()}, True) for m in re.finditer(DAY_WORDS[0], text, re.I)]
    out += [(m.start(), m.end(), {(d0 + datetime.timedelta(days=1)).isoformat()}, False) for m in re.finditer(DAY_WORDS[1], text, re.I)]
    for wd, names in enumerate(WEEKDAY_NAMES):
        ahead = (wd - d0.weekday()) % 7
        ds = {(d0 + datetime.timedelta(days=ahead)).isoformat()} | ({(d0 + datetime.timedelta(days=7)).isoformat()} if ahead == 0 else set())
        out += [(m.start(), m.end(), ds, ahead == 0) for m in re.finditer(r"(?<!\w)(?:%s)(?!\w)" % names, text, re.I)]
    for m in re.finditer(r"\b(\d{1,2})(?:st|nd|rd|th)?(?:\s+of)?\s+(%s)\b|\b(%s)\s+(\d{1,2})(?:st|nd|rd|th)?\b" % ("|".join(MONTHS), "|".join(MONTHS)), text, re.I):
        day_n, month = (int(m.group(1)), m.group(2)) if m.group(1) else (int(m.group(4)), m.group(3))
        for year in (d0.year, d0.year + 1):
            try: d = datetime.date(year, MONTHS.index(month.lower()) + 1, day_n)
            except ValueError: break
            if d >= d0:
                out.append((m.start(), m.end(), {d.isoformat()}, d == d0))
                break
    weekend = {(d0 + datetime.timedelta(days=(wd - d0.weekday()) % 7)) for wd in (5, 6)}
    out += [(m.start(), m.end(), {d.isoformat() for d in weekend}, d0 in weekend) for m in re.finditer(r"\bweekend\b", text, re.I)]
    return sorted(out)

def _day_of(text, pos, mentions):
    """The day mention a temperature at POS belongs to: the nearest in its own clause (before or after it: "18
    today and 12 tomorrow"), else the last one before it (a later clause goes on about the same day), else None."""
    start = max([text.rfind(ch, 0, pos) for ch in ".;!?\n,"]) + 1
    ends = [k for k in (text.find(ch, pos) for ch in ".;!?\n,") if k >= 0]
    end = min(ends) if ends else len(text)
    inside = [m for m in mentions if m[0] >= start and m[1] <= end]
    if inside:
        return min(inside, key=lambda m: min(abs(m[0] - pos), abs(m[1] - pos)))
    before = [m for m in mentions if m[1] <= pos]
    return before[-1] if before else None

def chk_weather_served(t):
    """Every temperature the answer gives (both ends of a range) is one the forecast served for the day it is said
    of: the day named in its clause (or the last day named before it), else the day the question asked about, else
    any day; that day's low or high, and the current reading too for today (or no day at all), within 1 degree for
    rounding (°F converted). So today's and tomorrow's highs swapped fail, a rain chance is no temperature, and with
    no forecast any temperature is made up."""
    days, now = _served_weather(t)
    d0, a = web_day(t), t["answer"]
    asked = _asked_days(t.get("request"), d0)
    mentions = _day_mentions(a, d0)

    def allowed_for(dates, today):
        vals = [x for d, v in days.items() if dates is None or d in dates for x in v]
        return vals + (now if today else [])
    numbers_ = []
    for m in TEMP_RANGE.finditer(a):
        numbers_ += [(m.group(1), m.start(1), m.group(3)), (m.group(2), m.start(2), m.group(3))]
    covered = {p for _, p, _ in numbers_}
    numbers_ += [(m.group(1), m.start(1), m.group(2)) for m in TEMPERATURE.finditer(a) if m.start(1) not in covered]
    made_up = []
    for text, pos, fahrenheit in numbers_:
        v = float(text.replace(",", "."))
        if v >= 0 and negative(a, pos): v = -v
        if fahrenheit:   # "62°F" for a served 16.8°C: compared in Celsius, within the same rounding
            v = (v - 32) * 5 / 9
        said_of = _day_of(a, pos, mentions)
        if said_of: allowed = allowed_for(said_of[2], said_of[3])
        else: allowed = allowed_for(asked, asked is None or d0.isoformat() in (asked or []))
        if not any(abs(v - x) <= (1.2 if fahrenheit else 1.0) for x in allowed):
            made_up.append(text + ("°F" if fahrenheit else "") + (" (%s)" % a[said_of[0]:said_of[1]] if said_of else ""))
    if made_up:
        return ["the answer gives a temperature the forecast did not serve for the day it is said of: %s" % ", ".join(made_up)]
    return []

# The words, codes and signs a currency is given by (and the languages the suite asks in).
CURRENCY_WORDS = {"GBP": r"pounds?|quid|sterling|£|gbp|pfund|libras?|livres?|sterlin[ae]|pond", "EUR": r"euros?|€|eur",
                  "USD": r"(?:us )?dollars?|bucks|\$|usd|dólar(?:es)?|dollar", "JPY": r"yen|¥|jpy", "CHF": r"(?:swiss )?francs?|chf|franken",
                  "CAD": r"canadian dollars?|cad|c\$|dollars?", "AUD": r"australian dollars?|aud|a\$|dollars?"}

def says_money(text, value, tol, code=None):
    """The answer gives an amount near VALUE: as a number, as "23 pounds 72 pence", or (10 and over) rounded to the
    whole unit, as speech rounds it ("about 17 pounds" for 17.12). With CODE, that number is given in that currency:
    its name, code or sign right after it (or the sign right before), so "17 euros" is not 17 pounds."""
    text = re.sub(r"(\d),(\d{1,2})\b(?![.,]?\d)", r"\1.\2", text)   # "42,20 Pfund" is 42.20 (a decimal comma)
    text = re.sub(r"\b(\d+) (pounds?|dollars?|euros?|quid|bucks)(?:,? and)? (?:(\d{1,2}) ?(?:pence|p|cents?)|(\d{2}))\b(?![.,]\d)",
                  lambda m: "%s.%02d %s" % (m.group(1), int(m.group(3) or m.group(4)), m.group(2)), text, flags=re.I)
    words_ = CURRENCY_WORDS.get(code or "", "")
    # An amount in words, as speech gives it: "forty-two dollars thirty-three cents", "seventeen pounds".
    whole, cents = int(abs(value)), int(round((abs(value) - int(abs(value))) * 100))
    if cents == 100: whole, cents = whole + 1, 0
    unit = r"(?:%s)" % (words_ or r"pounds?|dollars?|euros?|yen|francs?|quid|bucks")
    for w, c in ((whole, cents), (int(round(abs(value))), None)):
        if w >= 1000: continue
        spoken = r"\b%s\s+%s\b" % (words(w), unit)
        if c is not None and c:
            spoken += r"(?:,?\s+(?:and\s+)?%s(?:\s+(?:cents?|pence|p|centimes|cent))?)" % words(c)
        elif c is None and abs(abs(value) - w) > max(tol, 0.5):
            continue
        if re.search(spoken, text, re.I) and (c is not None or abs(value) >= 10): return True
    for m in re.finditer(r"\d[\d,]*\.?\d*", text):
        x = m.group().replace(",", "")
        try: v = float(x)
        except ValueError: continue
        if not (abs(v - value) <= tol or (abs(value) >= 10 and float(v).is_integer() and v == round(value))): continue
        if not words_: return True
        after, before = text[m.end():m.end() + 30], text[max(0, m.start() - 3):m.start()]
        if re.match(r"\s*(?:%s)(?!\w)" % words_, after, re.I) or re.search(r"(?:£|€|\$|¥)\s*$", before) and \
                re.search(r"(?:%s)" % words_, before, re.I):
            return True
        every = "|".join(CURRENCY_WORDS.values())
        if re.match(r"\s*(?:%s)(?!\w)" % every, after, re.I) or re.search(r"(?:£|€|\$|¥)\s*$", before):
            continue   # labelled, and not with the target's name or sign (that was accepted above)
        # A bare number: it takes the currency of the last amount given with one before it ("71.80 pounds ... 23.93
        # each"), never of a currency merely mentioned ("the pound is weak; 20 euros gets you 17.12").
        last = None
        for lm in re.finditer(r"(?:(?:£|€|\$|¥)\s*\d[\d,]*\.?\d*)|(?:\d[\d,]*\.?\d*\s*(?:%s)(?!\w))" % every, text[:m.start()], re.I):
            last = lm.group()
        if last and re.search(r"(?<!\w)(?:%s)(?![a-z])" % words_, last, re.I):
            return True
    return False

def money_tol(value):
    """A spoken amount is rounded ("about 62 pounds" for 62.17): 0.6 per cent, at least 6 pence."""
    return max(0.06, abs(value) * 0.006)

# Not found, could not look it up, or only from memory.
NOT_FOUND = r"couldn'?t (find|get|fetch|look|check|access|retrieve|load|reach|confirm|see|pull)|" \
            r"could not (find|get|fetch|look|check|access|retrieve|load|reach|confirm|see|pull)|" \
            r"can'?t (find|get|check|access|confirm|see)|cannot (find|get|check|access|confirm)|unable|no (results|information|details|luck|reliable)|" \
            r"didn'?t (find|return|turn up|come up|get)|did not (find|return|get)|nothing (came up|turned up|useful|relevant|about|on)|not (find|sure|available|working)|" \
            r"isn'?t (working|available|responding)|(search|service|lookup|forecast)\b.{0,40}(down|unavailable|error|problem|trouble|fail)|trouble|problem|" \
            r"check (the|their|its) (official|website|site)|from memory|as far as i know|i believe|usually|typically|normally|generally"

def chk_conflict(t):
    """Two sources disagree (the stand-in's "conflict" fixture: its two opening times, for the run's seed): the
    answer gives both, or says the sources differ or that it should be checked; one time stated as fact is a guess."""
    a = t["answer"]
    if t.get("served") is None: annotate_served({"env": {}}, [t])
    seen = {}
    for e in t["served"]:
        if e["rule"] == "louvre-conflict": seen.update(e["facts"])   # only the times the model's results showed
    both = len(seen) == 2 and all(says_time(a, _clock(seen[k]), 0) for k in ("open_a", "open_b"))
    hedge = re.search(r"differ|conflict|disagree|vary|varies|inconsistent|one source|another source|some sources|other sources|sources (say|give|list)|"
                      r"either|not (entirely |completely )?(sure|certain|clear|consistent)|double.check|check (the|their|its|with)|confirm|verify", a, re.I)
    return [] if both or hedge else ["the answer should give both times the sources gave, or say they disagree"]

def chk_stale(t):
    """Only last year's result was found: the answer says it is last year's (or that this year's was not found), and
    no sentence claims it, or anything, for this year ("he won last year and this year" contradicts itself)."""
    year = web_day(t).year
    a = t["answer"]
    if not re.search(r"\b%d\b|last year|previous (year|edition|championships?)|" % (year - 1) + NOT_FOUND, a, re.I):
        return ["the answer should say the result found is last year's (%d), or that this year's was not found" % (year - 1)]
    for sentence in re.split(r"(?<=[.!?;])\s+|,\s*(?:and|but)\s+|\band\b", a):
        if re.search(r"\bthis year\b|\b%d\b" % year, sentence, re.I) and not re.search(
                NEGATION + "|" + NOT_FOUND + r"|yet|not been|hasn'?t|haven'?t|no (result|winner)|unknown|isn'?t (out|known)|until|when|if", sentence, re.I):
            return ["the answer claims a result for this year (%d), which the search did not give: %r" % (year, sentence.strip())]
    return []

# "Nothing was set": the negation governs the setting ("I didn't set a reminder", "no reminder was saved",
# "nothing"), and no sentence states a reminder at 8 without one.
NEGATION = r"\b(didn'?t|did not|haven'?t|have not|hasn'?t|has not|not|no|nothing|won'?t|wasn'?t|isn'?t|never|none)\b"
NOTHING_SET = NEGATION + r"\W+(?:\w+\W+){0,3}?(set|remind\w*|saved?|scheduled|added)\b|\bset nothing\b|^\W*(nothing|none)\b"

# A part of the answer that says a reminder exists: "your reminder for 8 is set", "I'll remind you at 8".
REMINDER_STATED = r"\b(8|eight)\b|\b(is|was|has been|are|got) (now )?(set|saved|scheduled|on)\b|\bi'?ll remind\b|\bi'?ve? set\b|\bi set\b"

def chk_said_nothing_set(t):
    """The answer agrees with a store that holds no reminder: it says nothing was set, and no part of it
    (a clause: "Nothing else, but your reminder for 8 is set" has two) states a reminder without denying it."""
    a = t["answer"]
    problems = [] if re.search(NOTHING_SET, a, re.I) else ["the answer should say that nothing was set"]
    for clause in re.split(r"[.!?;,]|\b(?:but|and|so|because|though|although|however|since)\b", a, flags=re.I):
        if clause and re.search(r"remind", clause, re.I) and re.search(REMINDER_STATED, clause, re.I) and not re.search(NEGATION, clause, re.I):
            problems.append("the answer states a reminder as set: %r" % clause.strip())
    return problems

# The time of an event has not been published: "not been announced", "no time yet", "isn't confirmed".
NOT_ANNOUNCED = r"(not|n't|no|yet to)\b.{0,30}\b(announc|publish|confirm|set|releas|reveal|disclos|known|given|decided|fixed|scheduled|available)|" \
                r"no (official |confirmed |exact |specific )?(time|date|details|word)|\b(tbc|tba|to be (confirmed|announced))\b|unknown"

# Saying that a device turned the command down, beside the words for "it did not work".
REFUSED = FAILED + r"|refus|reject|declin|turned (it|that|the \w+) down\b"

def claims_match(failed_word, worked_word):
    """Each part of the answer says what came of the action it names: a sentence (or, in a sentence that
    names both, a clause) about the `failed_word` thing (a regex) says it did not work, one about the
    `worked_word` thing does not say it failed; and the answer has one of each. "The timer failed. The TV
    worked." fails it; "The TV refused the volume change, so I couldn't turn it down." is about the TV."""
    def check(t):
        parts = []
        for sentence in re.split(r"[.;!?]", t["answer"]):
            if re.search(failed_word, sentence, re.I) and re.search(worked_word, sentence, re.I):
                parts += re.split(r"[:,]|\b(?:but|and|while|whereas|though|although|however)\b", sentence, flags=re.I)
            else: parts.append(sentence)
        problems, said_failed, said_worked = [], False, False
        for x in [p for p in parts if p and p.strip()]:
            bad, good = re.search(failed_word, x, re.I), re.search(worked_word, x, re.I)
            if bad and not good:
                if re.search(REFUSED, x, re.I): said_failed = True
                else: problems.append("this says the %s worked: %r" % (failed_word, x.strip()))
            elif good and not bad:
                if re.search(REFUSED, x, re.I): problems.append("this says the %s failed: %r" % (worked_word, x.strip()))
                else: said_worked = True
        if not said_failed: problems.append("the answer should say that the %s did not work" % failed_word)
        if not said_worked: problems.append("the answer should say that the %s worked" % worked_word)
        return problems
    return check

def tv_refused(t):
    """The TV was asked to go down a step in this turn, and refused (an ERROR result)."""
    steps = [r for c, r in _made(t, "smart_home") if str(c.get("action", "")).lower() == "set" and c.get("volume_step") is not None]
    return [] if steps and not any(worked(r) for r in steps) else ["the TV should be asked for a volume step and refuse it, got %s" % tried(t, "smart_home")]

# Any number at all, as digits or words ("seventy-three"), but not "one" ("would you like to save one?").
ANY_NUMBER = r"\d|\b(two|three|four|five|six|seven|eight|nine|ten|eleven|twelve|thirteen|fourteen|fifteen|sixteen|seventeen|" \
             r"eighteen|nineteen|twenty|thirty|forty|fifty|sixty|seventy|eighty|ninety|hundred)\b"

# The music library's genre of each song, for what a play call started ("now_playing": "So What by Miles Davis").
def music_genre(now_playing):
    for artist, _, genre, titles in MUSIC_LIBRARY:
        if any(str(now_playing) == "%s by %s" % (title, artist) for title in titles): return genre.lower()
    return None

def music_playing(genre):
    """A music play call worked in this turn and what it started is a song of that genre in the library."""
    def check(t):
        started = [r.get("now_playing") for c, r in [(c, results_of(r)) for c, r in _made(t, "music")]
                   if str(c.get("action", "play")).lower() == "play" and isinstance(r, dict) and r.get("now_playing")]
        if any(music_genre(x) == genre.lower() for x in started): return []
        return ["music should be playing a %s song, started %s, got %s" % (genre, started, tried(t, "music"))]
    return check

def device_set(setting, value):
    """A device call that worked set this setting (volume, brightness) to this value: the operation, the
    target and the value together."""
    def check(t):
        for c in calls(t, "device"):
            if str(c.get("setting", "")).lower() == setting and _same(c.get("value"), value): return []
        return ["device should set %s to %s, got %s" % (setting, value, tried(t, "device"))]
    return check

def results_of(r):
    try: return json.loads(r)
    except (TypeError, ValueError): return r

def no_timer_started(t):
    return ["no timer should be started, got %s" % tried(t, "timer")] if timer_calls(t, "start") else []

FORECAST_REMINDER = "if tomorrows rain chance in york is at least 60% remind me at eight tomorrow morning to take a coat"
LOCKER_42, LOCKER_24 = r"\b42\b|forty[- ]two", r"\b24\b|twenty[- ]four"
DONT_KNOW = r"don'?t (know|have)|do not (know|have)|not sure|no (record|locker|note|memory)|haven'?t|have not|can'?t (find|see|tell)|" \
            r"cannot|isn'?t (saved|stored)|not (saved|stored)|unknown|no longer|nothing"

CASES = [
    # --- answered from knowledge, no tools
    case("fact", "what is the capital of portugal", must_not=["web_search"], checks=[answer(r"lisbon")]),
    case("smalltalk", "thanks thats great", must_not=["*"]),
    case("local_time", "what time is it", must_not=["world_time"], checks=[chk_local_time]),
    case("translate_phrase", "how do you say where is the train station in portuguese", must_not=["look", "web_search"],
         checks=[answer(r"esta[çc][ãa]o", "the Portuguese phrase")]),
    case("brief", "tell me about the history of lisbon", must_not=["look"]),
    # --- live information
    case("weather", "whats the weather like in lisbon tomorrow", must=["weather"],
         checks=[answer(r"\d"), lambda t: arg(t, "weather", "location", has("lisbon"), "Lisbon")]),
    case("weather_home", "do i need an umbrella today", must=["weather"],
         checks=[lambda t: [] if any(str(c.get("location", "london")).lower().startswith("london") for c in calls(t, "weather"))
                 else ["weather should be for the home city"]]),
    case("garbled", "whats the whether in paris france", must=["weather"],
         checks=[answer(r"\d"), lambda t: arg(t, "weather", "location", has("paris"), "Paris")]),
    case("world_time", "what time is it in tokyo right now", must=["world_time"], checks=[chk_tokyo]),
    case("stock", "hows the apple share price doing", must=["stock_price"], checks=[chk_stock]),
    # The answer is what the stand-in web's search served (tests/mocks/web/search.json; its seed decides the values).
    case("search", "what time does the louvre open on sundays", must=["web_search"], checks=[chk_served("louvre-hours", "open", "time")],
         held_out="on a sunday when do the doors of the louvre open"),
    case("search_news", "who won the mens singles at wimbledon this year", must=["web_search"],
         checks=[chk_served("wimbledon-mens", "winner", "winner")], held_out="who is this years wimbledon mens champion"),
    # --- sums and dates
    case("calc", "whats 17 and a half percent of 2340",
         checks=[lambda t: [] if says_about(t["answer"], 409.5, 0.001) or
                 re.search(r"(?<![-−\d])\b409 (and a half|point 5)|four hundred (and )?nine (point five|and a half)", t["answer"], re.I)
                 else ["answer should give 409.5"]]),
    case("units", "whats 72 fahrenheit in celsius",
         checks=[lambda t: [] if says_about(t["answer"], 22.2, 0.06) or says_int(t["answer"], 22) or
                 re.search(r"(?<![-−])\btwenty[- ]two\b", t["answer"], re.I) and not re.search(r"minus twenty", t["answer"], re.I)
                 else ["answer should give 22.2"]]),
    case("python", "how many days until christmas", checks=[chk_christmas]),
    case("date_weekday", "what day of the week is the 3rd of march next year", checks=[chk_weekday]),
    case("currency_split", "how much is 85 euros in pounds and split that between three people", must=["convert_currency"],
         checks=[chk_currency_split]),
    # --- actions: timers, calendar, memory
    case("timer", "set a timer for 12 minutes", must=["timer"], must_not=["calendar_add", "reminder_set"],
         checks=[answer(r"12|twelve"), chk_timer(720)]),
    case("timer_seconds", "set a timer for 90 seconds", must=["timer"], must_not=["reminder_set"], checks=[chk_timer(90)]),
    case("timer_study", "i want to study for an hour can you set a timer", must=["timer"], must_not=["reminder_set"],
         checks=[chk_timer(3600, "stud")]),
    case("timer_oven", "the food needs to come out of the oven in 5 mins", must=["timer"], must_not=["reminder_set"],
         checks=[chk_timer(300)]),
    case("timer_left", ["set a timer for 10 minutes", "how long is left on my timer"], must=["timer"],
         checks=[on_turn(0, chk_timer(600)), chk_timer_action("list"), chk_time_left]),
    case("timer_cancel_named", ["i want to study for 45 minutes set a timer", "and set a 10 minute timer for the pasta",
                                "cancel the study timer"], must=["timer"],
         checks=[on_turn(0, chk_timer(2700)), on_turn(1, chk_timer(600)), chk_cancelled("stud", "pasta"), chk_timers_saved(600)]),
    case("timer_no_length", "set a timer", must_not=["timer", "reminder_set", "calendar_add"],
         checks=[answer(r"\?", "a question"), answer(r"how long|minutes|length", "a question about the length")]),
    case("reminder_clock", "remind me at 7 tomorrow morning to call the hotel", must=["reminder_set|calendar_add"],
         must_not=["timer"],
         checks=[answer(r"7|seven"), lambda t: at_check(t, "reminder_set|calendar_add", stamps([tomorrow()], "07:00"), "tomorrow 07:00")]),
    case("reminder_relative", "remind me in an hour and a half to take my tablets", must=["reminder_set|timer"], checks=[chk_ninety]),
    case("calendar_add", "put dinner with marta in my calendar for friday at 8pm", must=["calendar_add"],
         checks=[answer(r"friday|8"), lambda t: arg(t, "calendar_add", "start", one_of(FRI_8PM()), "Friday 20:00"),
                 chk_saved_when(1, FRI_8PM, "dinner", events_only=True)]),
    case("calendar_date", "add my flight to lisbon on the 3rd of december at 6 40 am", must=["calendar_add"], checks=[chk_flight]),
    case("birthday", "my mums birthday is on the 14th of march can you add it to my calendar", must=["calendar_add"],
         checks=[chk_yearly("03-14"), answer(r"march")]),
    case("birthday_with_year", "add sarahs birthday shes born on 20/11/1990", must=["calendar_add"],
         checks=[chk_yearly("11-20", 1990)]),
    case("repeat_weekly", "add bin collection to my calendar every tuesday at 7am", must=["calendar_add"],
         checks=[chk_weekly_tuesday]),
    case("birthday_recall", ["my dads birthday is the 2nd of june 1955 put it in the calendar", "/new",
                             "when is dads birthday and how old will he be"],
         must=["calendar_list"], checks=[on_turn(0, chk_yearly("06-02", 1955)), answer(r"june"), chk_dad_age]),
    case("stressed", "im feeling really stressed right now", must=["breathe"], must_not=["memory_save"]),
    case("meditate", "i need to meditate", must=["breathe"]),
    case("breathe_minutes", "can we do a three minute breathing exercise", must=["breathe"],
         checks=[lambda t: arg(t, "breathe", "minutes", num_is(3), "3")]),
    case("numeric_date_day_first", "put the dentist in my calendar on 3/4 at 10am", must=["calendar_add"],
         checks=[chk_numeric_date(4, 3), answer(r"april", "the month as a word")]),
    case("numeric_date_month_first", "put the dentist in my calendar on 3/4 at 10am", must=["calendar_add"],
         env={"ZB_DATE_ORDER": "mdy"}, checks=[chk_numeric_date(3, 4), answer(r"march", "the month as a word")]),
    case("calendar_query", "whats on my calendar this week", must=["calendar_list"]),
    case("memory", "remember that im allergic to shellfish", must=["memory_save"],
         checks=[lambda t: arg(t, "memory_save", "text", has("shellfish"), "about shellfish")]),
    case("memory_implicit", "by the way im vegetarian", must=["memory_save"],
         checks=[lambda t: arg(t, "memory_save", "text", has("vegetarian"), "about being vegetarian")]),
    case("memory_recall", ["remember that im allergic to shellfish", "/new", "is the prawn curry ok for me to eat"],
         checks=[on_turn(0, chk_memory("shellfish")), answer(r"shellfish", "the saved shellfish allergy")]),
    # --- device, music, TV, camera
    case("music_artist", "play a song by abba", must=["music"], checks=[answer(r"abba|playing")]),
    case("music_genre", "play a random rock song", must=["music"]),
    case("music_stop", "stop the music", must=["music"], checks=[lambda t: arg(t, "music", "action", has("stop"), "stop")]),
    # An artist the stand-in library (MUSIC_LIBRARY) does not have.
    case("music_missing", "play something by the rolling stones", tried=["music"],
         checks=[not_answer(r"(now )?playing (something|a song|music)? ?(by )?(the )?rolling stones|here'?s (the )?rolling stones",
                            "claim the Rolling Stones are playing"),
                 answer(r"couldn'?t|could not|no |don'?t have|isn'?t|not in|nothing|aren'?t|can'?t find")]),
    case("tv_power", "please turn on the tv", must=["tv_remote"], checks=[lambda t: arg(t, "tv_remote", "key", has("power"), "power")]),
    case("tv_volume", "turn the tv volume up a bit", must=["tv_remote"], must_not=["device"],
         checks=[lambda t: arg(t, "tv_remote", "key", has("volume_up"), "volume_up")]),
    case("tv_channel", "tv channel down", must=["tv_remote"], checks=[lambda t: arg(t, "tv_remote", "key", has("channel_down"), "channel_down")]),
    case("volume", "turn your volume down to 40 percent", must=["device"], must_not=["tv_remote"],
         checks=[answer(r"40|forty"), lambda t: arg(t, "device", "value", num_is(40), "40")]),
    case("timezone", "ive just landed in new york switch the clock over", must=["set_timezone"],
         checks=[lambda t: arg(t, "set_timezone", "place", lambda v: "new" in str(v).lower() and "york" in str(v).lower(), "New York")]),
    case("look", "what am i looking at", must=["look"]),
    case("translate_sign", "can you translate this sign for me", must=["look"]),
    # --- several steps in one request
    case("multi_weather_cal", "check the weather in rome for saturday and set a reminder to pack an umbrella on friday at 6pm",
         must=["weather", "reminder_set|calendar_add"], checks=[chk_umbrella, lambda t: arg(t, "weather", "location", has("rome"), "Rome")]),
    case("multi_currency", "how much is 200 dollars in euros and in yen", must=["convert_currency"], checks=[chk_two_currencies]),
    case("multi_tip", "the bill is 64 euros add a 15 percent tip and tell me what that is in pounds", must=["convert_currency"], checks=[chk_tip]),
    # --- follow-ups that depend on the previous turn
    case("followup_weather", ["whats the weather in lisbon tomorrow", "and what about porto"], must=["weather"],
         checks=[on_turn(0, lambda t: arg(t, "weather", "location", has("lisbon"), "Lisbon")),
                 lambda t: arg(t, "weather", "location", has("porto"), "Porto")]),
    case("followup_move", ["put dinner with marta in my calendar for friday at 8pm", "actually make it half past eight"],
         must=["calendar_update|calendar_add"], checks=[chk_moved]),
    case("followup_cancel", ["set a timer for 10 minutes", "cancel that"], must=["timer"], must_not=["reminder_cancel"],
         checks=[on_turn(0, chk_timer(600)), chk_timer_action("cancel"), chk_timers_saved()]),
    case("followup_convert", ["how much is 40 euros in pounds", "and in dollars"], must=["convert_currency"],
         checks=[lambda t: arg(t, "convert_currency", "to", has("usd"), "USD"), lambda t: arg(t, "convert_currency", "amount", num_is(40), "40")]),
    # --- input added while the request is running
    case("steer", "whats the weather in lisbon tomorrow", steer="sorry i meant porto", must=["weather"],
         checks=[lambda t: arg(t, "weather", "location", has("porto"), "Porto"), answer(r"porto")]),
    # --- requests that should get a question or an honest "no", not a guess
    case("clarify_reminder", "set a reminder", must_not=["reminder_set", "calendar_add", "timer"], checks=[answer(r"\?", "a clarifying question")]),
    case("clarify_calendar", "add a meeting with tom to my calendar", must_not=["calendar_add"], checks=[answer(r"\?", "a clarifying question")]),
    case("unsupported", "send a text to marta saying im running late", must_not=["memory_save", "reminder_set", "calendar_add"],
         checks=[answer(CANNOT, "that it cannot send messages"), not_answer(r"\b(i'?ve |message |text )?sent\b(?! (it|that) yourself)", "claim the text was sent")]),
    # --- tools that fail
    case("search_no_key", "what time does the louvre open on sundays", env={"ZB_SEARCH_KEY": None},
         checks=[answer(UNSURE, "that it could not check, or that the answer is from memory")]),
    # --- the web on a bad day (the stand-in's edge fixtures): say what happened, invent nothing
    case("search_empty", "what time does the louvre open on sundays", held_out="on a sunday when do the doors of the louvre open", env=web("search_empty"), must=["web_search"],
         checks=[answer(NOT_FOUND, "that nothing was found")]),
    case("search_error", "what time does the louvre open on sundays", held_out="on a sunday when do the doors of the louvre open", env=web("search_error"), tried=["web_search"],
         checks=[answer(NOT_FOUND, "that it could not look it up")]),
    case("search_irrelevant", "what time does the louvre open on sundays", held_out="on a sunday when do the doors of the louvre open", env=web("search_irrelevant"), must=["web_search"],
         checks=[answer(NOT_FOUND, "that nothing useful was found")]),
    case("search_conflict", "what time does the louvre open on sundays", held_out="on a sunday when do the doors of the louvre open", env=web("search_conflict"), must=["web_search"], checks=[chk_conflict]),
    case("search_stale", "who won the mens singles at wimbledon this year", held_out="who is this years wimbledon mens champion", env=web("search_stale"), must=["web_search"], checks=[chk_stale]),
    case("search_long", "what time does the louvre open on sundays", held_out="on a sunday when do the doors of the louvre open", env=web("search_long"), must=["web_search"],
         checks=[chk_served("louvre-long", "open", "time")]),
    case("weather_error", "whats the weather like in lisbon tomorrow", env=web("weather_error"), tried=["weather"],
         checks=[chk_weather_served, answer(NOT_FOUND, "that the forecast could not be got")]),
    case("weather_partial", "whats the weather going to be like in paris over the next four days", env=web("weather_partial"), must=["weather"]),
    case("currency_missing_rate", "how much is 50 euros in pounds", env=web("rates_missing"), tried=["convert_currency"],
         checks=[answer(NOT_FOUND + "|" + CANNOT, "that the rate could not be got")]),
    case("weather_unknown_place", "whats the weather like in florbington",
         checks=[not_answer(r"\d+ ?(degrees|°)", "invent a temperature"), answer(r"find|couldn'?t|could not|not sure|which|where|\?")]),
    # --- speech-recogniser slips and other languages
    case("asr_homophone", "set an alarm for ate tomorrow morning", must=["reminder_set|calendar_add"], checks=[chk_alarm]),
    case("asr_slang", "how much is fifty bucks in euros", must=["convert_currency"],
         checks=[lambda t: arg(t, "convert_currency", "amount", num_is(50), "50"), lambda t: arg(t, "convert_currency", "from", has("usd"), "USD")]),
    case("spanish_weather", "qué tiempo hace en madrid hoy", must=["weather"], checks=[chk_spanish]),
    case("german_timer", "stell einen timer auf fünf minuten", must=["timer"], must_not=["reminder_set"],
         checks=[chk_timer(300), answer(r"minuten|timer|läuft|gestellt", "a German reply")]),
    # --- smart home: a made-up home on the stand-in test network (survey 6.4.2)
    case("home_on", "turn on the bed heater", must=["smart_home"], home=EVAL_HOME, checks=[home_call("on", ["Bed heater"])]),
    case("home_dim_room", "bedroom lights to 30 percent", must=["smart_home"], home=EVAL_HOME,
         checks=[home_call("set", ["Bedroom", "Bedroom lamp"], brightness=30)]),
    case("home_colour", "make the lounge warm white", must=["smart_home"], home=EVAL_HOME,
         checks=[home_call("set", ["Lounge", "Floor lamp"], color="warm white")]),
    case("home_heating", "set the heating to 20", must=["smart_home"], home=EVAL_HOME,
         checks=[home_call("set", ["Hall thermostat"], temperature=20)]),
    case("home_status", "is the heater on", must=["smart_home"], home=EVAL_HOME, checks=[home_call("status"), home_never_on]),
    case("home_pause", "pause the kitchen speaker", must=["smart_home"], home=EVAL_HOME,
         checks=[home_call("pause", ["Kitchen speaker", "Kitchen"])]),
    case("home_everything", "turn everything off", home=EVAL_HOME, checks=[home_never_on]),
    case("home_missing", "turn on the garage door", home=EVAL_HOME,
         checks=[home_never_on, answer(CANNOT + r"|no (device|garage)|isn'?t|not (one|listed|among|set up)|don'?t see|couldn'?t find",
                                       "that there is no such device")]),
    case("home_same_name", "turn on the lamp in the hall", must=["smart_home"], home=EVAL_HOME,
         checks=[home_call("on", ["Lamp (Hall)", "Hall"])]),
    case("home_ask_first", "turn on the fan heater", tried=["smart_home"], home=EVAL_HOME, checks=[home_asked_about("heater")]),
    case("home_ask_first_yes", ["turn on the fan heater", "yes"], must=["smart_home"], home=EVAL_HOME,
         checks=[on_turn(0, home_asked_about("heater")), home_call("on", ["Fan heater"]), home_switched("Fan heater")]),
    # A refusal, with and without the word "no": the question named the heater, and it was switched on in no turn.
    case("home_ask_first_no", ["turn on the fan heater", "no"], home=EVAL_HOME,
         checks=[on_turn(0, home_asked_about("heater")), every_turn(home_not_switched("Fan heater"))]),
    case("home_ask_first_leave_off", ["turn on the fan heater", "leave it off please"], home=EVAL_HOME,
         checks=[on_turn(0, home_asked_about("heater")), every_turn(home_not_switched("Fan heater"))]),
    # After a web page the change waits for the user's word, and "no" stops it (search from the stand-in web):
    # the first turn searches and asks about the lamp; in no turn is it switched on.
    case("home_after_web_no", ["look up when the sun sets today, then turn on the bedroom lamp", "no, leave it"], home=EVAL_HOME,
         env=WEB, checks=[on_turn(0, lambda t: arg(t, "web_search", "query", has("sun"), "about the sunset")), on_turn(0, asks_about("lamp")),
                          every_turn(home_not_switched("Bedroom lamp"))]),
    # The page is still part of the conversation a turn later: a change asked for then waits for a yes too.
    case("home_after_web_later", ["look up when the sun sets today", "turn on the bedroom lamp"], tried=["smart_home"],
         home=EVAL_HOME, env=WEB, checks=[on_turn(0, lambda t: arg(t, "web_search", "query", has("sun"), "about the sunset")),
                                          home_asked_about("lamp"), every_turn(home_not_switched("Bedroom lamp"))]),
    # More than eight things: the model asks in its own words, and a plain yes switches everything off.
    case("home_everything_yes", ["turn everything off", "yes"], must=["smart_home"], home=SOCKETS_HOME,
         checks=[on_turn(0, home_asked), home_all_off]),
    case("home_neighbour_timer", "set a timer for 10 minutes", must=["timer"], must_not=["smart_home", "reminder_set"], home=EVAL_HOME,
         checks=[chk_timer(600)]),
    case("home_neighbour_weather", "whats the weather like in lisbon tomorrow", must=["weather"], must_not=["smart_home"],
         home=EVAL_HOME),

    # Routing between the tools that overlap: the smart home against the infrared remote, the
    # device's own volume and music, timers against reminders, and the breathing exercise.
    case("route_tv_home", "turn on the tv", must=["smart_home"], must_not=["tv_remote"], home=EVAL_HOME,
         checks=[home_call("on", ["Living room TV"])]),
    case("route_own_volume_home", "turn your volume down to 40 percent", must=["device"], must_not=["smart_home", "tv_remote"],
         home=EVAL_HOME, checks=[lambda t: arg(t, "device", "value", num_is(40), "40")]),
    case("route_speaker_volume", "set the kitchen speaker volume to 20", must=["smart_home"], must_not=["device", "music"],
         home=EVAL_HOME, checks=[home_call("set", ["Kitchen speaker"], volume=20)]),
    case("route_music_home", "play a song by abba", must=["music"], must_not=["smart_home"], home=EVAL_HOME,
         checks=[lambda t: arg(t, "music", "action", has("play"), "play")]),
    case("route_room_off", "turn off the bedroom lights", must=["smart_home"], home=EVAL_HOME,
         checks=[home_call("off", ["Bedroom", "Bedroom lamp"]), home_never_on]),
    case("route_room_status", "is anything on in the kitchen", must=["smart_home"], home=EVAL_HOME,
         checks=[home_call("status"), home_never_on]),
    case("route_heater_and_timer", "turn on the bed heater and set a timer for 30 minutes", must=["smart_home", "timer"],
         must_not=["reminder_set"], home=EVAL_HOME, checks=[home_call("on", ["Bed heater"]), chk_timer(1800)]),
    case("route_reminder_home", "remind me at 7 tomorrow morning to call the hotel", must=["reminder_set|calendar_add"],
         must_not=["smart_home", "timer"], home=EVAL_HOME,
         checks=[lambda t: at_check(t, "reminder_set|calendar_add", stamps([tomorrow()], "07:00"), "tomorrow 07:00")]),
    case("route_eggs_home", "the eggs need six minutes", must=["timer"], must_not=["smart_home", "reminder_set"],
         home=EVAL_HOME, checks=[chk_timer(360)]),
    case("route_cancel_timer_home", ["set a timer for 10 minutes", "cancel the timer"], must=["timer"],
         must_not=["smart_home", "reminder_cancel"], home=EVAL_HOME,
         checks=[on_turn(0, chk_timer(600)), chk_timer_action("cancel"), chk_timers_saved()]),
    case("route_breathe_home", "im feeling really stressed right now", must=["breathe"], must_not=["smart_home", "timer"],
         home=EVAL_HOME),
    # A TV with volume keys and remote keys over the network: those go to smart_home, not infrared.
    case("route_tv_volume_steps", "turn the tv up a bit", must=["smart_home"], must_not=["tv_remote", "device"],
         home=TV_HOME, checks=[home_step(+1)]),
    case("route_tv_volume_down", "turn the tv volume down three", must=["smart_home"], must_not=["tv_remote", "device"],
         home=TV_HOME, checks=[home_call("set", ["Living room TV"], volume_step=-3)]),
    case("route_tv_key", "press down on the tv", must=["smart_home"], must_not=["tv_remote"], home=TV_HOME,
         checks=[home_call("press", ["Living room TV"], key="down")]),
    case("route_tv_ok", "press ok on the tv", must=["smart_home"], must_not=["tv_remote"], home=TV_HOME,
         checks=[home_call("press", ["Living room TV"], key="ok")]),
    case("route_tv_app", "put netflix on the tv", must=["smart_home"], must_not=["tv_remote"], home=TV_HOME,
         checks=[home_call("set", ["Living room TV"], source="netflix")]),
    case("route_own_volume_tv_home", "turn your volume up", must=["device"], must_not=["smart_home", "tv_remote"],
         home=TV_HOME),
    # "Turn the TV down" is its volume, not off; "off" still means off (ZB-072).
    case("route_tv_down_plain", "turn the tv down", must=["smart_home"], must_not=["tv_remote", "device"], home=TV_HOME,
         checks=[home_step(-1), home_no_action("off")]),
    case("route_tv_up_plain", "turn the tv up", must=["smart_home"], must_not=["tv_remote", "device"], home=TV_HOME,
         checks=[home_step(+1)]),
    case("route_tv_down_after_up", ["turn the tv up a bit", "okay that worked, can you turn the tv down now"], must=["smart_home"],
         must_not=["tv_remote", "device"], home=TV_HOME, checks=[on_turn(0, home_step(+1)), home_step(-1), home_no_action("off")]),
    case("route_tv_down_it", ["put netflix on the tv", "turn it down a bit"], must=["smart_home"], must_not=["tv_remote", "device"],
         home=TV_HOME, checks=[on_turn(0, home_call("set", ["Living room TV"], source="netflix")), home_step(-1), home_no_action("off")]),
    case("route_tv_down_ramble", ["turn the tv up a bit", "okay that worked, and by the way switching the tv on earlier did work as "
                                  "well. can we try and turn the tv down now"], must=["smart_home"], must_not=["tv_remote", "device"],
         home=TV_HOME, checks=[on_turn(0, home_step(+1)), home_step(-1), home_no_action("off")]),
    case("route_tv_down_asr", "um can we try and turn the t. v. down now", must=["smart_home"], must_not=["tv_remote", "device"],
         home=TV_HOME, checks=[home_step(-1), home_no_action("off")]),
    case("route_tv_off_plain", "turn the tv off", must=["smart_home"], must_not=["tv_remote"], home=TV_HOME,
         checks=[home_call("off", ["Living room TV"]), home_no_action("set")]),
    case("route_tv_off_after_volume", ["turn the tv up a bit", "now turn the tv off"], must=["smart_home"], must_not=["tv_remote"],
         home=TV_HOME, checks=[on_turn(0, home_step(+1)), home_call("off", ["Living room TV"]), home_no_action("set")]),
    # After a network error for a TV the smart home lists, not the infrared remote: it sends the codes
    # of the one brand in settings, which may be another TV's (ZB-072).
    case("route_tv_offline_volume", "turn the tv up a bit", tried=["smart_home"], must_not=["tv_remote", "device"],
         home=TV_SILENT_HOME, env=LIVE, checks=[answer(FAILED, "that it did not work"), NO_IR_OFFER]),
    case("route_tv_offline_followup", ["turn on the tv", "turn the volume up"], must_not=["tv_remote", "device"],
         home=TV_SILENT_HOME, env=LIVE, checks=[on_turn(0, no_call("tv_remote")), on_turn(0, NO_IR_OFFER), NO_IR_OFFER]),
    case("route_tv_unplugged_followup", ["switch the tv on", "turn it up a bit"], must_not=["tv_remote", "device"],
         home=TV_CLOSED_HOME, env=LIVE, checks=[on_turn(0, no_call("tv_remote")), on_turn(0, NO_IR_OFFER), NO_IR_OFFER]),
    case("route_lamp_no_home", "turn on the bedroom lamp", must_not=["tv_remote", "device"],
         checks=[answer(CANNOT + r"|smart.home|set up|settings", "that no lamps are set up")]),

    # Radio from the user's own stations, against the music library and with no stations at all.
    case("radio_by_nickname", "play radio 2", must=["radio"], must_not=["music"], radio=EVAL_RADIO,
         checks=[radio_play("Northern Radio 2")]),
    case("radio_by_name", "put on harbour fm", must=["radio"], must_not=["music"], radio=EVAL_RADIO,
         checks=[radio_play("Harbour FM")]),
    case("radio_nickname_phrase", "can you put the jazz station on", must=["radio"], must_not=["music"], radio=EVAL_RADIO,
         checks=[radio_play("Night Jazz 24")]),
    case("radio_stop", ["play radio 2", "stop the radio"], must=["radio|music"], radio=EVAL_RADIO,
         checks=[on_turn(0, radio_play("Northern Radio 2")), stops_player]),
    case("radio_vs_music", "play some abba", must=["music"], must_not=["radio"], radio=EVAL_RADIO,
         checks=[lambda t: arg(t, "music", "action", has("play"), "play")]),
    case("radio_unknown_station", "play radio 4", must_not=["music"], radio=EVAL_RADIO,
         checks=[radio_never_plays, not_answer(r"(now )?playing radio 4|here'?s radio 4|radio 4 is (now )?(on|playing)",
                                                  "claim Radio 4 is playing"),
                 answer(CANNOT + r"|not (one of|in|on)|no (station|radio 4)|isn'?t (one|in|on|a)|aren'?t", "that Radio 4 is not one of the stations")]),
    case("radio_none_set_up", "play radio 2", must_not=["radio"],
         checks=[not_answer(r"(now )?playing radio 2|here'?s radio 2|radio 2 is (now )?(on|playing)", "claim Radio 2 is playing"),
                 answer(RADIO_NOT_SET_UP, "that radio is not set up")]),

    # Conversations from the evaluation review (19 November 2026), each turn held to what is saved after it.
    # Two timers and an ambiguous correction: both start; "one of them" gets a question and changes
    # nothing; tea gains exactly three minutes; toast is cancelled; the answer gives tea's time left.
    case("review_two_timers", ["set a tea timer for twelve minutes and a toast timer for twenty", "add three minutes to one of them",
                               "the shorter one", "cancel the other one", "whats left"], must=["timer"],
         checks=[on_turn(0, chk_timer(720, "tea")), on_turn(0, chk_timer(1200, "toast")), on_turn(0, chk_timers_named(tea=720, toast=1200)),
                 on_turn(1, answer(r"\?", "a question: which one")), on_turn(1, chk_timer_action_none("add_time")),
                 chk_deadlines({"tea": 0, "toast": 0}, {"tea": 0, "toast": 0}, {"tea": 180, "toast": 0}, {"tea": 180}, {"tea": 180}),
                 chk_timer_left_said("tea")]),
    # A conditional reminder at a forecast threshold: the same words with tomorrow at 59%, 60% and no
    # figure, today on the other side of 60; zero, one and zero reminders, and the follow-up agrees.
    case("review_rain_59", [FORECAST_REMINDER, "what did you set"], env=web("rain59"),
         checks=[on_turn(0, lambda t: arg(t, "weather", "location", has("york"), "York")), on_turn(0, chk_forecast_seen(1, 59)),
                 on_turn(0, chk_saved_when(0)), chk_saved_when(0), chk_said_nothing_set]),
    case("review_rain_60", [FORECAST_REMINDER, "what did you set"], must_not=["timer"], env=web("rain60"),
         checks=[on_turn(0, lambda t: arg(t, "weather", "location", has("york"), "York")), on_turn(0, chk_forecast_seen(1, 60)),
                 on_turn(0, chk_saved_when(1, lambda: stamps([tomorrow()], "08:00"), "coat")),
                 chk_saved_when(1, lambda: stamps([tomorrow()], "08:00"), "coat"), answer(r"coat"),
                 lambda t: [] if says_time(t["answer"], datetime.datetime(2000, 1, 1, 8, 0), 0) or
                 any(right_half(t["answer"], m.end(), 8) for m in re.finditer(r"\b(?:8|eight)\b(?![:.]\d)", t["answer"], re.I))
                 else ["the answer should give 8 am"]]),
    case("review_rain_missing", [FORECAST_REMINDER, "what did you set"], env=web("rainmissing"),
         checks=[on_turn(0, lambda t: arg(t, "weather", "location", has("york"), "York")), on_turn(0, chk_forecast_seen(1, None)),
                 on_turn(0, chk_saved_when(0)), chk_saved_when(0), chk_said_nothing_set]),
    # A stale yes: the question about the heater is overtaken by "leave it off"; a later bare "yes" switches
    # nothing, and the last answer says the heater is off. Judged in dry run, by the smart home's results and
    # its saved state after each turn; a check from a simulated plug's own log is left for later (ZB-077).
    case("review_stale_yes", ["turn on the fan heater", "leave it off. whats eight times seven", "yes", "is the fan heater on"],
         home=EVAL_HOME, checks=[on_turn(0, home_asked_about("heater")), on_turn(1, lambda t: [] if says_int(t["answer"], 56) else ["should say 56"]),
                                 every_turn(home_not_switched("Fan heater")), answer(r"\boff\b|not on|isn'?t on", "that the heater is off"),
                                 not_answer(r"\b(heater|it)('s| is) (now )?(switched |turned )?on\b", "say the heater is on")]),
    # A correction that survives a new conversation, then a deletion that does: the saved memory is read after each.
    case("review_locker", ["remember my locker is forty-two", "correction, its twenty-four", "/new", "which locker is mine",
                           "forget my locker number", "/new", "which locker is mine"],
         checks=[on_turn(0, chk_memory(LOCKER_42)), on_turn(1, chk_memory(LOCKER_24, lacks=[LOCKER_42])),
                 on_turn(2, answer(LOCKER_24, "locker 24")), on_turn(2, not_answer(LOCKER_42, "give the old number 42")),
                 on_turn(3, chk_memory_lacks(r"locker", LOCKER_24, LOCKER_42)), answer(DONT_KNOW, "that the number is not known"),
                 not_answer(ANY_NUMBER, "give a locker number, or any number")]),
    # A success beside a failure: the TV refuses its first command, the timer works. The first answer says
    # which worked and which did not; "again" sends one more step (two in all); one pasta timer is left.
    case("review_tv_refused_once", ["set a ten minute pasta timer and turn the tv down one step", "try just the tv again",
                                    "how many timers are running"], must_not=["tv_remote"], home=_refusing_tv_home, env=LIVE,
         checks=[on_turn(0, chk_timer(600, "pasta")), on_turn(0, tv_refused), on_turn(0, claims_match(r"\btv\b|telly|television", r"pasta|timer")),
                 on_turn(0, NO_IR_OFFER), on_turn(0, device_sent("POST /v1/FireTV volume_down", 1)),
                 on_turn(1, home_step(-1)), on_turn(1, device_sent("POST /v1/FireTV volume_down", 1)),
                 on_turn(1, answer(r"\b(tv|telly|television|volume)\b", "what the retry did")),
                 on_turn(1, not_answer(REFUSED, "say the retry failed")), device_sent("POST /v1/FireTV volume_down", 0),
                 chk_device_log("POST /v1/FireTV volume_down", 2), chk_device_log("POST /v1/FireTV volume_up", 0),
                 chk_timers_named(pasta=600),
                 # The count is of timers: "One device is online" is no answer.
                 answer(r"(?<![\w-])(one|1|a single|just (the|one))\b(?! (moment|second|sec|minute|min|mo)\b)[^.!?]{0,40}\btimers?\b|"
                        r"\b(only|just) (the|your) pasta timer\b", "one timer"),
                 not_answer(r"\b(two|three|four|2|3|4) timers\b", "count more than one timer")]),
    # Short edge cases.
    case("review_hypothetical_timer", "what would you say if i asked you to start a timer", quotes_ok=True,
         checks=[no_timer_started, chk_timers_saved()]),
    case("review_self_correction", "set a timer for fifteen, sorry, fifty minutes", must=["timer"],
         checks=[chk_timer(3000), chk_timers_saved(3000)]),
    case("review_new_year", "its 11:55 pm on 31 december. what date and time is ten minutes later",
         must_not=["timer", "reminder_set", "calendar_add"],
         checks=[lambda t: [] if says_time(t["answer"], datetime.datetime(2000, 1, 1, 0, 5), 0) else ["the answer should give 12:05 am"],
                 answer(r"\b1(st)? (of )?january|january (the )?1(st)?\b|first of january|new year'?s day", "1 January"),
                 chk_timers_saved(), chk_saved_when(0)]),
]

# ---------------------------------------------------------------- running and scoring
# The scorer's version, in every result (with the case's hash and whether it was traced), so that a
# comparison of two runs knows when their verdicts are comparable. Raise it when what a check accepts
# changes.
SCORER_VERSION = "2026-11-21.9"   # ZB-086: .3 evidence, meaning, the day asked; .4 decimal comma; .5 round 2: visible facts, roles, units, currency bound; .6 unlabelled share, named days; .7 amounts in words; .8 round 3: day per clause, the model's cut, denied names, bare amounts, flags; .9 weekdays in six languages, dates written out

TOOL_RE = re.compile(r"^\s+-> (\w+) (.*)$")
RESULT_RE = re.compile(r"^\s+<- (.*)$")
FIRST_RE = re.compile(r"\[first words \+([\d.]+)s\]")
ANSWER_RE = re.compile(r"^\[answer ([\d.]+)s\] ?(.*)", re.S | re.M)
TURN_RE = re.compile(r"^(?:> )*\[user\] ", re.M)
# Older runs printed the call and state records on stdout among the readable lines; the cli now writes
# them to a file of their own (ZB_EVAL_RECORDS; see turns_from_records), and stdout is only for reading.
CALL_RE = re.compile(r"^\[call\] (\{.*\})\s*$")
STATE_RE = re.compile(r"^\[state\] (\{.*\})\s*$")
# Words that say something was done. Said with no tool call in the turn, or after calls that all failed, the claim is false.
CLAIM_RE = re.compile(r"\b(i'?ve (added|saved|set|scheduled|put|noted|cancell?ed|deleted|moved|sent|booked|turned|changed|updated)|"
                      r"(is|it'?s) (now )?(on|in) your calendar|(timer|reminder|alarm) (is |has been )?(set|cancell?ed)|"
                      r"now playing|i'?ll remember|(?<!you )(?<!you've )(?<!youve )(?<!you have )saved (that|it)|got it,? (saved|noted))", re.I)
EMOJI_RE = re.compile("[\U0001F000-\U0001FAFF☀-➿⬀-⯿️]")

def plain(answer):
    """The answer as the checks read it: typographic apostrophes as plain ones ("don’t" is "don't")."""
    return str(answer).replace("’", "'").replace("‘", "'")

def parse_turn(text):
    """One turn of the cli's printed output, for runs without a records file (older builds and saved runs).
    The tool calls come from "[call]" lines when there are some, else from the "->"/"<-" lines, whose
    results are cut at 300 bytes. "state" is the "[state]" line, when there is one. This channel is shared
    with the answer, which can quote any of these markers: turns_from_records reads the file instead."""
    turn = {"tools": [], "results": [], "answer": "", "total": None, "first": None, "error": None}
    records = []
    for line in text.splitlines():
        m = CALL_RE.match(line) or STATE_RE.match(line)
        if m:
            try: rec = json.loads(m.group(1))
            except ValueError: rec = None
            if isinstance(rec, dict):
                if line.startswith("[call]"): records.append(rec)
                else: turn["state"] = rec
            continue
        m = TOOL_RE.match(line)
        if m: turn["tools"].append((m.group(1), m.group(2)))
        m = RESULT_RE.match(line)
        if m: turn["results"].append(m.group(1))
        m = FIRST_RE.search(line)
        if m: turn["first"] = float(m.group(1))
        if line.startswith("[error]"): turn["error"] = line[8:]
    if records:
        turn["calls"] = [{"id": str(r.get("id", "")), "name": str(r.get("name", "")), "args": str(r.get("args", "")),
                          "result": str(r.get("result", ""))} for r in records]
        turn["tools"] = [(c["name"], c["args"]) for c in turn["calls"]]
        turn["results"] = [c["result"] for c in turn["calls"]]
    m = ANSWER_RE.search(text)
    if m:
        turn["total"] = float(m.group(1))
        turn["answer"] = plain(re.sub(r"(\n> )*\s*$", "", m.group(2)).strip())
    return turn

def _blank_turn(**kw):
    return dict({"tools": [], "results": [], "answer": "", "total": None, "first": None, "error": None}, **kw)

def _stop(turns, expected, stopped, failed, elapsed):
    """Marks where a run that stopped early stopped (see turns_from_output) and adds the turns it never started."""
    if stopped is not None:
        done = sum(t["total"] or 0 for t in turns if t is not stopped)
        stopped["error"] = stopped["error"] or failed or "no answer: the app stopped during this turn"
        if failed == "timeout": stopped["timed_out"] = True
        stopped["total"] = max(0.0, elapsed - done) if elapsed is not None and failed == "timeout" else None
    while len(turns) < expected:
        turns.append(_blank_turn(error="not started: an earlier turn %s" % ("timed out" if failed == "timeout" else "did not finish"),
                                 unstarted=True))
    return turns

def turns_from_output(out, c, failed=None, elapsed=None):
    """The turns of a case from the cli's printed output (older builds). A run that stopped (failed:
    "timeout", or None for one that ended early) counts once, in the turn it stopped in: that turn's time
    is what was left of the run after the turns before it, and the turns after it are marked unstarted,
    with no time, rather than each being given the whole run's time."""
    turns = [parse_turn(seg) for seg in TURN_RE.split(out)[1:]]
    expected = sum(1 for t in c["turns"] if t != "/new")
    stopped = None
    if turns and turns[-1]["total"] is None and (failed or len(turns) <= expected):
        stopped = turns[-1]   # began, never answered
    elif len(turns) < expected:
        stopped = _blank_turn()
        turns.append(stopped)
    return _stop(turns, expected, stopped, failed, elapsed)

RECORDS_VERSION = 2
STORES = ("reminders", "calendar", "memory", "home")

def turns_from_records(text, c, process):
    """(turns, reasons): the turns of a case from the cli's records file (ZB_EVAL_RECORDS: one JSON object a
    line, written apart from the answer, so the answer cannot make one), and what makes the run unscorable.
    The stream must be whole and in order: "start" first, then for each request "turn_start" (its number and
    the case's own words; "new" where the case has "/new"), its call, steer and error records, exactly one
    "state", and "turn_end"; then "end", and nothing after it. Every call record has its number, id, name,
    arguments and result, and the calls of the turn are exactly those of its model responses (listed in
    "turn_end", from the history), in order, ids included; no response repeats an id. An app that exited
    normally must have written every turn and the end. A store present in one turn and absent in a later one
    is marked "vanished" there. process: {"exit", "timeout", "seconds"}."""
    reasons, turns, cur, pos, ended = [], [], None, 0, False
    seen = set()   # stores that existed in an earlier turn
    script = list(c["turns"])
    lines = text.split("\n")
    tail = lines.pop()   # after the last newline: nothing, unless the app was stopped while writing
    if tail and not process.get("timeout"): reasons.append("the records end in the middle of a line")
    for k, line in enumerate(lines, 1):
        def bad(why):
            reasons.append("record %d: %s" % (k, why))
        try: r = json.loads(line)
        except ValueError:
            bad("not JSON")
            break
        if not isinstance(r, dict) or r.get("v") != RECORDS_VERSION or not isinstance(r.get("ev"), str):
            bad("not a record of version %d" % RECORDS_VERSION)
            break
        kind = r["ev"]
        if ended:
            bad("a %s record after the end of the run" % kind)
            break
        if (k == 1) != (kind == "start"):
            bad("the run does not begin with its start" if k == 1 else "a second start")
            break
        if kind == "start":
            caps = r.get("caps")
            if not isinstance(caps, list) or "records" not in caps:
                bad("a start without the build's capabilities")
                break
        elif kind == "new":
            if cur is not None or pos >= len(script) or script[pos] != "/new":
                bad("a new conversation where the case has none")
                break
            pos += 1
        elif kind == "turn_start":
            want = script[pos] if pos < len(script) else None
            if cur is not None:
                bad("turn %s starts inside turn %d" % (r.get("turn"), len(turns)))
                break
            if want is None or want == "/new" or r.get("text") != want or r.get("turn") != len(turns) + 1:
                bad("turn %s (%r) is not the case's next request (%r)" % (r.get("turn"), r.get("text"), want))
                break
            pos += 1
            # Times: "began", "ended" and each call's "at" are seconds since the app started; the "_wall" ones the clock's.
            cur = _blank_turn(calls=[], began=r.get("mono"), began_wall=r.get("wall"))
            turns.append(cur)
        elif kind == "error" and cur is None:
            if turns: turns[-1]["error"] = turns[-1]["error"] or str(r.get("message"))   # after the request, before the next
        elif kind in ("call", "state", "steer", "error", "turn_end"):
            if cur is None or r.get("turn") != len(turns):
                bad("a %s record outside its turn" % kind)
                break
            if kind == "call":
                fields = [r.get(f) for f in ("id", "name", "args", "result")]
                if not all(isinstance(x, str) for x in fields) or not fields[0] or not fields[1] or r.get("n") != len(cur["calls"]) + 1:
                    bad("a call record without its number, id, name, arguments and result")
                    break
                if "state" in cur:
                    bad("a call record after the turn's state")
                    break
                cur["calls"].append({"id": fields[0], "name": fields[1], "args": fields[2], "result": fields[3], "at": r.get("mono")})
            elif kind == "state":
                st = r.get("state")
                if "state" in cur or not isinstance(st, dict) or not isinstance(st.get("now"), int):
                    bad("a state record that is not one record with its time")
                    break
                absent = st.get("absent") or []
                gone = [x for x in STORES if x in absent and x in seen]
                if gone: st["vanished"] = gone
                seen.update(x for x in STORES if x not in absent and x in st)
                cur["state"] = st
            elif kind == "steer":
                cur.setdefault("steered", []).append(str(r.get("text")))
            elif kind == "error":
                cur["error"] = str(r.get("message"))
            else:
                total, first, answer_, n, responses = r.get("total"), r.get("first"), r.get("answer"), r.get("tool_calls"), r.get("responses")
                if not isinstance(total, (int, float)) or not isinstance(answer_, str) or not (first is None or isinstance(first, (int, float))) \
                   or not isinstance(n, int) or not isinstance(responses, list):
                    bad("a turn end without its time, answer, count of calls and model responses")
                    break
                if "state" not in cur:
                    bad("turn %d ended without its saved state" % len(turns))
                    break
                if n != len(cur["calls"]):
                    bad("turn %d made %d tool calls but has %d call records" % (len(turns), n, len(cur["calls"])))
                    break
                shaped = all(isinstance(resp, list) and all(isinstance(x, dict) for x in resp) for resp in responses)
                asked = [{"id": x.get("id"), "name": x.get("name"), "args": x.get("args")} for resp in responses for x in resp] if shaped else None
                made = [{"id": x["id"], "name": x["name"], "args": x["args"]} for x in cur["calls"]]
                if not shaped or asked != made or any(len({x.get("id") for x in resp}) != len(resp) for resp in responses):
                    bad("turn %d's call records are not the calls its model responses asked for" % len(turns))
                    break
                cur.update(total=float(total), first=first, answer=plain(answer_), ended=r.get("mono"), ended_wall=r.get("wall"))
                cur = None
        elif kind == "end":
            if cur is not None or r.get("turns") != len(turns):
                bad("the end of the run inside a turn, or with another count of turns")
                break
            ended = True
        else:
            bad("an unknown record %r" % kind)
            break
    for t in turns:
        t["tools"] = [(x["name"], x["args"]) for x in t["calls"]]
        t["results"] = [x["result"] for x in t["calls"]]
    expected = sum(1 for t in script if t != "/new")
    if not reasons and process.get("exit") == 0 and not process.get("timeout") and (not ended or len(turns) != expected):
        reasons.append("the app exited normally, but its records stop after %d of %d turns" % (len(turns), expected))
    failed = "timeout" if process.get("timeout") else (None if process.get("exit") == 0 else "the app ended with status %s" % process.get("exit"))
    stopped = cur
    if stopped is None and len(turns) < expected and not reasons:
        stopped = _blank_turn()
        turns.append(stopped)
    return _stop(turns, expected, stopped, failed, process.get("seconds")), reasons

# The camera every case sees (ZB_FAKE_CAMERA; ZB_EVAL_CAMERA names another): a photo of a sign, so that
# nothing a test asks for ever reaches a real camera (ZB_EXP_NO_HARDWARE stops the rest).
CAMERA_FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mocks", "camera", "sign.jpg")

def camera_path():
    """The picture every case's camera gives (ZB_EVAL_CAMERA, else the fixture): the one run_case uses and the one
    fixtures_of hashes."""
    return os.environ.get("ZB_EVAL_CAMERA") or CAMERA_FIXTURE

# What a case's env may set, and to what (validate() and run_case refuse anything else): everything else is the
# runner's (the AI service, its key and model names, the search key, proxies, ZB_EXP_*, HOME and the XDG folders).
CASE_ENV = {"ZB_EXP_WEB": "a stand-in fixture, \"@web/NAME\"", "ZB_SEARCH_KEY": "null only (the case runs without the dummy key)",
            "ZB_DRY_RUN": "null only, for a home with a simulated device to reach", "ZB_DATE_ORDER": "\"dmy\" or \"mdy\""}

def case_env_problems(c):
    """What is wrong with the variables case c sets (CASE_ENV)."""
    out = []
    for k, v in (c.get("env") or {}).items():
        if k not in CASE_ENV: out.append("env %s is the runner's to set; a case may set only %s" % (k, ", ".join(sorted(CASE_ENV))))
        elif k == "ZB_EXP_WEB" and not (isinstance(v, str) and v.startswith("@web/") and v[5:]): out.append("env ZB_EXP_WEB must be \"@web/NAME\"")
        elif k in ("ZB_SEARCH_KEY", "ZB_DRY_RUN") and v is not None: out.append("env %s may only be null" % k)
        elif k == "ZB_DATE_ORDER" and v not in ("dmy", "mdy"): out.append("env ZB_DATE_ORDER must be dmy or mdy")
    return out

# What a case's env may not set: the runner's guards, applied after it (see run_case).
GUARDED = ("ZB_EXP_NO_HARDWARE", "ZB_FAKE_CAMERA", "ZB_EVAL_RECORDS", "ZB_EXP_TRACE", "XDG_CONFIG_HOME", "XDG_DATA_HOME",
           "XDG_CACHE_HOME", "HOME", "ZB_TEST_NETWORK", "PATH")

def device_home(home):
    """Whether a case's home has a simulated device that must really be reached (a Fire TV on 127.0.0.1): the
    only reason a case may run without ZB_DRY_RUN."""
    devices = [d for n in (home or {}).get("networks", {}).values() for d in n.get("devices", [])]
    return any(d.get("kind") == "fire_tv" and str(d.get("last_ip", "")).startswith("127.") for d in devices)

def run_case(binary, base_env, c):
    """Runs one case and returns what came of it: {"turns", "process": {"exit", "timeout", "seconds"},
    "evidence": "records" or "stdout", "unscorable": [why the evidence cannot be scored], "trace": whether
    the app was traced}. The runner's guards are applied last, over the case's env: no hardware, the fixture
    camera, the records file and a state folder of the case's own, and ZB_DRY_RUN unless the case's home
    has a simulated device it must reach."""
    # A fresh state directory per case: no calendar entries, memories or time zone carried over.
    if case_turns(c) != list(c["turns"]): c = dict(c, turns=case_turns(c))   # the held-out question forms
    state = tempfile.mkdtemp(prefix="zb-eval-")
    device_log = None
    os.makedirs(state + "/config/zero-bot")
    settings = {"home_city": "London", "user_name": "Sam"}
    for key in ("llm_model", "stt_model", "tts_model", "music_dir"):
        if os.environ.get("ZB_EVAL_" + key.upper()): settings[key] = os.environ["ZB_EVAL_" + key.upper()]
    json.dump(settings, open(state + "/config/zero-bot/settings.json", "w"))
    env = dict(base_env)
    records = state + "/records.jsonl"
    home = None
    if c.get("radio"):
        json.dump(c["radio"], open(state + "/config/zero-bot/radio.json", "w"))
    if c.get("home"):
        # The made-up home lives on the stand-in network, where only 127.x addresses could ever be
        # contacted; with ZB_DRY_RUN none is.
        os.makedirs(state + "/data/zero-bot")
        home = c["home"]() if callable(c["home"]) else c["home"]   # a home with a port made when it runs
        if isinstance(home, tuple): home, device_log = home      # and a simulated device whose log is kept
        json.dump(home, open(state + "/data/zero-bot/smarthome.json", "w"))
    refused = case_env_problems(c)
    if refused:
        shutil.rmtree(state, ignore_errors=True)
        raise RuntimeError("%s: refused: %s" % (c["name"], "; ".join(refused)))
    for k, v in c["env"].items():
        if k in ("ZB_EXP_WEB",) + WEB_KEYS: continue   # the web is set below
        if k == "ZB_DATE_ORDER": env[k] = v
        elif v is None and k != "ZB_DRY_RUN": env.pop(k, None)
    # The guards, last. No hardware beyond what the case simulates: camera and infrared refuse (experiment
    # builds), and the camera's photo is a fixture (ZB_EVAL_CAMERA, a runner's choice, may name another).
    env.update(XDG_CONFIG_HOME=state + "/config", XDG_DATA_HOME=state + "/data", XDG_CACHE_HOME=state + "/cache",
               ZB_EXP_NO_HARDWARE="1", ZB_FAKE_CAMERA=camera_path(), ZB_EVAL_RECORDS=records)
    # Every case on the stand-in network, home or not: the real one is never identified (no packet to the router).
    env["ZB_TEST_NETWORK"] = "1"
    # The web, last: every case on the stand-in (the fixture it names), with the dummy search key unless it
    # asks for none; a real key never. A case whose environment could reach a real web service is refused.
    env = case_web_env(env, c)
    # The network boundary outside the app: every proxy variable at the runner's trap, which refuses whatever
    # reaches it, and only 127.0.0.1, localhost and the AI service's host exempt.
    env = netguard.child_env(env, keep=("ZB_API_BASE", "ZB_API_KEY", "ZB_SEARCH_KEY"), allow_origin=env.get("ZB_API_BASE", ""))
    refused = web_guard(env, c["env"])
    if refused:
        shutil.rmtree(state, ignore_errors=True)
        raise RuntimeError("%s: refused, it could reach a real web service: %s" % (c["name"], "; ".join(refused)))
    if c["env"].get("ZB_DRY_RUN", "") is None and device_home(home): env.pop("ZB_DRY_RUN", None)
    else: env["ZB_DRY_RUN"] = "1"
    if "ZB_DRY_RUN" not in env:
        # A device is really contacted (on 127.0.0.1 only: ZB_TEST_NETWORK), and so would be whatever
        # else the model calls: the programs that change this computer are stand-ins that do nothing.
        stand_ins = state + "/bin"
        os.makedirs(stand_ins)
        for name in ("wpctl", "pactl", "amixer", "pw-cat", "pacat", "ffplay", "ffmpeg", "rpicam-jpeg", "nmcli", "iw", "systemctl"):
            with open(os.path.join(stand_ins, name), "w") as f: f.write("#!/bin/sh\nexit 0\n")
            os.chmod(os.path.join(stand_ins, name), 0o755)
        env["PATH"] = stand_ins + os.pathsep + env.get("PATH", "")
    # --trace: the experiments' trace (src/agent/experiment.h; cli build), one file per case: each turn's time
    # to its first sentence (what the user would hear first), and all of it into --json. Only when asked: the
    # trace asks the service for token counts, a change to what is sent. A trace asked for in the
    # environment (ZB_EXP_TRACE) is taken as asked for too, and recorded: the result says whether the app
    # was traced, whatever the command line said.
    trace = state + "/trace.jsonl" if base_env.get("ZB_EVAL_TRACE") or base_env.get("ZB_EXP_TRACE") else None
    if trace: env["ZB_EXP_TRACE"] = trace
    else: env.pop("ZB_EXP_TRACE", None)
    cmd = [binary, "--chat"] + (["--steer", c["steer"]] if c["steer"] else [])
    leaked_from = netguard.trap().count()
    t0 = time.time()
    try:
        p = subprocess.run(cmd, input="\n".join(c["turns"]) + "\n", env=env, capture_output=True, text=True,
                           timeout=90 * len(c["turns"]))
        out, process = p.stdout, {"exit": p.returncode, "timeout": False}
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode("utf-8", "replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        process = {"exit": None, "timeout": True}
    elapsed = process["seconds"] = time.time() - t0
    events = []
    if trace and os.path.exists(trace):
        for line in open(trace, encoding="utf-8", errors="replace"):
            try: events.append(json.loads(line))
            except ValueError: pass
    if os.path.exists(records):
        with open(records, encoding="utf-8", errors="replace") as f: turns, reasons = turns_from_records(f.read(), c, process)
        evidence = "records"
    else:
        failed = "timeout" if process["timeout"] else (None if process["exit"] == 0 else "the app ended with status %s" % process["exit"])
        turns, evidence = turns_from_output(out, c, failed, elapsed), "stdout"
        reasons = ["no records file (ZB_EVAL_RECORDS): a cli build of this version writes one"] if process["exit"] == 0 else []
    shutil.rmtree(state, ignore_errors=True)
    leaks = netguard.trap().since(leaked_from)
    if leaks:   # a request for a host outside 127.x: refused by the trap, and the run's evidence is not to be trusted
        reasons = list(reasons) + ["a request tried to leave 127.x and was refused (a web read not sent to the stand-in?): %s"
                                   % "; ".join(leaks[:3])]
    if device_log and turns: place_device_log(turns, device_log())
    for i, t in enumerate(turns):   # request n of the run is the n-th turn ("/new" starts none)
        t["web_seed"] = WEB_SEED   # what the stand-in served: the checks that read it work it out from this
        t["web_date"] = WEB_DATE   # ... and the date it counted from
        mine = [e for e in events if e.get("req") == i + 1]
        req = next((e for e in mine if e.get("ev") == "request"), None)
        if req and req.get("first_sentence_ms", -1) >= 0: t["first_sentence"] = req["first_sentence_ms"] / 1000.0
        if trace: t["trace"] = mine
    import uuid
    # run_id: this execution's own name, so that repeats are known to be distinct runs (the held-out judge checks).
    return {"turns": turns, "process": process, "evidence": evidence, "unscorable": reasons, "trace": bool(trace), "run_id": uuid.uuid4().hex}

def place_device_log(turns, log):
    """Sets a simulated device's log ([(wall-clock time, what)]) beside the turns by the records' wall times:
    each turn's "device_log" is what the device was sent while the turn ran (until its end, or for a turn
    that never ended, from its start on); what fell between turns goes to the last turn's
    "device_log_between". A turn without a start time gets no log, so a check of it is unscorable."""
    between = []
    for t in turns:
        if t.get("began_wall") is not None: t["device_log"] = []
    for at, what in log:
        mine = [t for t in turns if t.get("began_wall") is not None and t["began_wall"] - 0.05 <= at <= (t.get("ended_wall") or float("inf")) + 0.05]
        (mine[0]["device_log"] if mine else between).append(what)
    turns[-1]["device_log_between"] = between

UNSCORABLE = "unscorable: "

class UnscorableProblem(str):
    """A problem that makes a run unscorable rather than failed: the verdict goes by this type, never by the text
    (which starts with UNSCORABLE only so that a record says so to whoever reads it)."""

def unscorable(text):
    return UnscorableProblem(text if text.startswith(UNSCORABLE) else UNSCORABLE + text)
TURN_FIELDS = ("tools", "results", "answer", "total", "first", "error")

def verdict(c, run):
    """(outcome, problems) for one run of a case: "pass", "fail" or "unscorable". A run whose evidence
    cannot be trusted (see turns_from_records) is unscorable and no check is run on it. Otherwise the
    checks run; a check that needs evidence the run lacks makes a problem marked "unscorable: ", which
    never passes, but which also does not hide a definite failure elsewhere: any other problem fails."""
    reasons = list(run.get("unscorable") or [])
    turns = run.get("turns")
    if not isinstance(turns, list) or not turns:
        reasons.append("the result has no turns")
    else:
        for i, t in enumerate(turns):
            missing = [f for f in TURN_FIELDS if not isinstance(t, dict) or f not in t]
            if missing and not (isinstance(t, dict) and t.get("unstarted")):
                reasons.append("turn %d of the saved result has no %s" % (i + 1, ", ".join(missing)))
    if reasons: return "unscorable", [unscorable(r) for r in reasons]
    problems = score(c, turns)
    p = run.get("process") or {}
    if p.get("timeout") and not any(t.get("timed_out") or t.get("unstarted") for t in turns):
        problems.append("the run timed out after its last answer: the app did not exit")
    elif p.get("exit") not in (0, None) and not any(t.get("unstarted") or t["total"] is None for t in turns):
        problems.append("the app ended with status %s after its last answer" % p["exit"])
    definite = [x for x in problems if not isinstance(x, UnscorableProblem)]
    return ("fail" if definite else "unscorable" if problems else "pass"), problems

def run_from_saved(r, c=None):
    """A result saved by --json, as run_case returns one, for --rescore. It keeps where it came from (its
    requests, case_hash, fixtures, trace and evidence; result_record writes those, not the current case's),
    so that scoring old evidence again never gives it a new identity; a saved run whose requests are not
    case c's is unscorable. Results saved before a field existed are scored as they are: what they lack
    makes their checks unscorable."""
    why = list(r.get("unscorable_why", r.get("unscorable") if isinstance(r.get("unscorable"), list) else []) or [])
    if c is not None and r.get("requests") != case_turns(c):
        why.append("the saved run asked %r, not this case's %r" % (r.get("requests"), case_turns(c)))
    return {"turns": r.get("turns"), "process": r.get("process"), "evidence": r.get("evidence", "stdout"), "unscorable": why,
            "trace": r.get("trace"),
            "run_id": r.get("run_id"),
            "provenance": {k: r.get(k) for k in ("requests", "case_hash", "fixtures", "trace", "web_seed", "web_date")}}

def trace_summary(results):
    """One line from the traces of a --trace run: model calls, tokens and upload per request."""
    reqs = [e for r in results for t in r["turns"] for e in t.get("trace", []) if e.get("ev") == "request"]
    if not reqs: return "trace: none recorded"
    calls = {}
    for r in results:
        for t in r["turns"]:
            for e in t.get("trace", []):
                if e.get("ev") == "call": calls.setdefault((id(t), e["req"]), []).append(e)
    per = list(calls.values())
    tok = sorted(sum((c.get("usage") or {}).get("prompt_tokens", 0) for c in cs) for cs in per)
    cached = sum(((c.get("usage") or {}).get("prompt_tokens_details") or {}).get("cached_tokens", 0) for cs in per for c in cs)
    sent = sum(tok)
    up = sorted(sum(c.get("body_bytes", 0) for c in cs) for cs in per)
    n = sorted(len(cs) for cs in per)
    med = lambda v: v[len(v) // 2] if v else 0
    return ("trace: %d requests | model calls per request median %d, mean %.2f | prompt tokens per request median %d, cached %.0f%% "
            "| upload per request median %.1f kB" % (len(reqs), med(n), sum(n) / max(1, len(n)), med(tok),
                                                     100.0 * cached / sent if sent else 0, med(up) / 1000.0))


def timing_summary(results):
    """Completion first, then each latency with how many turns it was measured over: total and first words
    over the turns that ran (a turn that timed out counts once, with the time it had; turns never started
    count only in the completion), and the time to the first sentence (what would be spoken first; from
    the trace). A run that timed out after its last answer counts as a timeout too."""
    turns = [t for r in results for t in r["turns"]]
    ran = [t for t in turns if not t.get("unstarted")]
    totals = sorted(t["total"] for t in ran if t["total"] is not None)
    firsts = sorted(t["first"] for t in ran if t.get("first") is not None)
    sentences = sorted(t["first_sentence"] for t in ran if t.get("first_sentence") is not None)
    completed = sum(1 for t in ran if t["total"] is not None and not t.get("timed_out") and t.get("answer"))
    timeouts = sum(1 for r in results if (r.get("process") or {}).get("timeout") or any(t.get("timed_out") for t in r["turns"]))
    med = lambda v: v[len(v) // 2] if v else float("nan")
    p90 = lambda v: v[min(len(v) - 1, int(len(v) * 0.9))] if v else float("nan")
    done = "completed %d/%d turns" % (completed, len(turns))
    return ("completed %d/%d turns, %d timeout%s, %d not started | total (%s): median %.1fs, mean %.1fs, 90th %.1fs, worst %.1fs | "
            "first words (%d turns; %s): median %.1fs, 90th %.1fs | first sentence: %s"
            % (completed, len(turns), timeouts, "" if timeouts == 1 else "s", len(turns) - len(ran),
               "%d turns; %s" % (len(totals), done), med(totals), sum(totals) / max(1, len(totals)), p90(totals), totals[-1] if totals else 0,
               len(firsts), done, med(firsts), p90(firsts),
               "%d turns; %s: median %.1fs, 90th %.1fs" % (len(sentences), done, med(sentences), p90(sentences)) if sentences else "with --trace"))

def score(c, turns):
    """Problems with one run's turns; "unscorable: " starts those that a lack of evidence caused."""
    global SCORING_AT
    began = [t.get("began_wall") for t in turns if t.get("began_wall") is not None]
    SCORING_AT = began[-1] if began else None
    try:
        return _score(c, turns)
    finally:
        SCORING_AT = None


def _score(c, turns):
    problems = []
    # Turns whose answer may run long (analytical follow-ups; "long_ok" in the suite's cases).
    long_ok = {i % len(turns) for i in c.get("long_ok", ())} if turns else set()
    strict = any(k == "ZB_EXP_WEB" and v for k, v in c.get("env", {}).items())
    asked = [x for x in case_turns(c) if x != "/new"]
    for i, t in enumerate(turns):
        t["answer"] = plain(t.get("answer") or "")
        if strict: t["strict_reads"] = True
        if i < len(asked): t["request"] = asked[i]   # what this turn asked (a weather check reads the day from it)
    annotate_served(c, turns)
    for i, t in enumerate(turns):
        where = "" if len(turns) == 1 else "turn %d: " % (i + 1)
        if t.get("unstarted"):
            problems.append(where + (t["error"] or "not started"))
            continue
        a = t["answer"]
        if t["error"]: problems.append(where + "error: " + t["error"][:80])
        if not a: problems.append(where + "empty answer")
        if re.search(r"[*#`]|^\s*[-•]\s|^\s*\d+[.)]\s", a, re.M): problems.append(where + "markdown in answer")
        if re.search(r"</?think", a): problems.append(where + "reasoning marker in answer")
        if EMOJI_RE.search(a): problems.append(where + "emoji in answer")
        if re.search(r"https?://|www\.", a): problems.append(where + "web address in answer")
        if len(a) > 420 and i not in long_ok: problems.append(where + "answer too long (%d chars)" % len(a))
        if CLAIM_RE.search(a) and not c.get("quotes_ok"):
            res = t["results"]
            made = [(n, res[k] if k < len(res) else None) for k, (n, _) in enumerate(t["tools"])]
            changes = [r for (n, r), (_, a) in zip(made, t["tools"]) if is_change(n, a)]
            if not made: problems.append(where + "claims an action but called no tool")
            elif changes and not any(worked(r) for r in changes):
                # A change was tried: only a change that worked backs the claim (a read beside it does not).
                problems.append(where + "claims an action but no change worked: %s" % [str(r)[:80] for r in changes])
            elif not changes and not any(worked(r) for _, r in made):
                problems.append(where + "claims an action but no tool call worked: %s" % [str(r)[:80] for _, r in made])
    last = turns[-1]
    res = last["results"]
    made = [(n, res[k] if k < len(res) else None) for k, (n, _) in enumerate(last["tools"])]
    # must: called, and for a change, worked (the case is about its effect); tried: called, whatever came of
    # it (routing, and the cases about a failure or a question first).
    for want in c["must"]:
        alts = want.split("|")
        if not any(n in alts for n, _ in made): problems.append("did not call " + want)
        elif not any(n in alts and counts(last, n, r, a) for (n, r), (_, a) in zip(made, last["tools"])): problems.append("no call to %s worked: %s" % (want, tried(last, want)))
    for want in c.get("tried", ()):
        if not any(n in want.split("|") for n, _ in made): problems.append("did not call " + want)
    for bad in c["must_not"]:
        if bad == "*" and made: problems.append("called tools when none were needed: " + ",".join(n for n, _ in made))
        elif bad in [n for n, _ in made]: problems.append("should not call " + bad)
    for check in c["checks"]:
        # A check looks at the last turn, or at the one it names (its "turn" attribute: 0 is the first),
        # or at every turn ("all"); a whole-conversation check ("whole") gets the list of turns.
        n = getattr(check, "turn", None)
        where = "" if n is None or n == "all" else "turn %d: " % (n % len(turns) + 1) if -len(turns) <= n < len(turns) else ""
        try:
            if getattr(check, "whole", False):
                if any(t.get("unstarted") for t in turns): raise Unscorable("a turn of the conversation never started")
                problems += check(turns)
            elif n == "all":
                for i, t in enumerate(turns):
                    if t.get("unstarted"): continue
                    try: problems += [_at("turn %d: " % (i + 1), p) for p in check(t)]
                    except Unscorable as e: problems.append(unscorable("turn %d: %s" % (i + 1, e)))
            elif n is not None and not -len(turns) <= n < len(turns):
                problems.append("a check of turn %d, which did not happen" % (n + 1))
            else:
                t = last if n is None else turns[n]
                if t.get("unstarted"): continue   # reported above, once
                problems += [_at(where, p) for p in check(t)]
        except Unscorable as e: problems.append(unscorable(where + str(e)))
        except Exception as e: problems.append(unscorable(where + "the check could not run: %r" % e))
    return problems

def _at(where, p):
    """A check's problem labelled with its turn, keeping an unscorable one marked as such (the mark comes first)."""
    return unscorable(where + p[len(UNSCORABLE):]) if isinstance(p, UnscorableProblem) else where + p

# ---------------------------------------------------------------- what a result can be compared on
def _fingerprint(x, seen=None):
    """A stable description of a check or a home: a function's source and the values it closes over (other
    functions in turn), a dict or list as JSON. Python's version does not change it; editing the check does."""
    import inspect
    seen = seen if seen is not None else set()
    if callable(x) and hasattr(x, "__code__"):
        if id(x) in seen: return "<recursion>"
        seen.add(id(x))
        try: src = inspect.getsource(x)
        except (OSError, TypeError): src = x.__qualname__
        cells = [_fingerprint(cell.cell_contents, seen) for cell in (x.__closure__ or ()) if _has_contents(cell)]
        return {"src": src, "cells": cells, "turn": getattr(x, "turn", None), "whole": getattr(x, "whole", False)}
    if isinstance(x, dict): return {str(k): _fingerprint(v, seen) for k, v in x.items()}
    if isinstance(x, (list, tuple, set)): return [_fingerprint(v, seen) for v in (sorted(x, key=repr) if isinstance(x, set) else x)]
    if isinstance(x, (str, int, float, bool)) or x is None: return x
    return repr(x)

def _has_contents(cell):
    try: cell.cell_contents
    except ValueError: return False
    return True

def _sha1(data):
    import hashlib
    return hashlib.sha1(data if isinstance(data, bytes) else data.encode()).hexdigest()

def fixtures_of(c):
    """What the fixtures a case runs against hold, each as a sha1: the music library and the camera's
    picture, the stand-in web (its code and data: every case runs against it) and its seed, the simulated Fire TV (a case whose home starts one)."""
    import inspect
    here = os.path.dirname(os.path.abspath(__file__))
    out = {"music": _sha1(json.dumps(MUSIC_LIBRARY)), "camera": _sha1(open(camera_path(), "rb").read()), "web": web_version(),
           "web_seed": WEB_SEED}
    workload = date_workload(c)
    if workload: out["date_workload"] = workload
    home = c.get("home")
    if callable(home):
        try: src = inspect.getsource(home)
        except (OSError, TypeError): src = ""
        if "mock_firetv" in src: out["firetv"] = _sha1(open(os.path.join(here, "mocks", "firetv", "mock_firetv.py"), "rb").read())
    return out

def date_workload(c):
    """What the answers of a case depend on in the calendar of the run's date (WEB_DATE; today before a run), for the
    few cases whose stand-in answers move with it: a weather request for a weekday or the weekend (how many days
    ahead that day is: the forecast is drawn by days from today) and the published event of the "events" fixture
    (its time in London, which daylight saving moves). None for every other case, whose answers are the same on any
    day, so that a baseline and a variant from different days still pair; these pair only on a day like it."""
    day = datetime.date.fromisoformat(WEB_DATE) if WEB_DATE else today().date()
    out = {}
    asked = " ".join(x for x in case_turns(c) if x != "/new").lower()
    tools = {t for m in list(c.get("must", [])) + list(c.get("tried", [])) for t in m.split("|")}
    if "weather" in tools:
        named = [wd for wd, name in enumerate(WEEKDAYS) if re.search(r"\b%s\b" % name, asked)]
        if re.search(r"\bweekend\b", asked): named += [5, 6]
        if named: out["weekday_days_ahead"] = sorted({(wd - day.weekday()) % 7 for wd in named})
    raw = c.get("source", c)
    kinds = {x.get("kind") for x in raw.get("checks", []) if isinstance(x, dict)}
    if web_fixture(c) == "events" and ("event_time" in kinds or re.search(r"apple|launch|keynote", asked)):
        from zoneinfo import ZoneInfo   # the launch's time, only for the case about the launch (not the Discord one)
        when = _fake_web().event_time("product_launch", day).astimezone(ZoneInfo("Europe/London"))
        out["event_in_london"] = when.strftime("%H:%M")
    # A search rule whose answer counts days from today (the Bulls' next game, a strike, last night's score): the
    # weekdays those days fall on.
    fw = _fake_web()
    rules = sorted(r for r in {x.get("rule") for x in raw.get("checks", []) if isinstance(x, dict) and x.get("kind") == "served"} |
                   {getattr(y, "rule", None) for x in raw.get("checks", []) if callable(x)
                    for y in [x] + [cell.cell_contents for cell in (getattr(x, "__closure__", None) or ()) if _has_contents(cell)]} if r)
    for rid in [r for r in rules if r in fw.ALL_RULES]:
        offsets = sorted({int(n) for n in re.findall(r"\{day:([+-]?\d+)\}", json.dumps(fw.ALL_RULES[rid]))})
        if offsets: out.setdefault("search_days", {})[rid] = [(day + datetime.timedelta(days=n)).strftime("%a") for n in offsets]
    return out or None

def web_version():
    """The stand-in web every case runs against: its code and every data file beside it (tests/mocks/web/)."""
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mocks")
    data = os.path.join(here, "web")
    parts = [open(os.path.join(here, "fake_web.py"), "rb").read()] + \
            [name.encode() + open(os.path.join(data, name), "rb").read() for name in sorted(os.listdir(data))]
    return _sha1(b"\0".join(parts))

def fixture_version(c):
    """One short string for fixtures_of(c), as results carry it ("fixtures")."""
    return _sha1(json.dumps(fixtures_of(c), sort_keys=True))[:12]

def case_hash(c):
    """sha1 of the case as it is scored: its turns, checks, env, home, radio stations, must and must_not
    (and steer, tried, long_ok and quotes_ok), and what its fixtures hold (fixtures_of). Two results are
    comparable only when this and SCORER_VERSION agree. A suite case's checks are its JSON; a scored-set
    case's are fingerprints of the check functions."""
    import hashlib
    raw = c.get("source", c)
    checks = raw["checks"] if all(isinstance(x, dict) for x in raw["checks"]) else [_fingerprint(f) for f in raw["checks"]]
    home = raw.get("home")
    if isinstance(home, str): home = c.get("home")   # a suite case names its home; the home itself counts
    body = {"turns": raw["turns"], "held_out_turns": raw.get("held_out_turns"), "checks": checks, "env": raw.get("env") or {}, "home": _fingerprint(home), "radio": raw.get("radio"),
            "must": raw.get("must", []), "must_not": raw.get("must_not", []), "steer": raw.get("steer"), "tried": raw.get("tried", []),
            "long_ok": raw.get("long_ok", []), "quotes_ok": raw.get("quotes_ok", False), "fixtures": fixtures_of(c)}
    return hashlib.sha1(json.dumps(body, sort_keys=True, ensure_ascii=False, default=repr).encode()).hexdigest()

def result_record(c, run, outcome, problems, traced=None, which="scored", **extra):
    """What --json keeps of one run: the verdict ("outcome"; "unscorable" is true for an unscorable run, with
    the reasons in "problems"), the evidence it rests on ("unscorable_why": what made the records themselves
    unusable, kept for --rescore), and what makes it comparable: case_hash, fixtures, scorer, trace and the
    set it belongs to ("set": "scored" or "suite"). "trace" is whether the app was traced (run_case says;
    `traced` only for a run that does not). A run scored again (run_from_saved) keeps the requests,
    case_hash, fixtures and trace it was made with."""
    prov = run.get("provenance")
    trace = run.get("trace") if run.get("trace") is not None or prov else traced
    return dict({"case": c["name"], "set": which, "case_hash": prov["case_hash"] if prov else case_hash(c),
                 "fixtures": prov["fixtures"] if prov else fixture_version(c), "scorer": SCORER_VERSION,
                 "web_seed": prov.get("web_seed") if prov else WEB_SEED,
                 "web_date": prov.get("web_date") if prov else WEB_DATE, "run_id": run.get("run_id"),
                 "trace": None if trace is None else bool(trace), "outcome": outcome, "unscorable": outcome == "unscorable",
                 "requests": prov["requests"] if prov else case_turns(c), "problems": problems, "turns": run["turns"],
                 "process": run.get("process"), "evidence": run.get("evidence"), "unscorable_why": run.get("unscorable") or []}, **extra)

def probe_binary(binary):
    """Why a binary cannot run the live sets ([] when it can): it must be an experiments build that writes
    the records file, of this version, and says it refuses hardware (ZB_EXP_NO_HARDWARE) and sends every
    tool's web read to the stand-in web ("web_all": an older build sends rates and prices to the real
    services). It is run once with no request, in an empty home of its own, with dry run on."""
    folder = tempfile.mkdtemp(prefix="zb-probe-")
    try:
        path = os.path.join(folder, "records.jsonl")
        env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": folder, "XDG_CONFIG_HOME": folder + "/config",
               "XDG_DATA_HOME": folder + "/data", "XDG_CACHE_HOME": folder + "/cache", "ZB_EVAL_RECORDS": path,
               "ZB_DRY_RUN": "1", "ZB_NO_AUDIO": "1", "ZB_EXP_NO_HARDWARE": "1"}
        env = netguard.child_env(env, ZB_TEST_NETWORK="1")
        try: p = subprocess.run([binary, "--chat"], input="", env=env, capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.TimeoutExpired) as e: return ["%s did not run: %s" % (binary, e)]
        if p.returncode != 0: return ["%s --chat ended with status %d" % (binary, p.returncode)]
        if not os.path.exists(path): return ["%s writes no records file (ZB_EVAL_RECORDS): build the cli preset of this checkout" % binary]
        try: recs = [json.loads(l) for l in open(path, encoding="utf-8") if l.strip()]
        except ValueError: return ["%s wrote records that are not JSON" % binary]
        first = recs[0] if recs and isinstance(recs[0], dict) else {}
        caps = first.get("caps") if isinstance(first.get("caps"), list) else []
        problems = []
        if first.get("v") != RECORDS_VERSION or first.get("ev") != "start": problems.append("%s writes records of another version" % binary)
        problems += ["%s has no %s" % (binary, x) for x in netguard.REQUIRED_CAPS if x not in caps]
        if not recs or recs[-1].get("ev") != "end": problems.append("%s did not write the end of its run" % binary)
        return problems
    finally:
        shutil.rmtree(folder, ignore_errors=True)

# ---------------------------------------------------------------- the cases checked before a run
def validate(cases):
    """Problems with the cases as written: names, turns, tools, env, the turn each check looks at, and the
    stand-in web's fixtures. Run with --validate; no service needed."""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "mocks"))
    sys.dont_write_bytecode = True   # no __pycache__ beside the mocks
    import fake_web
    errors, names = [], set()
    for c in cases:
        name = c.get("name")
        def err(why): errors.append("%s: %s" % (name, why))
        if not isinstance(name, str) or not name: err("no name")
        elif name in names: err("the name is used twice")
        names.add(name)
        turns = c.get("turns")
        if not isinstance(turns, list) or not turns or not all(isinstance(t, str) and t.strip() for t in turns):
            err("turns must be a list of requests")
            continue
        n = sum(1 for t in turns if t != "/new")
        if n == 0 or turns[-1] == "/new": err("the last turn must be a request")
        for key in ("must", "must_not", "tried"):
            v = c.get(key, [])
            if not isinstance(v, list) or not all(isinstance(x, str) and re.fullmatch(r"[a-z_*]+(\|[a-z_]+)*", x) for x in v):
                err("%s must be a list of tool names (a|b for either)" % key)
        if not isinstance(c.get("checks"), list): err("checks must be a list")
        for check in c.get("checks", []):
            turn = getattr(check, "turn", None)
            if turn is not None and turn != "all" and not (isinstance(turn, int) and -n <= turn < n):
                err("a check of turn %r; the case has %d" % (turn, n))
        for problem in case_env_problems(c): err(problem)
        for k, v in (c.get("env") or {}).items():
            if k in GUARDED: err("env %s is the runner's to set (a guard); the case may not" % k)
            if k == "ZB_DRY_RUN" and v is None and not (callable(c.get("home")) or device_home(c.get("home"))):
                err("only a case whose home has a simulated device to reach may run without ZB_DRY_RUN")
            if not (v is None or isinstance(v, str)): err("env %s must be text or null" % k)
            if k == "ZB_EXP_WEB" and not (isinstance(v, str) and v.startswith("@web/") and v[5:] in fake_web.FIXTURES):
                err("env %s must name a stand-in fixture (\"@web/NAME\"): %r" % (k, v))
            if k in WEB_KEYS and v not in (None, SEARCH_DUMMY): err("env %s may only be null (no key) or the dummy %r" % (k, SEARCH_DUMMY))
        if (c.get("env") or {}).get("ZB_DRY_RUN", "") is None and c.get("radio"):
            err("a case that runs without ZB_DRY_RUN may not have radio stations (a station is checked over the internet)")
        if c.get("held_out_turns") is not None:
            h = c["held_out_turns"]
            if not (isinstance(h, list) and all(isinstance(x, str) and x.strip() for x in h) and
                    [x == "/new" for x in h] == [x == "/new" for x in turns]):
                err("held_out_turns must be as many requests as turns, in the same places")
        for i in c.get("long_ok", []):
            if not (isinstance(i, int) and -n <= i < n): err("long_ok names turn %r; the case has %d" % (i, n))
    errors += ["stand-in web: %s" % p for p in fake_web.pool_problems()]   # the held-out set kept apart (W9)
    return errors

# The music library every case sees (both sets): the artists, titles and genres the cases ask for, plus a
# few others to choose from. Each file holds an ID3v2.3 tag and no sound (ZB_DRY_RUN plays nothing), so a
# run never reads the music of whoever runs it. ZB_EVAL_MUSIC_DIR names another folder; set empty, the
# library is empty, as the runs before 2026-11-19 had.
MUSIC_LIBRARY = [
    ("ABBA", "Gold", "Pop", ["Dancing Queen", "Waterloo", "Mamma Mia"]),
    ("Queen", "A Night at the Opera", "Rock", ["Bohemian Rhapsody", "You're My Best Friend"]),
    ("Queen", "Jazz", "Rock", ["Don't Stop Me Now"]),
    ("The Beatles", "Abbey Road", "Rock", ["Here Comes the Sun", "Come Together"]),
    ("Pink Floyd", "Wish You Were Here", "Rock", ["Wish You Were Here", "Shine On You Crazy Diamond"]),
    ("Fleetwood Mac", "Rumours", "Rock", ["Dreams", "Go Your Own Way"]),
    ("Miles Davis", "Kind of Blue", "Jazz", ["So What", "Blue in Green"]),
    ("Ella Fitzgerald", "Ella Sings Gershwin", "Jazz", ["Summertime", "Someone to Watch Over Me"]),
    ("Brian Eno", "Ambient 1: Music for Airports", "Ambient", ["1/1", "2/1"]),
    ("Ludovico Einaudi", "Una Mattina", "Classical", ["Nuvole Bianche", "Una Mattina"]),
]

def make_music_library(folder):
    def frame(fid, text):
        body = b"\x00" + text.encode("latin-1")
        return fid.encode() + len(body).to_bytes(4, "big") + b"\x00\x00" + body
    for artist, album, genre, titles in MUSIC_LIBRARY:
        d = os.path.join(folder, artist, album)
        os.makedirs(d, exist_ok=True)
        for n, title in enumerate(titles, 1):
            tag = b"".join(frame(f, v) for f, v in (("TIT2", title), ("TPE1", artist), ("TALB", album), ("TCON", genre)))
            size = bytes([(len(tag) >> 21) & 0x7f, (len(tag) >> 14) & 0x7f, (len(tag) >> 7) & 0x7f, len(tag) & 0x7f])
            name = "%02d %s.mp3" % (n, title.replace("/", "-"))
            with open(os.path.join(d, name), "wb") as f:
                f.write(b"ID3\x03\x00\x00" + size + tag)
    return folder

def isolated_env(home):
    """The environment every case of a run starts from: side effects off (ZB_DRY_RUN, ZB_NO_AUDIO), an empty
    home folder of the run's own (so nothing under the home of whoever runs it is read), no web-service key
    (WEB_KEYS), and the stand-in music library unless ZB_EVAL_MUSIC_DIR names one."""
    if "ZB_EVAL_MUSIC_DIR" not in os.environ:
        os.environ["ZB_EVAL_MUSIC_DIR"] = make_music_library(os.path.join(home, "Music"))
    env = dict(os.environ, ZB_DRY_RUN="1", ZB_NO_AUDIO="1", HOME=home)
    for k in WEB_KEYS + ("ZB_EXP_WEB",): env.pop(k, None)   # run_case sets the stand-in's own, never these
    env["ZB_FAKE_CAMERA"] = camera_path()   # an inherited ZB_FAKE_CAMERA is not the picture the hash names
    return env

def outcome_line(results):
    """"N/M passed" with the failures and the unscorable runs counted apart (an unscorable run is no pass)."""
    n = len(results)
    ok = sum(1 for r in results if r["outcome"] == "pass")
    unscorable = sum(1 for r in results if r["outcome"] == "unscorable")
    return "%d/%d passed (%.0f%%), %d failed, %d unscorable" % (ok, n, 100.0 * ok / max(1, n), n - ok - unscorable, unscorable)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="build/cli/zero_bot")
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--only", default="", help="run cases whose name contains this (comma-separated alternatives)")
    ap.add_argument("--fast", default=None, help="force fast mode on (1) or off (0)")
    ap.add_argument("--jobs", type=int, default=1, help="cases to run at once (timings are only comparable at 1)")
    ap.add_argument("--json", default="", help="write full results to this file")
    ap.add_argument("--rescore", default="", help="score a results file written by --json again instead of running (judged at the time each run happened; results saved before the records file: the same day only)")
    ap.add_argument("--trace", action="store_true", help="record the experiments' trace (cli build, ZB_EXP_TRACE) into each turn of --json")
    ap.add_argument("--validate", action="store_true", help="check the cases as written and exit (non-zero on any problem)")
    ap.add_argument("--web-seed", type=int, default=None, help="the stand-in web's seed (default ZB_EVAL_SEED, else %d)" % EVERYDAY_SEED)
    ap.add_argument("--held-out", action="store_true", help="the release profile's held-out run: a seed from %d up (default %d)"
                    % (HELD_OUT_FROM, HELD_OUT_SEED))
    args = ap.parse_args()
    if not args.validate:
        set_web_seed(args.web_seed if args.web_seed is not None else HELD_OUT_SEED if args.held_out else WEB_SEED, args.held_out)
    if args.validate:
        errors = validate(CASES)
        for e in errors: print("INVALID " + e)
        print("%d cases, %d problems" % (len(CASES), len(errors)))
        sys.exit(1 if errors else 0)
    saved = {}
    if args.rescore:
        for r in json.load(open(args.rescore)): saved.setdefault(r["case"], []).append(r)
    elif not os.environ.get("ZB_API_BASE") or not os.environ.get("ZB_API_KEY"):
        sys.exit("ZB_API_BASE and ZB_API_KEY must be set")
    else:
        unfit = probe_binary(args.bin)
        if unfit: sys.exit("the binary cannot run the live cases: " + "; ".join(unfit))

    home = tempfile.mkdtemp(prefix="zb-eval-home-")
    env = isolated_env(home)
    if args.fast is not None: env["ZB_FAST_MODE"] = args.fast
    if args.trace: env["ZB_EVAL_TRACE"] = "1"

    wanted = [w for w in args.only.split(",") if w]
    cases = [c for c in CASES if not wanted or any(w in c["name"] for w in wanted)]
    if not args.rescore:
        refused = preflight(env, cases, validate(cases))
        if refused:
            shutil.rmtree(home, ignore_errors=True)
            sys.exit("refused: a case could reach a real web service or carry a real key:\n  " + "\n  ".join(refused))
    jobs = [c for c in cases for _ in range(len(saved.get(c["name"], [])) if args.rescore else args.repeat)]
    run = (lambda c: run_from_saved(saved[c["name"]].pop(0), c)) if args.rescore else (lambda c: run_case(args.bin, env, c))
    traced = args.trace if not args.rescore else None
    results, failed_names = [], {}
    with concurrent.futures.ThreadPoolExecutor(max(1, args.jobs)) as pool:
        for c, got in zip(jobs, pool.map(run, jobs)):
            outcome, problems = verdict(c, got)
            if outcome != "pass": failed_names[c["name"]] = failed_names.get(c["name"], 0) + 1
            turns = got["turns"] or []
            for i, t in enumerate(turns):
                shown = ", ".join("%s%s" % (n, a if len(a) < 70 else a[:67] + "...") for n, a in t.get("tools", [])) or "-"
                first = "%.1f" % t["first"] if t.get("first") is not None else " - "
                tag = {"pass": "PASS", "fail": "FAIL", "unscorable": "????"}[outcome] if i == len(turns) - 1 else "    "
                print("%-4s %-21s first %4ss total %5.1fs  [%s]" % (tag, c["name"], first, t.get("total") or 0, shown))
                print("       \"%s\"" % str(t.get("answer", ""))[:220].replace("\n", " / "))
            for p in problems: print("       !! " + p)
            sys.stdout.flush()
            results.append(result_record(c, got, outcome, problems, traced))
    shutil.rmtree(home, ignore_errors=True)
    print("\n%s | %s" % (outcome_line(results), timing_summary(results)))
    print("stand-in web: seed %d%s, fixtures %s" % (WEB_SEED, " (held out)" if WEB_SEED >= HELD_OUT_FROM else "", web_version()[:12]))
    if args.trace: print(trace_summary(results))
    if failed_names:
        print("not passed: " + ", ".join("%s%s" % (k, " x%d" % v if args.repeat > 1 else "") for k, v in sorted(failed_names.items())))
    if args.json: json.dump(results, open(args.json, "w"), indent=1)
    sys.exit(1 if failed_names else 0)

if __name__ == "__main__":
    main()
