#!/usr/bin/env python3
"""Runs the generated request set (tests/suite/cases.json, written by tests/suite/generate.py)
through the command-line agent against the live service, with the scored set's machinery
(tests/agent_eval.py: the same runner, transcript parser, speech-suitability rules and checks).

Select by tag, name or a stratified sample, so a subset can be run:

    tests/agent_suite.py --list                          # what would run, and the tag counts
    tests/agent_suite.py --tags timer,reminder           # cases with any of these tags
    tests/agent_suite.py --tags asr+home                 # "+" means both
    tests/agent_suite.py --skip-tags search,camera
    tests/agent_suite.py --sample 60 --seed 1            # a few from every category
    tests/agent_suite.py --only timer-00 --repeat 5

Other options as in tests/agent_eval.py: --bin, --repeat, --jobs (keep 1 for timings), --fast 0|1,
--json OUT (full results), --rescore OUT (score saved results again; same day only), --trace (the
experiments' trace of each turn in --json: model calls, tokens, timings; tests/exp_report.py).
--validate compiles every case offline (kinds, nested ones too, what each needs, regular expressions,
the turns checks look at, homes, the stand-in web's fixtures) and exits non-zero on a problem. It needs
ZB_API_BASE and ZB_API_KEY (and model names in ZB_EVAL_LLM_MODEL etc.); cases tagged "search" are
left out without ZB_SEARCH_KEY. Every case starts from an empty state with ZB_DRY_RUN, as in the
scored set, and a small stand-in music library (MUSIC_LIBRARY: tags only, no sound) unless
ZB_EVAL_MUSIC_DIR names a folder; ZB_EVAL_MUSIC_DIR= (empty) gives an empty library, as runs before
2026-11-19 had. The summary gives the pass rate per tag and the slowest cases.

Checks are declarative in cases.json ({"kind": ...}); KINDS below compiles each one. Expected
dates, times, amounts and ages are worked out when the case runs. A check looks at the last turn,
or at the one its "turn" names (0 is the first, -1 the last; "/new" is not a turn); "long_ok" lists
the turns whose answer may be longer than the usual cap (analytical follow-ups). "must" tools have to
be called and, for a change, work; "tried" tools only have to be called (routing, failures, questions
first). Runs end "pass", "fail" or "unscorable", as in tests/agent_eval.py.
"""
import argparse
import concurrent.futures
import datetime
import json
import math
import os
import random
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(1, os.path.join(HERE, "mocks"))   # fake_web: the stand-in web's fixtures, for event_time
sys.dont_write_bytecode = True   # importing agent_eval leaves no __pycache__ in tests/
import agent_eval as ev   # noqa: E402

HOMES = {"eval": ev.EVAL_HOME, "tv": ev.TV_HOME, "sockets": ev.SOCKETS_HOME, "tv_silent": ev.TV_SILENT_HOME,
         "tv_closed": ev.TV_CLOSED_HOME}
MONTHS = ["january", "february", "march", "april", "may", "june", "july", "august", "september", "october", "november", "december"]
WEEKDAYS = ["monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"]
LANGUAGE = {"es": r"\b(el|la|los|las|de|que|en|hoy|grados|está|minutos|listo|temporizador|tiempo|hace)\b",
            "de": r"\b(der|die|das|ist|und|minuten|läuft|gestellt|grad|wetter|heute|morgen)\b",
            "fr": r"\b(le|la|les|est|il|fait|degrés|aujourd'hui|minuteur|demain)\b",
            "it": r"\b(il|la|di|che|è|oggi|gradi|minuti|domani)\b",
            "pt": r"\b(o|a|de|que|é|hoje|graus|minutos|está)\b",
            "nl": r"\b(de|het|een|is|graden|minuten|vandaag)\b"}


# ------------------------------------------------------------------ dates worked out when the case runs
def next_date(md, today=None):
    """The next month-day "MM-DD" on or after today."""
    d = today or ev.today().date()
    m, day = int(md[:2]), int(md[3:])
    target = datetime.date(d.year, m, day)
    return target if target >= d else datetime.date(d.year + 1, m, day)


def days_for(spec):
    today = ev.today().date()
    if spec == "today": return [today]
    if spec == "tomorrow": return [today + datetime.timedelta(days=1)]
    if spec.startswith("next:"): return ev.next_weekday(int(spec[5:]))
    if spec.startswith("next+:"):   # "next Monday": the coming one, or the one after it
        days = ev.next_weekday(int(spec[6:]))
        return days + [days[-1] + datetime.timedelta(days=7)]
    if spec.startswith("date:"): return [next_date(spec[5:])]
    raise ValueError("unknown day " + spec)


def stamps_for(day, times):
    if day == "next_clock":   # the next time the clock shows each of these
        now = ev.today()
        out = []
        for hhmm in times:
            t = now.replace(hour=int(hhmm[:2]), minute=int(hhmm[3:]), second=0, microsecond=0)
            out.append((t if t > now else t + datetime.timedelta(days=1)).strftime("%Y-%m-%d %H:%M"))
        return out
    return [s for hhmm in times for s in ev.stamps(days_for(day), hhmm)]


# The music library every case sees: tests/agent_eval.py's (MUSIC_LIBRARY there), shared by both sets.
MUSIC_LIBRARY = ev.MUSIC_LIBRARY


def says_date(text, d):
    t = text.lower()
    if d.isoformat() in t: return True
    month = MONTHS[d.month - 1]
    return bool(re.search(r"\b(%s|%s)\b" % (month, month[:3]), t) and re.search(r"\b%d(st|nd|rd|th)?\b" % d.day, t)) or \
        bool(re.search(r"\b(%s|%s)\b" % (month, month[:3]), t) and re.search(r"\b%s\b" % ev.words(d.day), t))


# "4 pounds 50 pence", "3 dollars and 25 cents": an amount in words is the same as 4.50 or 3.25.
MONEY_WORDS = re.compile(r"\b(\d+) (?:pounds?|dollars?|euros?)(?:,? and)? (\d{1,2}) ?(?:pence|p|cents?)\b", re.I)


def says_number(text, values, tol):
    text = MONEY_WORDS.sub(lambda m: "%s.%02d" % (m.group(1), int(m.group(2))), text)
    for v in values:
        if ev.says_about(text, v, tol): return True
        if float(v).is_integer() and ev.says_int(text, int(v)): return True
    return False


# ------------------------------------------------------------------ check kinds
def k_answer(s): return ev.answer(s["re"], s.get("what"))
def k_not_answer(s): return ev.not_answer(s["re"], s.get("what") or "match /%s/" % s["re"])


def k_arg(s):
    """An argument of a call that worked; with "made", of any call made, whatever came of it (for a request
    that cannot work in a fresh state, such as skipping a track when nothing plays)."""
    tool, key = s["tool"], s["key"]
    if s.get("made"):
        return lambda t: k_arg(dict(s, made=False))(dict(t, results=["made"] * len(t["tools"])))
    if "has" in s:
        subs = [x.lower() for x in s["has"]]
        return lambda t: ev.arg(t, tool, key, lambda v: any(x in str(v).lower() for x in subs), "about " + "/".join(subs))
    if "num" in s: return lambda t: ev.arg(t, tool, key, ev.num_is(s["num"]), str(s["num"]))
    if "is" in s: return lambda t: ev.arg(t, tool, key, lambda v: str(v).strip().lower() == s["is"].lower(), s["is"])
    if "any" in s: return lambda t: ev.arg(t, tool, key, lambda v: str(v).strip().lower() in s["any"], " or ".join(s["any"]))
    raise ValueError("arg check needs has, num, is or any")


def k_timer(s): return ev.chk_timer(s["seconds"], s.get("label"))
def k_timer_action(s): return ev.chk_timer_action(s["action"])
def k_time_left(s): return ev.chk_time_left
def k_cancelled(s): return ev.chk_cancelled(s["want"], s["keep"])


def k_countdown(s):
    """Counted down from now: a timer of that length, or a reminder that many minutes away (either is right)."""
    return ev.chk_countdown(s["seconds"])


def k_timer_becomes(s):
    """The running timer now has a new length: added to, or replaced by a new one of that length; with a
    recorded state, exactly one timer is saved, of the new length (a replacement that left the old one
    running is two timers)."""
    def check(t):
        if not (any(abs(ev.timer_seconds(c) - s["added"]) < 1 for c in ev.timer_calls(t, "add_time")) or
                any(abs(ev.timer_seconds(c) - s["seconds"]) < 1 for c in ev.timer_calls(t, "start"))):
            return ["the timer should gain %d seconds or be restarted for %d, got %s" % (s["added"], s["seconds"], ev.tried(t, "timer"))]
        return ev.chk_timers_saved(s["seconds"])(t)
    return check


def k_timers_saved(s): return ev.chk_timers_saved(*s["lengths"])


def k_moved(s):
    return ev.chk_moved_item(lambda: stamps_for(*s["first"]), lambda: stamps_for(*s["then"]), s["what"], s.get("title"))


def k_memory(s): return ev.chk_memory(s["has"], s.get("lacks", ()))
def k_memory_lacks(s): return ev.chk_memory_lacks(*s["re"])


def k_events_saved(s):
    """Exactly n calendar events are saved after the turn."""
    def check(t):
        st = ev.state(t)
        if len(ev.saved_events(st)) == s["n"]: return []
        return ["%d calendar events should be saved: %s" % (s["n"], ev.shown_state(st, "when"))]
    return check


def k_at(s):
    def check(t):
        ok = stamps_for(s["day"], s["times"])
        return ev.at_check(t, s["tools"], ok, " or ".join(ok) + (" repeating weekly" if s.get("weekly") else ""), s.get("weekly", False))
    return check


def k_yearly(s): return ev.chk_yearly(s["md"], s.get("year"))


def k_number(s):
    return lambda t: [] if says_number(t["answer"], s["values"], s["tol"]) else \
        ["answer should give %s" % " or ".join("%g" % v for v in s["values"])]


def k_days_until(s):
    def check(t):
        n = (next_date(s["md"]) - ev.today().date()).days
        ok = [n + k for k in range(-s.get("slack", 0), s.get("slack", 0) + 1)]
        return [] if any(ev.says_int(t["answer"], x) for x in ok) else ["answer should say %s days" % " or ".join(map(str, ok))]
    return check


def k_weeks_until(s):
    def check(t):
        n = (next_date(s["md"]) - ev.today().date()).days
        ok = sorted({n // 7, round(n / 7), math.ceil(n / 7)})
        return [] if any(ev.says_int(t["answer"], x) for x in ok) else ["answer should say about %s weeks" % "/".join(map(str, ok))]
    return check


def k_weekday_of(s):
    def check(t):
        if s.get("date"): d = datetime.date.fromisoformat(s["date"])
        else:
            year = ev.today().year + (1 if s.get("year") == "next" else 0)
            d = datetime.date(year, int(s["md"][:2]), int(s["md"][3:]))
        want = WEEKDAYS[d.weekday()]
        return [] if re.search(want, t["answer"], re.I) else ["answer should say %s (%s)" % (want, d)]
    return check


def k_date_in(s):
    def check(t):
        d = ev.today().date() + datetime.timedelta(days=s["days"])
        return [] if says_date(t["answer"], d) else ["answer should give the date %s" % d.strftime("%-d %B %Y")]
    return check


def k_today_date(s): return k_date_in({"days": 0})


def k_today_weekday(s):
    return lambda t: [] if re.search(WEEKDAYS[ev.today().weekday()], t["answer"], re.I) else \
        ["answer should say %s" % WEEKDAYS[ev.today().weekday()]]


def k_local_time(s): return ev.chk_local_time


def k_currency(s):
    def check(t):
        for c in ev.calls(t, "convert_currency"):
            if s.get("amount") is not None and not ev._same(c.get("amount"), s["amount"]): continue
            if s.get("from") and str(c.get("from", "")).upper() != s["from"]: continue
            if s.get("to") and str(c.get("to", "")).upper() != s["to"]: continue
            return []
        return ["convert_currency should convert %s %s to %s, got %s" % (s.get("amount"), s.get("from") or "?", s.get("to") or "?",
                                                                         ev.calls(t, "convert_currency"))]
    return check


def k_converted(s):
    def check(t):
        got = {str(r.get("to")): r for r in ev.results(t, "convert_currency") if isinstance(r, dict) and "rate" in r}
        problems = []
        for code in s["targets"]:
            if code not in got: problems.append("no convert_currency call to " + code)
            else:
                want = s["amount"] * got[code]["rate"]
                if not ev.says_about(t["answer"], want, max(0.06, want * 0.006)): problems.append("answer should give %.2f %s" % (want, code))
        return problems
    return check


def k_converted_share(s):
    def check(t):
        res = [r for r in ev.results(t, "convert_currency") if isinstance(r, dict) and "rate" in r]
        if not res: return ["no usable convert_currency result"]
        want = s["amount"] * s["factor"] * res[0]["rate"]
        return [] if ev.says_about(t["answer"], want, 0.06) else ["answer should give %.2f" % want]
    return check


def k_stock_answer(s): return ev.chk_stock


def k_time_in(s):
    from zoneinfo import ZoneInfo
    return lambda t: [] if ev.says_time(t["answer"], datetime.datetime.now(ZoneInfo(s["zone"]))) else \
        ["answer should give the current time in " + s["zone"]]


def k_time_conv(s):
    def check(t):
        from zoneinfo import ZoneInfo
        local = ev.today().replace(hour=int(s["local"][:2]), minute=int(s["local"][3:]), second=0, microsecond=0).astimezone()
        there = local.astimezone(ZoneInfo(s["zone"]))
        return [] if ev.says_time(t["answer"], there, 0) else ["answer should give %s in %s" % (there.strftime("%H:%M"), s["zone"])]
    return check


def k_home_weather(s):
    return lambda t: [] if ev.calls(t, "weather") and all(str(c.get("location", "london")).lower().startswith("london")
                                                          for c in ev.calls(t, "weather")) else ["weather should be for the home city (London)"]


def k_music_filter(s):
    subs = [x.lower() for x in s["has"]]
    def check(t):
        for c in ev.calls(t, "music"):
            if any(x in (str(c.get(s["key"], "")) + " " + str(c.get("query", ""))).lower() for x in subs): return []
        return ["music should filter by %s about %s, got %s" % (s["key"], "/".join(subs), ev.calls(t, "music"))]
    return check


def k_language(s):
    return lambda t: [] if re.search(LANGUAGE[s["lang"]], t["answer"], re.I) else ["answer should be in language " + s["lang"]]


def k_age_on(s):
    def check(t):
        age = next_date(s["md"]).year - s["born"]
        return [] if ev.says_int(t["answer"], age) else ["answer should give the age %d" % age]
    return check


def k_no_call(s):
    return lambda t: ["should not call %s" % s["tool"]] if ev.attempts(t, s["tool"]) else []


def k_max_calls(s):
    """At most n calls to the tool (or to any of a|b) in the turn: a budget, as for searches in a row."""
    names = s["tool"].split("|")
    def check(t):
        got = sum(1 for n, _ in t["tools"] if n in names)
        return [] if got <= s["n"] else ["at most %d call%s to %s, got %d" % (s["n"], "" if s["n"] == 1 else "s", s["tool"], got)]
    return check


def k_max_words(s):
    """An answer of at most n words: for simple factual turns only (never analytical ones)."""
    def check(t):
        got = len(re.findall(r"[^\W_]+(?:['’.,:-][^\W_]+)*", t["answer"]))
        return [] if got <= s["n"] else ["answer should be at most %d words, got %d" % (s["n"], got)]
    return check


def k_all_day_on(s):
    """An all-day calendar_add on that day."""
    def check(t):
        days = [d.isoformat() for d in days_for(s["day"])]
        if any(c.get("all_day") is True and str(c.get("start"))[:10] in days for c in ev.calls(t, "calendar_add")): return []
        return ["calendar_add should be all_day on %s, got %s" % (" or ".join(days), ev.calls(t, "calendar_add"))]
    return check


def k_either(s):
    """Passes when every check of any one group passes: for requests with more than one right answer."""
    groups = [[KINDS[x["kind"]](x) for x in group] for group in s["of"]]
    def check(t):
        found = [sum((c(t) for c in group), []) for group in groups]
        return [] if any(not f for f in found) else ["none of the right answers: " + " / ".join("; ".join(f) for f in found)]
    return check


def k_asks(s): return ev.asks_about(s["name"])
def k_forecast_seen(s): return ev.chk_forecast_seen(s["days"], s["rain"])
def k_nothing_set(s): return ev.chk_said_nothing_set


def k_event_time(s):
    """The answer gives the stand-in web's published event time (tests/mocks/fake_web.py, EVENTS), converted
    to the user's time zone: exactly, with its am or pm right."""
    import fake_web
    from zoneinfo import ZoneInfo
    def check(t):
        when = fake_web.event_time(s["event"]).astimezone(ZoneInfo(s.get("zone", "Europe/London")))
        return [] if ev.says_time(t["answer"], when, 0) else \
            ["answer should give the event's time in %s, %s" % (s.get("zone", "Europe/London"), when.strftime("%H:%M (%-I:%M %p) on %-d %B"))]
    return check


def k_unannounced_time(s):
    """For an event whose time is not published: the answer says so, and gives no time of day for it except
    as what usually happens ("usually 9:30"): any other time is a guess."""
    def check(t):
        a = t["answer"]
        problems = [] if re.search(ev.NOT_ANNOUNCED, a, re.I) else ["the answer should say the time has not been announced"]
        for sentence in re.split(r"(?<=[.!?])\s+", a):
            if re.search(TIME_OF_DAY, sentence, re.I) and not re.search(USUALLY, sentence, re.I):
                problems.append("the answer gives a time the search did not: %r" % sentence.strip())
        return problems
    return check


TIME_OF_DAY = r"\b\d{1,2}(:\d\d)?\s*(am|pm|a\.m\.|p\.m\.)|\b\d{1,2}:\d\d\b|o'?clock|\b(noon|midday|midnight)\b"
# What usually happens, or would if it went ahead ("if it lists in New York, trading would open at 9:30"),
# rather than a time given for this event.
USUALLY = r"\b(usually|typically|normally|generally|traditionally|tends? to|historically|in the past|previous|regular|standard|" \
          r"open(s|ing)? (at|from)|opening (time|bell|hours?))\b|\bif\b[^.!?]*\bwould\b"


def k_home_call(s): return ev.home_call(s["action"], s.get("targets"), **s.get("values", {}))
def k_home_never_on(s): return ev.home_never_on
def k_home_asked(s): return ev.home_asked_about(s.get("name"))
def k_home_switched(s): return ev.home_switched(s["name"])
def k_home_not_switched(s): return ev.home_not_switched(s["name"])
def k_home_all_off(s): return ev.home_all_off
def k_home_step(s): return ev.home_step(s["sign"])
def k_home_no_action(s): return ev.home_no_action(*s["actions"])


KINDS = {name[2:]: f for name, f in globals().items() if name.startswith("k_") and callable(f)}


def compile_check(s):
    """One check; with "turn", it carries that turn's number (or "all": every turn) for tests/agent_eval.py's
    score(). A whole-conversation check ("moved") looks at every turn itself."""
    f = KINDS[s["kind"]](s)
    if "turn" not in s or getattr(f, "whole", False): return f
    def on_turn(t): return f(t)
    on_turn.turn = s["turn"]
    return on_turn


def compile_case(c):
    """A case from cases.json in the form tests/agent_eval.py runs and scores ("source" keeps the JSON, for
    the case's hash)."""
    out = dict(c, checks=[compile_check(s) for s in c["checks"]], home=HOMES[c["home"]] if c.get("home") else None, source=c)
    return out


# What each kind needs ("re" is a regular expression); the others are optional.
NEEDS = {"answer": ["re"], "not_answer": ["re"], "arg": ["tool", "key"], "timer": ["seconds"], "timer_action": ["action"],
         "cancelled": ["want", "keep"], "countdown": ["seconds"], "timer_becomes": ["added", "seconds"], "timers_saved": ["lengths"],
         "moved": ["first", "then", "what"], "memory": ["has"], "memory_lacks": ["re"], "events_saved": ["n"],
         "at": ["day", "times", "tools"], "yearly": ["md"], "number": ["values", "tol"], "days_until": ["md"], "weeks_until": ["md"],
         "date_in": ["days"], "currency": [], "converted": ["targets", "amount"], "converted_share": ["amount", "factor"],
         "time_in": ["zone"], "time_conv": ["local", "zone"], "music_filter": ["key", "has"], "language": ["lang"], "age_on": ["md", "born"],
         "no_call": ["tool"], "max_calls": ["tool", "n"], "max_words": ["n"], "all_day_on": ["day"], "either": ["of"],
         "home_call": ["action"], "home_asked": [], "home_switched": ["name"], "home_not_switched": ["name"], "home_step": ["sign"],
         "home_no_action": ["actions"], "asks": ["name"], "forecast_seen": ["days", "rain"], "event_time": ["event"]}
FIELDS = ("name", "tags", "turns", "must", "must_not", "checks", "env", "home", "steer")


def validate(cases):
    """Every case compiled offline, as a live run would: the fields every case has, each check's kind (inside
    "either" too) and what it needs, its regular expressions, the turn it looks at, the home and the stand-in
    web's fixture it names; then the scored set's checks (tests/agent_eval.py validate). Returns the problems."""
    import fake_web
    errors = []
    for c in cases:
        name = c.get("name", "?")
        def err(why): errors.append("%s: %s" % (name, why))
        missing = [f for f in FIELDS if f not in c]
        if missing:
            err("no %s" % ", ".join(missing))
            continue
        if not isinstance(c["tags"], list) or not c["tags"] or not all(isinstance(t, str) for t in c["tags"]): err("tags must name a category first")
        if c["home"] is not None and c["home"] not in HOMES: err("no home called %r" % c["home"])
        n = sum(1 for t in c["turns"] if t != "/new") if isinstance(c["turns"], list) else 0

        def one(s, where):
            if not isinstance(s, dict) or s.get("kind") not in KINDS:
                err("%s: no check kind %r" % (where, s.get("kind") if isinstance(s, dict) else s))
                return
            lacking = [k for k in NEEDS.get(s["kind"], []) if k not in s]
            if lacking: err("%s: %s needs %s" % (where, s["kind"], ", ".join(lacking)))
            for key in ("re", "has"):
                pats = s.get(key) if s["kind"] in ("answer", "not_answer", "memory", "memory_lacks") else None
                for pat in ([pats] if isinstance(pats, str) else pats or []):
                    try: re.compile(pat)
                    except (re.error, TypeError) as e: err("%s: %s is not a regular expression: %s" % (where, key, e))
            if "turn" in s and not (s["turn"] == "all" or (isinstance(s["turn"], int) and -n <= s["turn"] < n)):
                err("%s: looks at turn %r; the case has %d" % (where, s["turn"], n))
            if s["kind"] == "either":
                for g, group in enumerate(s.get("of", [])):
                    for k, x in enumerate(group): one(x, "%s, alternative %d.%d" % (where, g + 1, k + 1))
            if s["kind"] == "event_time" and s.get("event") not in fake_web.EVENTS: err("%s: no stand-in event %r" % (where, s.get("event")))
            if s["kind"] == "unannounced_time" and "ZB_EXP_WEB" not in c["env"]: err("%s: unannounced_time needs the stand-in web" % where)
        for i, s in enumerate(c["checks"] if isinstance(c["checks"], list) else []): one(s, "check %d" % (i + 1))
        if any(isinstance(s, dict) and s.get("kind") == "event_time" for s in c["checks"]) and "ZB_EXP_WEB" not in c["env"]:
            err("event_time needs the stand-in web (env ZB_EXP_WEB)")
        try: compiled = compile_case(c)
        except Exception as e:   # noqa: BLE001
            err("does not compile: %r" % e)
            continue
        errors += ev.validate([compiled]) if not any(e.startswith(name + ":") for e in errors) else []
    names = [c.get("name") for c in cases]
    errors += ["%s: the name is used twice" % x for x in sorted({x for x in names if names.count(x) > 1})]
    return errors


# ------------------------------------------------------------------ selection
def selected(cases, args):
    def has_tags(c, expr):
        return any(all(p in c["tags"] for p in alt.split("+")) for alt in expr.split(",") if alt)
    out = [c for c in cases if (not args.tags or has_tags(c, args.tags)) and not (args.skip_tags and has_tags(c, args.skip_tags))
           and (not args.only or any(w in c["name"] for w in args.only.split(",") if w))]
    if args.sample:
        # A few from every category, in turn, chosen by the seed.
        rng = random.Random(args.seed)
        groups = {}
        for c in out: groups.setdefault(c["tags"][0], []).append(c)
        for g in groups.values(): rng.shuffle(g)
        picked, order = [], sorted(groups)
        while len(picked) < args.sample and any(groups.values()):
            for cat in order:
                if groups[cat] and len(picked) < args.sample: picked.append(groups[cat].pop())
        names = {c["name"] for c in picked}
        out = [c for c in out if c["name"] in names]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--cases", default=os.path.join(HERE, "suite", "cases.json"))
    ap.add_argument("--bin", default="build/cli/zero_bot")
    ap.add_argument("--tags", default="", help="cases with any of these tags (comma-separated; a+b means both)")
    ap.add_argument("--skip-tags", default="", help="leave out cases with any of these tags")
    ap.add_argument("--only", default="", help="cases whose name contains this (comma-separated alternatives)")
    ap.add_argument("--sample", type=int, default=0, help="a stratified sample of this many cases (by category)")
    ap.add_argument("--seed", type=int, default=1, help="seed for --sample")
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=1, help="cases at once (timings are only comparable at 1)")
    ap.add_argument("--fast", default=None, help="force fast mode on (1) or off (0)")
    ap.add_argument("--json", default="", help="write full results to this file")
    ap.add_argument("--rescore", default="", help="score a results file written by --json again (same day only)")
    ap.add_argument("--list", action="store_true", help="list the selected cases and tag counts, run nothing")
    ap.add_argument("--trace", action="store_true", help="record the experiments' trace (cli build, ZB_EXP_TRACE) into each turn of --json")
    ap.add_argument("--slowest", type=int, default=10, help="how many of the slowest cases to list")
    ap.add_argument("--validate", action="store_true", help="compile and check every case offline, run nothing (non-zero on any problem)")
    args = ap.parse_args()

    cases = json.load(open(args.cases, encoding="utf-8"))
    if args.validate:
        errors = validate(cases)
        for e in errors: print("INVALID " + e)
        print("%d cases, %d problems" % (len(cases), len(errors)))
        sys.exit(1 if errors else 0)
    chosen = selected(cases, args)
    if not args.rescore and not args.list and not os.environ.get("ZB_SEARCH_KEY"):
        # Cases whose search comes from the stand-in web (env ZB_EXP_WEB) need no key.
        dropped = [c for c in chosen if "search" in c["tags"] and not c.get("env", {}).get("ZB_SEARCH_KEY", "x") is None
                   and "ZB_EXP_WEB" not in c.get("env", {})]
        if dropped:
            print("no ZB_SEARCH_KEY: leaving out %d cases tagged search" % len(dropped))
            chosen = [c for c in chosen if c not in dropped]
    if args.list:
        tags = {}
        for c in chosen:
            print("%-16s %-34s %s" % (c["name"], ",".join(c["tags"][1:]), " | ".join(c["turns"])))
            for t in c["tags"]: tags[t] = tags.get(t, 0) + 1
        print("\n%d cases; tags: %s" % (len(chosen), ", ".join("%s %d" % kv for kv in sorted(tags.items(), key=lambda kv: (-kv[1], kv[0])))))
        return
    saved = {}
    if args.rescore:
        for r in json.load(open(args.rescore)): saved.setdefault(r["case"], []).append(r)
        chosen = [c for c in chosen if c["name"] in saved]
    elif not os.environ.get("ZB_API_BASE") or not os.environ.get("ZB_API_KEY"):
        sys.exit("ZB_API_BASE and ZB_API_KEY must be set")

    # An empty home folder of the run's own, and the stand-in music library (tests/agent_eval.py isolated_env).
    home = tempfile.mkdtemp(prefix="zb-suite-home-")
    env = ev.isolated_env(home)
    if args.fast is not None: env["ZB_FAST_MODE"] = args.fast
    if args.trace: env["ZB_EVAL_TRACE"] = "1"
    compiled = {c["name"]: compile_case(c) for c in chosen}
    jobs = [c for c in chosen for _ in range(len(saved.get(c["name"], [])) if args.rescore else args.repeat)]
    run = (lambda c: ev.run_from_saved(saved[c["name"]].pop(0))) if args.rescore else (lambda c: ev.run_case(args.bin, env, compiled[c["name"]]))
    traced = args.trace if not args.rescore else None

    results, failed_names, per_tag = [], {}, {}
    with concurrent.futures.ThreadPoolExecutor(max(1, args.jobs)) as pool:
        for c, got in zip(jobs, pool.map(run, jobs)):
            outcome, problems = ev.verdict(compiled[c["name"]], got)
            if outcome != "pass": failed_names[c["name"]] = failed_names.get(c["name"], 0) + 1
            for t in c["tags"]:
                ok, n, u = per_tag.get(t, (0, 0, 0))
                per_tag[t] = (ok + (outcome == "pass"), n + 1, u + (outcome == "unscorable"))
            turns = got["turns"] or []
            for i, t in enumerate(turns):
                shown = ", ".join("%s%s" % (n, a if len(a) < 70 else a[:67] + "...") for n, a in t.get("tools", [])) or "-"
                first = "%.1f" % t["first"] if t.get("first") is not None else " - "
                tag = {"pass": "PASS", "fail": "FAIL", "unscorable": "????"}[outcome] if i == len(turns) - 1 else "    "
                print("%-4s %-16s first %4ss total %5.1fs  [%s]" % (tag, c["name"], first, t.get("total") or 0, shown))
                print("       \"%s\"" % str(t.get("answer", ""))[:220].replace("\n", " / "))
            for p in problems: print("       !! " + p)
            sys.stdout.flush()
            tr = traced if traced is not None else any("trace" in t for t in turns)
            results.append(ev.result_record(compiled[c["name"]], got, outcome, problems, tr, "suite", tags=c["tags"],
                                            seconds=sum(t.get("total") or 0 for t in turns)))
    n = len(results)
    shutil.rmtree(home, ignore_errors=True)
    if not n:
        print("no cases selected")
        return
    print("\n%s | %s" % (ev.outcome_line(results), ev.timing_summary(results)))
    print("by tag: " + ", ".join("%s %d/%d%s" % (t, ok, k, " (%d unscorable)" % u if u else "")
                                 for t, (ok, k, u) in sorted(per_tag.items(), key=lambda kv: (kv[1][0] / kv[1][1], kv[0]))))
    slow = sorted(results, key=lambda r: -r["seconds"])[:args.slowest]
    print("slowest: " + ", ".join("%s %.1fs" % (r["case"], r["seconds"]) for r in slow))
    if args.trace: print(ev.trace_summary(results))
    if failed_names:
        print("not passed: " + ", ".join("%s%s" % (k, " x%d" % v if args.repeat > 1 else "") for k, v in sorted(failed_names.items())))
    if args.json: json.dump(results, open(args.json, "w"), indent=1)
    sys.exit(1 if failed_names else 0)


if __name__ == "__main__":
    main()
