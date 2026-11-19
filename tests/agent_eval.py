#!/usr/bin/env python3
"""Runs a set of spoken-style requests through the command-line agent and scores them.

Each case names the tools that should (or should not) be called, what the tool arguments
and the answer must contain, and optional follow-up turns. Expected dates, times and
amounts are computed when the case runs (from today's date or from what the tools
returned), never hard-coded, so a confident wrong number fails. The script reports
pass/fail, tool calls, timings and averages, so changes to prompts, tool descriptions or
the loop can be compared run against run.

Usage:
    . path/to/env.sh            # sets ZB_API_BASE, ZB_API_KEY, optional ZB_SEARCH_KEY
    tests/agent_eval.py [--bin build/cli/zero_bot] [--repeat N] [--only substring[,substring]]
                        [--fast 0|1] [--jobs N] [--json out.json] [--rescore out.json] [--trace]

--trace records the experiments' trace (src/agent/experiment.h, cli build) of every turn into
--json: model calls with their timings and token counts, tools, guards. tests/exp_report.py
summarises such files. --validate checks the cases as written and runs nothing.

Side effects are disabled (ZB_DRY_RUN): no music plays, no infrared is sent and no smart-home
device is switched. Every case starts from an empty calendar, memory and settings, in a home folder
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

def says_time(text, when, slack_min=2):
    """True when the answer states the clock time `when` (24-hour or 12-hour), give or take slack. An am or
    pm that is said must be the right one: "3:00 am" is not 15:00."""
    for d in range(-slack_min, slack_min + 1):
        t = when + datetime.timedelta(minutes=d)
        h12 = t.hour % 12 or 12
        for pat in (r"\b0?%d[:.]%02d\b" % (t.hour, t.minute), r"\b%d[:.]%02d\b" % (h12, t.minute)):
            if any(right_half(text, m.end(), t.hour) for m in re.finditer(pat, text)): return True
        if t.hour % 12 == 0 and t.minute == 0 and re.search(r"midnight" if t.hour == 0 else r"noon|midday", text, re.I): return True
        if t.minute == 0:
            for m in re.finditer(r"\b(%d|%s)\s*(o['’]?clock\b|(?=a\.?\s?m\b|p\.?\s?m\b|(?:in the|this|tomorrow) (?:morning|afternoon|evening)|tonight|at night))" % (h12, ONES[h12]), text, re.I):
                if right_half(text, m.end(), t.hour): return True
    return False

def today(): return datetime.datetime.now()

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

def counts(turn, name, result):
    """Whether a call to `name` with this result counts as made (see CHANGES)."""
    return worked(result) or (name not in CHANGES and not turn.get("strict_reads"))

def calls(turn, name):
    """Parsed arguments of the calls to `name` (or any of a|b) in this turn that did what they asked: a
    change whose result is an ERROR, or that waits for a yes, did not."""
    return [a for (a, r), n in zip(_made(turn, name), [n for n, _ in turn["tools"] if n in name.split("|")])
            if counts(turn, n, r)]

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
# files hold. A missing file is an empty list there; a file the app could not read is null and named in
# "unreadable". A check that needs the state of a turn without one (a saved run from before the record, a
# build without it), or a store that could not be read, is unscorable: never a pass.
def state(turn):
    st = turn.get("state")
    if st is None: raise Unscorable("no saved state was recorded for this turn")
    if not isinstance(st, dict): raise Unscorable("the saved state is not a record: %r" % (st,))
    return st

def _store(st, name):
    if name in (st.get("unreadable") or []): raise Unscorable("the app could not read its %s file" % name)
    if name not in st: raise Unscorable("the saved state has no %s" % name)
    items = st[name]
    if not isinstance(items, list) or not all(isinstance(x, dict) for x in items):
        raise Unscorable("the saved %s is not a list of records" % name)
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
    return [] if says_about(t["answer"], share, 0.06) else ["answer should give a third of the converted amount, %.2f" % share]

def chk_two_currencies(t):
    got = {str(r.get("to")): r for r in results(t, "convert_currency") if isinstance(r, dict)}
    problems = []
    for code in ("EUR", "JPY"):
        if code not in got: problems.append("no convert_currency call to " + code)
        elif not says_about(t["answer"], 200 * got[code]["rate"], 200 * got[code]["rate"] * 0.006):
            problems.append("answer should give %.2f %s" % (200 * got[code]["rate"], code))
    return problems

def chk_tip(t):
    res = [r for r in results(t, "convert_currency") if isinstance(r, dict) and "rate" in r]
    if not res: return ["no usable convert_currency result"]
    want = 64 * 1.15 * res[0]["rate"]
    return [] if says_about(t["answer"], want, 0.06) else ["answer should give %.2f pounds" % want]

def chk_stock(t):
    res = [r for r in results(t, "stock_price") if isinstance(r, dict) and r.get("price")]
    if not res: return ["no usable stock_price result"]
    # A spoken price is rounded ("about 62,500" for 62,497.44): half a per cent, at least 1.
    price = res[0]["price"]
    return [] if says_about(t["answer"], price, max(1.0, abs(price) * 0.005)) else ["answer should give the price %.2f" % price]

def chk_tokyo(t):
    from zoneinfo import ZoneInfo
    return [] if says_time(t["answer"], datetime.datetime.now(ZoneInfo("Asia/Tokyo"))) else ["answer should give the current time in Tokyo"]

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

def case(name, request, must=(), must_not=(), checks=(), env=None, steer=None, home=None, radio=None, quotes_ok=False, tried=()):
    """request: one utterance or a list (a conversation; "/new" starts a fresh one, memories are kept).
    must / must_not / checks apply to the last turn; speech-suitability rules apply to every turn.
    must: tools the last turn must call; a change (CHANGES) must also have worked, since the case is about
    its effect. tried: tools it must call whatever came of the call, for routing and for the cases about a
    failure or a question first (a change that waits for the user's yes has not worked yet).
    home: a smart-home file to start from, saved for the stand-in test network (see EVAL_HOME).
    radio: a radio station file to start from (see EVAL_RADIO); without one there are no stations.
    quotes_ok: the answer may quote a confirmation without having acted ("what would you say if..."):
    the rule against claiming an action without a tool call is left out; the saved state is checked instead."""
    return {"name": name, "turns": [request] if isinstance(request, str) else list(request), "must": list(must),
            "must_not": list(must_not), "checks": list(checks), "env": env or {}, "steer": steer, "home": home,
            "radio": radio, "quotes_ok": quotes_ok, "tried": list(tried)}

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

def home_switched(name):
    def check(t):
        return [] if any(isinstance(r, dict) and r.get("target") == name and r.get("now") == "on" for r in results(t, "smart_home")) \
            else ["%s should have been switched on, got %s" % (name, results(t, "smart_home"))]
    return check

def home_not_switched(name):
    """The user declined: whatever the model tried, the thing was not switched on in this turn (as
    every_turn(...): in no turn of the conversation)."""
    def check(t):
        bad = [r for r in results(t, "smart_home") if isinstance(r, dict) and r.get("target") == name and r.get("now") == "on"]
        return ["%s was switched on although the user declined: %s" % (name, bad)] if bad else []
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

# The stand-in web (tests/mocks/fake_web.py): web search, forecasts and place names from canned
# fixtures, and search without a key, through the cli build's ZB_EXP_WEB. "@web" in a case's env is
# replaced by the stand-in's address when the case runs; one stand-in serves the whole run.
def web(fixture="default"):
    return {"ZB_EXP_WEB": "@web/" + fixture, "ZB_SEARCH_KEY": "stand-in"}

WEB = web()
_web = {}

def web_base():
    with _ports_lock:
        if "base" not in _web:
            p = subprocess.Popen([sys.executable, "-I", os.path.join(os.path.dirname(os.path.abspath(__file__)), "mocks", "fake_web.py")],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
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
    the home and a function giving the TV's log of commands ("METHOD path action"), which the runner
    stores in the last turn as "device_log"."""
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
    return home, lambda: ["%s %s %s" % (e["method"], e["path"], e["query"].get("action", "")) for e in tv.stats["requests"]]

def chk_device_log(sent, n):
    """A whole-conversation check: the device was sent exactly n requests of this kind ("POST /v1/FireTV
    volume_down"), counting the one it refused."""
    def check(turns):
        log = turns[-1].get("device_log")
        if log is None: raise Unscorable("the device's log of commands was not recorded")
        got = sum(1 for x in log if x == sent)
        return [] if got == n else ["the TV should get %r %d time%s in all, got %d: %s" % (sent, n, "" if n == 1 else "s", got, log)]
    check.whole = True
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

def chk_saved_when(n, ok=None, mention=None):
    """Exactly n reminders and calendar events are saved; with ok, each at one of those times, and with
    mention, each mentioning it."""
    def check(t):
        st = state(t)
        items = [_when(x) for x in saved_reminders(st) + saved_events(st)]
        bad = len(items) != n or any((ok and w not in ok()) or (mention and mention not in name.lower()) for w, name in items)
        return ["%d reminder%s should be saved%s%s, saved: %s" % (n, "" if n == 1 else "s", " for %s" % ok() if ok else "",
                                                                 " about %s" % mention if mention else "", items)] if bad else []
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

# "Nothing was set": the negation governs the setting ("I didn't set a reminder", "no reminder was saved",
# "nothing"), and no sentence states a reminder at 8 without one.
NEGATION = r"\b(didn'?t|did not|haven'?t|have not|hasn'?t|has not|not|no|nothing|won'?t|wasn'?t|isn'?t|never|none)\b"
NOTHING_SET = NEGATION + r"\W+(?:\w+\W+){0,3}?(set|remind\w*|saved?|scheduled|added)\b|\bset nothing\b|^\W*(nothing|none)\b"

def chk_said_nothing_set(t):
    a = t["answer"]
    problems = [] if re.search(NOTHING_SET, a, re.I) else ["the answer should say that nothing was set"]
    for sentence in re.split(r"(?<=[.!?])\s+", a):
        if re.search(r"remind", sentence, re.I) and re.search(r"\b(8|eight)\b", sentence, re.I) and not re.search(NEGATION, sentence, re.I):
            problems.append("the answer states a reminder at 8 as set: %r" % sentence.strip())
    return problems

# The time of an event has not been published: "not been announced", "no time yet", "isn't confirmed".
NOT_ANNOUNCED = r"(not|n't|no|yet to)\b.{0,30}\b(announc|publish|confirm|set|releas|reveal|disclos|known|given|decided|fixed|scheduled|available)|" \
                r"no (official |confirmed |exact |specific )?(time|date|details|word)|\b(tbc|tba|to be (confirmed|announced))\b|unknown"

def claims_match(failed_word, worked_word):
    """Each part of the answer says what came of the action it names: a clause about the `failed_word` thing
    (a regex) says it did not work, a clause about the `worked_word` one does not say it failed; and the
    answer has one of each. "The timer failed. The TV worked." fails it."""
    def check(t):
        clauses = [x for x in re.split(r"[.;:!?]|,|\b(?:but|and|while|whereas|though|although|however)\b", t["answer"], flags=re.I) if x and x.strip()]
        problems, said_failed, said_worked = [], False, False
        for x in clauses:
            bad, good = re.search(failed_word, x, re.I), re.search(worked_word, x, re.I)
            if bad and not good:
                if re.search(FAILED, x, re.I): said_failed = True
                else: problems.append("this says the %s worked: %r" % (failed_word, x.strip()))
            elif good and not bad:
                if re.search(FAILED, x, re.I): problems.append("this says the %s failed: %r" % (worked_word, x.strip()))
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
    case("search", "what time does the louvre open on sundays", must=["web_search"], checks=[answer(r"\b0?9(?![\d:.]\d)|\b0?9[:.]00|\bnine\b", "9 o'clock")]),
    case("search_news", "who won the mens singles at wimbledon this year", must=["web_search"]),
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
                 chk_saved_when(1, FRI_8PM, "dinner")]),
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
                                          home_asked_about("lamp")]),
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
    # nothing, and the last answer says the heater is off.
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
                 on_turn(0, NO_IR_OFFER), on_turn(1, home_step(-1)),
                 chk_device_log("POST /v1/FireTV volume_down", 2), chk_device_log("POST /v1/FireTV volume_up", 0),
                 chk_timers_named(pasta=600),
                 answer(r"(?<![\w-])(one|1|a single|just (the|one))\b(?! (moment|second|sec|minute|min|mo)\b)", "one timer"),
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
SCORER_VERSION = "2026-11-19.2"

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

RECORDS_VERSION = 1

def turns_from_records(text, c, process):
    """(turns, reasons): the turns of a case from the cli's records file (ZB_EVAL_RECORDS: one JSON object a
    line, written apart from the answer, so the answer cannot make one), and what makes the run unscorable.
    The records are checked as they are read: version, order, turn numbers, the request each turn answers
    (the case's own words, "/new" where the case has it), the fields every call and state record needs, a
    call record for every call in the turn's history, no more turns than the case has, and, for an app that
    exited normally, records to the end of the run. process: {"exit", "timeout", "seconds"}."""
    reasons, turns, cur, pos, ended = [], [], None, 0, False
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
        if kind == "start":
            if k != 1:
                bad("a second start")
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
            cur = _blank_turn(calls=[], began=r.get("mono"))   # seconds since the app started, as are "ended" and each call's "at"
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
                cur["calls"].append({"id": fields[0], "name": fields[1], "args": fields[2], "result": fields[3], "at": r.get("mono")})
            elif kind == "state":
                st = r.get("state")
                if "state" in cur or not isinstance(st, dict) or not isinstance(st.get("now"), int):
                    bad("a state record that is not one record with its time")
                    break
                cur["state"] = st
            elif kind == "steer":
                cur.setdefault("steered", []).append(str(r.get("text")))
            elif kind == "error":
                cur["error"] = str(r.get("message"))
            else:
                total, first, answer_, n = r.get("total"), r.get("first"), r.get("answer"), r.get("tool_calls")
                if not isinstance(total, (int, float)) or not isinstance(answer_, str) or not (first is None or isinstance(first, (int, float))) \
                   or not isinstance(n, int):
                    bad("a turn end without its time, answer and count of calls")
                    break
                if n != len(cur["calls"]):
                    bad("turn %d made %d tool calls but has %d call records" % (len(turns), n, len(cur["calls"])))
                    break
                cur.update(total=float(total), first=first, answer=plain(answer_), ended=r.get("mono"))
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

def run_case(binary, base_env, c):
    """Runs one case and returns what came of it: {"turns", "process": {"exit", "timeout", "seconds"},
    "evidence": "records" or "stdout", "unscorable": [why the evidence cannot be scored]}."""
    # A fresh state directory per case: no calendar entries, memories or time zone carried over.
    state = tempfile.mkdtemp(prefix="zb-eval-")
    device_log = None
    os.makedirs(state + "/config/zero-bot")
    settings = {"home_city": "London", "user_name": "Sam"}
    for key in ("llm_model", "stt_model", "tts_model", "music_dir"):
        if os.environ.get("ZB_EVAL_" + key.upper()): settings[key] = os.environ["ZB_EVAL_" + key.upper()]
    json.dump(settings, open(state + "/config/zero-bot/settings.json", "w"))
    env = dict(base_env, XDG_CONFIG_HOME=state + "/config", XDG_DATA_HOME=state + "/data", XDG_CACHE_HOME=state + "/cache")
    # No hardware beyond what the case simulates: camera and infrared refuse (experiment builds), and the
    # camera's photo is a fixture.
    env["ZB_EXP_NO_HARDWARE"] = "1"
    env.setdefault("ZB_FAKE_CAMERA", CAMERA_FIXTURE)
    records = state + "/records.jsonl"
    env["ZB_EVAL_RECORDS"] = records
    if c.get("radio"):
        json.dump(c["radio"], open(state + "/config/zero-bot/radio.json", "w"))
    if c.get("home"):
        # The made-up home lives on the stand-in network, where only 127.x addresses could ever be
        # contacted; with ZB_DRY_RUN none is.
        os.makedirs(state + "/data/zero-bot")
        home = c["home"]() if callable(c["home"]) else c["home"]   # a home with a port made when it runs
        if isinstance(home, tuple): home, device_log = home      # and a simulated device whose log is kept
        json.dump(home, open(state + "/data/zero-bot/smarthome.json", "w"))
        env["ZB_TEST_NETWORK"] = "1"
    for k, v in c["env"].items():
        if v is None: env.pop(k, None)
        elif isinstance(v, str) and v.startswith("@web"): env[k] = web_base() + v[4:]   # the stand-in web, started once
        else: env[k] = v
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
    # trace asks the service for token counts, a change to what is sent.
    trace = state + "/trace.jsonl" if base_env.get("ZB_EVAL_TRACE") else None
    if trace: env["ZB_EXP_TRACE"] = trace
    cmd = [binary, "--chat"] + (["--steer", c["steer"]] if c["steer"] else [])
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
    if device_log and turns: turns[-1]["device_log"] = device_log()
    for i, t in enumerate(turns):   # request n of the run is the n-th turn ("/new" starts none)
        mine = [e for e in events if e.get("req") == i + 1]
        req = next((e for e in mine if e.get("ev") == "request"), None)
        if req and req.get("first_sentence_ms", -1) >= 0: t["first_sentence"] = req["first_sentence_ms"] / 1000.0
        if trace: t["trace"] = mine
    return {"turns": turns, "process": process, "evidence": evidence, "unscorable": reasons}

UNSCORABLE = "unscorable: "
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
    if reasons: return "unscorable", [UNSCORABLE + r for r in reasons]
    problems = score(c, turns)
    p = run.get("process") or {}
    if p.get("timeout") and not any(t.get("timed_out") or t.get("unstarted") for t in turns):
        problems.append("the run timed out after its last answer: the app did not exit")
    elif p.get("exit") not in (0, None) and not any(t.get("unstarted") or t["total"] is None for t in turns):
        problems.append("the app ended with status %s after its last answer" % p["exit"])
    definite = [x for x in problems if not x.startswith(UNSCORABLE)]
    return ("fail" if definite else "unscorable" if problems else "pass"), problems

def run_from_saved(r):
    """A result saved by --json, as run_case returns one, for --rescore. Results saved before the process
    and evidence fields existed are scored as they are: what they lack makes their checks unscorable."""
    why = r.get("unscorable_why", r.get("unscorable") if isinstance(r.get("unscorable"), list) else [])
    return {"turns": r.get("turns"), "process": r.get("process"), "evidence": r.get("evidence", "stdout"), "unscorable": list(why or [])}

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
    problems = []
    # Turns whose answer may run long (analytical follow-ups; "long_ok" in the suite's cases).
    long_ok = {i % len(turns) for i in c.get("long_ok", ())} if turns else set()
    strict = any(k == "ZB_EXP_WEB" and v for k, v in c.get("env", {}).items())
    for t in turns:
        t["answer"] = plain(t.get("answer") or "")
        if strict: t["strict_reads"] = True
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
            changes = [r for n, r in made if n in CHANGES]
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
        elif not any(n in alts and counts(last, n, r) for n, r in made): problems.append("no call to %s worked: %s" % (want, tried(last, want)))
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
                    try: problems += ["turn %d: %s" % (i + 1, p) for p in check(t)]
                    except Unscorable as e: problems.append(UNSCORABLE + "turn %d: %s" % (i + 1, e))
            elif n is not None and not -len(turns) <= n < len(turns):
                problems.append("a check of turn %d, which did not happen" % (n + 1))
            else:
                t = last if n is None else turns[n]
                if t.get("unstarted"): continue   # reported above, once
                problems += [where + p for p in check(t)]
        except Unscorable as e: problems.append(UNSCORABLE + where + str(e))
        except Exception as e: problems.append(UNSCORABLE + where + "the check could not run: %r" % e)
    return problems

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

def case_hash(c):
    """sha1 of the case as it is scored: its turns, checks, env, home, radio stations, must and must_not
    (and steer, tried, long_ok and quotes_ok). Two results are comparable only when this and SCORER_VERSION
    agree. A suite case's checks are its JSON; a scored-set case's are fingerprints of the check functions."""
    import hashlib
    raw = c.get("source", c)
    checks = raw["checks"] if all(isinstance(x, dict) for x in raw["checks"]) else [_fingerprint(f) for f in raw["checks"]]
    home = raw.get("home")
    if isinstance(home, str): home = c.get("home")   # a suite case names its home; the home itself counts
    body = {"turns": raw["turns"], "checks": checks, "env": raw.get("env") or {}, "home": _fingerprint(home), "radio": raw.get("radio"),
            "must": raw.get("must", []), "must_not": raw.get("must_not", []), "steer": raw.get("steer"), "tried": raw.get("tried", []),
            "long_ok": raw.get("long_ok", []), "quotes_ok": raw.get("quotes_ok", False)}
    return hashlib.sha1(json.dumps(body, sort_keys=True, ensure_ascii=False, default=repr).encode()).hexdigest()

def result_record(c, run, outcome, problems, traced, which="scored", **extra):
    """What --json keeps of one run: the verdict ("outcome"; "unscorable" is true for an unscorable run, with
    the reasons in "problems"), the evidence it rests on ("unscorable_why": what made the records themselves
    unusable, kept for --rescore), and what makes it comparable: case_hash, scorer, trace, and the set it
    belongs to ("set": "scored" or "suite")."""
    return dict({"case": c["name"], "set": which, "case_hash": case_hash(c), "scorer": SCORER_VERSION, "trace": bool(traced),
                 "outcome": outcome, "unscorable": outcome == "unscorable", "requests": c["turns"], "problems": problems,
                 "turns": run["turns"], "process": run.get("process"), "evidence": run.get("evidence"),
                 "unscorable_why": run.get("unscorable") or []}, **extra)

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
        for k, v in (c.get("env") or {}).items():
            if not (v is None or isinstance(v, str)): err("env %s must be text or null" % k)
            if isinstance(v, str) and v.startswith("@web"):
                fx = v[5:] or "default"
                if not v.startswith("@web/") or fx not in fake_web.FIXTURES: err("env %s names a stand-in fixture that does not exist: %r" % (k, v))
        for i in c.get("long_ok", []):
            if not (isinstance(i, int) and -n <= i < n): err("long_ok names turn %r; the case has %d" % (i, n))
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
    home folder of the run's own (so nothing under the home of whoever runs it is read), and the stand-in
    music library unless ZB_EVAL_MUSIC_DIR names one."""
    if "ZB_EVAL_MUSIC_DIR" not in os.environ:
        os.environ["ZB_EVAL_MUSIC_DIR"] = make_music_library(os.path.join(home, "Music"))
    env = dict(os.environ, ZB_DRY_RUN="1", ZB_NO_AUDIO="1", HOME=home)
    if os.environ.get("ZB_EVAL_CAMERA"): env["ZB_FAKE_CAMERA"] = os.environ["ZB_EVAL_CAMERA"]
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
    ap.add_argument("--rescore", default="", help="score a results file written by --json again instead of running (same day; clock-time checks will fail)")
    ap.add_argument("--trace", action="store_true", help="record the experiments' trace (cli build, ZB_EXP_TRACE) into each turn of --json")
    ap.add_argument("--validate", action="store_true", help="check the cases as written and exit (non-zero on any problem)")
    args = ap.parse_args()
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

    home = tempfile.mkdtemp(prefix="zb-eval-home-")
    env = isolated_env(home)
    if args.fast is not None: env["ZB_FAST_MODE"] = args.fast
    if args.trace: env["ZB_EVAL_TRACE"] = "1"

    wanted = [w for w in args.only.split(",") if w]
    cases = [c for c in CASES if not wanted or any(w in c["name"] for w in wanted)]
    jobs = [c for c in cases for _ in range(len(saved.get(c["name"], [])) if args.rescore else args.repeat)]
    run = (lambda c: run_from_saved(saved[c["name"]].pop(0))) if args.rescore else (lambda c: run_case(args.bin, env, c))
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
            tr = traced if traced is not None else any("trace" in t for t in turns)
            results.append(result_record(c, got, outcome, problems, tr))
    shutil.rmtree(home, ignore_errors=True)
    print("\n%s | %s" % (outcome_line(results), timing_summary(results)))
    if args.trace: print(trace_summary(results))
    if failed_names:
        print("not passed: " + ", ".join("%s%s" % (k, " x%d" % v if args.repeat > 1 else "") for k, v in sorted(failed_names.items())))
    if args.json: json.dump(results, open(args.json, "w"), indent=1)
    sys.exit(1 if failed_names else 0)

if __name__ == "__main__":
    main()
