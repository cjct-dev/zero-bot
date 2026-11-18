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
                        [--fast 0|1] [--jobs N] [--json out.json] [--rescore out.json]

Side effects are disabled (ZB_DRY_RUN): no music plays, no infrared is sent and no smart-home
device is switched. Every case starts from an empty calendar, memory and settings; the smart-home
cases start from a made-up home on the stand-in test network (ZB_TEST_NETWORK).
"""
import argparse, concurrent.futures, datetime, json, os, re, shutil, subprocess, sys, tempfile, time

# ---------------------------------------------------------------- helpers for checks
ONES = "zero one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen " \
       "seventeen eighteen nineteen".split()
TENS = "_ _ twenty thirty forty fifty sixty seventy eighty ninety".split()

def words(n):
    """English words for 0..999, the way a speech-friendly answer might spell a number."""
    if n < 20: return ONES[n]
    if n < 100: return TENS[n // 10] + ("" if n % 10 == 0 else "[- ]" + ONES[n % 10])
    return ONES[n // 100] + " hundred" + ("" if n % 100 == 0 else " (and )?" + words(n % 100))

def says_int(text, n):
    """True when the answer states the whole number n, as digits or as words."""
    return bool(re.search(r"(?<![\d.,])%d(?![\d]|[.,]\d)" % n, text) or
                (0 <= n < 1000 and re.search(r"\b" + words(n) + r"\b", text, re.I)))

def numbers(text):
    return [float(x.replace(",", "")) for x in re.findall(r"\d[\d,]*\.?\d*", text) if x.replace(",", "").strip(".")]

def says_about(text, value, tol):
    """True when some number in the answer is within tol of value."""
    return any(abs(n - value) <= tol for n in numbers(text))

def says_time(text, when, slack_min=2):
    """True when the answer states the clock time `when` (24-hour or 12-hour), give or take slack."""
    for d in range(-slack_min, slack_min + 1):
        t = when + datetime.timedelta(minutes=d)
        h12 = t.hour % 12 or 12
        for pat in (r"\b0?%d[:.]%02d\b" % (t.hour, t.minute), r"\b%d[:.]%02d\b" % (h12, t.minute)):
            if re.search(pat, text): return True
        if t.hour % 12 == 0 and t.minute < 10 and re.search(r"midnight" if t.hour == 0 else r"noon|midday", text, re.I): return True
        if t.minute == 0 and re.search(r"\b(%d|%s)\s*(o'?clock|a\.?m|p\.?m)" % (h12, ONES[h12]), text, re.I): return True
    return False

def today(): return datetime.datetime.now()

def next_weekday(wd):
    """Dates 'friday' can reasonably mean today: the next one, or a week on if today is that day."""
    d = today().date()
    ahead = (wd - d.weekday()) % 7
    return [d + datetime.timedelta(days=ahead)] + ([d + datetime.timedelta(days=7)] if ahead == 0 else [])

def stamps(days, hhmm): return ["%s %s" % (d.isoformat(), hhmm) for d in days]

def tomorrow(): return today().date() + datetime.timedelta(days=1)

def calls(turn, name):
    """Parsed arguments of every call to `name` (or any of a|b) in this turn."""
    out = []
    for n, a in turn["tools"]:
        if n in name.split("|"):
            try: out.append(json.loads(a))
            except ValueError: out.append({})
    return out

def results(turn, name):
    """Tool results (parsed as JSON where possible) for calls to `name` in this turn."""
    out = []
    for (n, _), r in zip(turn["tools"], turn["results"]):
        if n in name.split("|"):
            try: out.append(json.loads(r))
            except ValueError: out.append(r)
    return out

def arg(turn, name, key, ok, what):
    """Problem text unless some call to `name` has argument `key` accepted by ok()."""
    got = [c.get(key) for c in calls(turn, name)]
    if any(v is not None and ok(v) for v in got): return []
    return ["%s.%s should be %s, got %s" % (name, key, what, got)]

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
    return [] if says_about(t["answer"], res[0]["price"], 1.0) else ["answer should give the price %.2f" % res[0]["price"]]

def chk_tokyo(t):
    from zoneinfo import ZoneInfo
    return [] if says_time(t["answer"], datetime.datetime.now(ZoneInfo("Asia/Tokyo"))) else ["answer should give the current time in Tokyo"]

def chk_local_time(t):
    return [] if says_time(t["answer"], today()) else ["answer should give the current local time"]

def chk_flight(t):
    d = today().date()
    year = d.year if d <= datetime.date(d.year, 12, 3) else d.year + 1
    return arg(t, "calendar_add", "start", one_of(["%d-12-03 06:40" % year]), "3 December 06:40")

def chk_moved(t):
    ok = stamps(next_weekday(4), "20:30")
    if any(str(c.get("start"))[:16] in ok for c in calls(t, "calendar_update|calendar_add")): return []
    return ["the event should be moved to Friday 20:30 with calendar_update"]

def chk_alarm(t):
    ok = stamps([tomorrow()], "08:00")
    if any(str(c.get("at", c.get("start")))[:16] in ok for c in calls(t, "reminder_set|calendar_add")): return []
    return ["the alarm should be for tomorrow 08:00"]

def timer_seconds(c):
    """The length a timer call asks for, in seconds (minutes and seconds may both be given)."""
    try: return float(c.get("hours") or 0) * 3600 + float(c.get("minutes") or 0) * 60 + float(c.get("seconds") or 0)
    except (TypeError, ValueError): return -1

def timer_calls(t, action):
    return [c for c in calls(t, "timer") if str(c.get("action", "")).lower() == action]

def chk_timer(seconds, label=None):
    """A timer started for this long, with a label mentioning `label` when one is given."""
    def check(t):
        starts = timer_calls(t, "start")
        if any(abs(timer_seconds(c) - seconds) < 1 and (label is None or label in str(c.get("label", "")).lower())
               for c in starts): return []
        return ["timer should start for %d seconds%s, got %s" % (seconds, " with a label about " + label if label else "", starts)]
    return check

def chk_timer_action(action):
    return lambda t: [] if timer_calls(t, action) else ["timer should be called with action %s, got %s" % (action, calls(t, "timer"))]

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
        return []
    return check

def chk_ninety(t):
    # "In an hour and a half" counts down from now, so a timer is as right as a reminder.
    if any(abs(timer_seconds(c) - 5400) < 1 for c in timer_calls(t, "start")): return []
    for c in calls(t, "reminder_set"):
        if "in_minutes" in c and abs(float(c["in_minutes"]) - 90) < 0.01: return []
        if c.get("at"):
            try: due = datetime.datetime.strptime(str(c["at"])[:16], "%Y-%m-%d %H:%M")
            except ValueError: continue
            if abs((due - today()).total_seconds() - 5400) < 180: return []
    return ["the reminder should fire 90 minutes from now"]

def chk_umbrella(t):
    ok = stamps(next_weekday(4), "18:00")
    if any(str(c.get("at", c.get("start")))[:16] in ok for c in calls(t, "reminder_set|calendar_add")): return []
    return ["the reminder should be for Friday 18:00"]

def chk_spanish(t):
    return [] if re.search(r"\b(grados|hoy|está|hace|lluvia|nublado|despejado|cielo|temperatura|máxima)\b", t["answer"], re.I) \
        else ["answer should be in Spanish"]

# ---------------------------------------------------------------- the cases
def chk_numeric_date(month, day):
    def check(t):
        d = today().date()
        year = d.year if d <= datetime.date(d.year, month, day) else d.year + 1
        return arg(t, "calendar_add", "start", one_of(["%d-%02d-%02d 10:00" % (year, month, day)]),
                   "%02d-%02d 10:00 (month-day)" % (month, day))
    return check

def chk_yearly(month_day, year=None):
    """A calendar_add for an all-day yearly event on month_day ("03-14"), in `year` if one was given."""
    def check(t):
        got = calls(t, "calendar_add")
        for c in got:
            start = str(c.get("start"))[:10]
            if c.get("repeat") == "yearly" and c.get("all_day") is True and start[5:] == month_day and \
               (year is None or start[:4] == str(year) or c.get("since_year") == year):
                return []
        return ["calendar_add should be all_day, repeat yearly, on %s%s; got %s" % (month_day, " %d" % year if year else "", got)]
    return check

def chk_weekly_tuesday(t):
    ok = stamps(next_weekday(1), "07:00")
    for c in calls(t, "calendar_add"):
        if c.get("repeat") == "weekly" and str(c.get("start"))[:16] in ok: return []
    return ["calendar_add should repeat weekly from next Tuesday 07:00, got %s" % calls(t, "calendar_add")]

def chk_dad_age(t):
    d = today().date()
    year = d.year if d <= datetime.date(d.year, 6, 2) else d.year + 1
    return [] if says_int(t["answer"], year - 1955) else ["answer should give the age %d" % (year - 1955)]

def case(name, request, must=(), must_not=(), checks=(), env=None, steer=None, home=None, radio=None):
    """request: one utterance or a list (a conversation; "/new" starts a fresh one, memories are kept).
    must / must_not / checks apply to the last turn; speech-suitability rules apply to every turn.
    home: a smart-home file to start from, saved for the stand-in test network (see EVAL_HOME).
    radio: a radio station file to start from (see EVAL_RADIO); without one there are no stations."""
    return {"name": name, "turns": [request] if isinstance(request, str) else list(request), "must": list(must),
            "must_not": list(must_not), "checks": list(checks), "env": env or {}, "steer": steer, "home": home,
            "radio": radio}

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
                                                                            " and %s" % values if values else "", calls(t, "smart_home"))]
    return check

def home_never_on(t):
    """Nothing switched on: "everything" and unknown things must never be guessed at."""
    bad = [c for c in calls(t, "smart_home") if str(c.get("action", "")).lower() in ("on", "set")]
    return ["switched something on without a clear target: %s" % bad] if bad else []

def home_asked(t):
    """The change waited for a yes: the tool answered CONFIRM, nothing was switched, and the user was asked."""
    got = results(t, "smart_home")
    problems = [] if any(isinstance(r, str) and r.startswith("CONFIRM") for r in got) else ["smart_home never answered CONFIRM"]
    if any(isinstance(r, dict) and r.get("now") == "on" for r in got): problems.append("something was switched on without a yes")
    return problems + ([] if "?" in t["answer"] else ["the answer should ask the user"])

def home_switched(name):
    def check(t):
        return [] if any(isinstance(r, dict) and r.get("target") == name and r.get("now") == "on" for r in results(t, "smart_home")) \
            else ["%s should have been switched on, got %s" % (name, results(t, "smart_home"))]
    return check

def home_not_switched(name):
    """The user declined: whatever the model tried, the thing was not switched on in this turn."""
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
        return [] if ok else ["smart_home should set volume_step %s 0, got %s" % (">" if sign > 0 else "<", calls(t, "smart_home"))]
    return check

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
        return ["radio should play %s, got %s" % (station, calls(t, "radio"))]
    return check

def radio_never_plays(t):
    played = [c for c in calls(t, "radio") if str(c.get("action", "play")).lower() == "play"]
    return ["radio should not play anything, got %s" % played] if played else []

def stops_player(t):
    if any(str(c.get("action", "")).lower() == "stop" for c in calls(t, "radio|music")): return []
    return ["radio or music should stop, got %s" % calls(t, "radio|music")]

# Saying that radio is not available: "no radio stations are set up", "I can't play radio stations".
RADIO_NOT_SET_UP = (r"not set up|no (radio )?stations|(can'?t|cannot|unable to|not able to|don'?t|do not|no way to)\b.{0,40}"
                    r"\b(radio|stations?|stream|live)|radio (isn'?t|is not|aren'?t|has not|hasn'?t)|without (any )?(radio )?stations")
UNSURE = r"search|can'?t (check|look|confirm)|cannot|not set up|unable|not sure|may have changed|might have changed|" \
         r"check (the|their)|double.check|verify|usually|typically|normally|generally|i believe|from memory|as far as i know"

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
    case("calc", "whats 17 and a half percent of 2340", checks=[answer(r"409\.5|409 and a half|nine and a half|409 point 5", "409.5")]),
    case("units", "whats 72 fahrenheit in celsius", checks=[answer(r"22\.2|22 point 2|\b22\b|twenty[- ]two", "22.2")]),
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
         checks=[chk_timer_action("list"), chk_time_left]),
    case("timer_cancel_named", ["i want to study for 45 minutes set a timer", "and set a 10 minute timer for the pasta",
                                "cancel the study timer"], must=["timer"], checks=[chk_cancelled("stud", "pasta")]),
    case("timer_no_length", "set a timer", must_not=["timer", "reminder_set", "calendar_add"],
         checks=[answer(r"\?", "a question"), answer(r"how long|minutes|length", "a question about the length")]),
    case("reminder_clock", "remind me at 7 tomorrow morning to call the hotel", must=["reminder_set|calendar_add"],
         must_not=["timer"],
         checks=[answer(r"7|seven"), lambda t: [] if any(str(c.get("at", c.get("start")))[:16] in stamps([tomorrow()], "07:00")
                                                         for c in calls(t, "reminder_set|calendar_add")) else ["should be set for tomorrow 07:00"]]),
    case("reminder_relative", "remind me in an hour and a half to take my tablets", must=["reminder_set|timer"], checks=[chk_ninety]),
    case("calendar_add", "put dinner with marta in my calendar for friday at 8pm", must=["calendar_add"],
         checks=[answer(r"friday|8"), lambda t: arg(t, "calendar_add", "start", one_of(FRI_8PM()), "Friday 20:00")]),
    case("calendar_date", "add my flight to lisbon on the 3rd of december at 6 40 am", must=["calendar_add"], checks=[chk_flight]),
    case("birthday", "my mums birthday is on the 14th of march can you add it to my calendar", must=["calendar_add"],
         checks=[chk_yearly("03-14"), answer(r"march")]),
    case("birthday_with_year", "add sarahs birthday shes born on 20/11/1990", must=["calendar_add"],
         checks=[chk_yearly("11-20", 1990)]),
    case("repeat_weekly", "add bin collection to my calendar every tuesday at 7am", must=["calendar_add"],
         checks=[chk_weekly_tuesday]),
    case("birthday_recall", ["my dads birthday is the 2nd of june 1955 put it in the calendar", "/new",
                             "when is dads birthday and how old will he be"],
         must=["calendar_list"], checks=[answer(r"june"), chk_dad_age]),
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
         checks=[answer(r"shellfish", "the saved shellfish allergy")]),
    # --- device, music, TV, camera
    case("music_artist", "play a song by abba", must=["music"], checks=[answer(r"abba|playing")]),
    case("music_genre", "play a random rock song", must=["music"]),
    case("music_stop", "stop the music", must=["music"], checks=[lambda t: arg(t, "music", "action", has("stop"), "stop")]),
    case("music_missing", "play something by pink floyd", must=["music"],
         checks=[not_answer(r"(now )?playing (something|a song|music)? ?(by )?pink floyd|here'?s pink floyd", "claim Pink Floyd is playing"),
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
         checks=[lambda t: arg(t, "weather", "location", has("porto"), "Porto")]),
    case("followup_move", ["put dinner with marta in my calendar for friday at 8pm", "actually make it half past eight"],
         must=["calendar_update|calendar_add"], checks=[chk_moved]),
    case("followup_cancel", ["set a timer for 10 minutes", "cancel that"], must=["timer"], must_not=["reminder_cancel"],
         checks=[chk_timer_action("cancel")]),
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
    case("home_ask_first", "turn on the fan heater", must=["smart_home"], home=EVAL_HOME, checks=[home_asked]),
    case("home_ask_first_yes", ["turn on the fan heater", "yes"], must=["smart_home"], home=EVAL_HOME,
         checks=[home_call("on", ["Fan heater"]), home_switched("Fan heater")]),
    # A refusal, with and without the word "no": nothing is switched on.
    case("home_ask_first_no", ["turn on the fan heater", "no"], home=EVAL_HOME, checks=[home_not_switched("Fan heater")]),
    case("home_ask_first_leave_off", ["turn on the fan heater", "leave it off please"], home=EVAL_HOME,
         checks=[home_not_switched("Fan heater")]),
    # After a web page the change waits for the user's word, and "no" stops it (needs ZB_SEARCH_KEY).
    case("home_after_web_no", ["look up when the sun sets today, then turn on the bedroom lamp", "no, leave it"], home=EVAL_HOME,
         checks=[home_not_switched("Bedroom lamp")]),
    # The page is still part of the conversation a turn later: a change asked for then waits for a yes too.
    case("home_after_web_later", ["look up when the sun sets today", "turn on the bedroom lamp"], must=["smart_home"],
         home=EVAL_HOME, checks=[home_asked]),
    # More than eight things: the model asks in its own words, and a plain yes switches everything off.
    case("home_everything_yes", ["turn everything off", "yes"], must=["smart_home"], home=SOCKETS_HOME, checks=[home_all_off]),
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
         checks=[lambda t: [] if any(str(c.get("at", c.get("start")))[:16] in stamps([tomorrow()], "07:00")
                                     for c in calls(t, "reminder_set|calendar_add")) else ["the reminder should be for tomorrow 07:00"]]),
    case("route_eggs_home", "the eggs need six minutes", must=["timer"], must_not=["smart_home", "reminder_set"],
         home=EVAL_HOME, checks=[chk_timer(360)]),
    case("route_cancel_timer_home", ["set a timer for 10 minutes", "cancel the timer"], must=["timer"],
         must_not=["smart_home", "reminder_cancel"], home=EVAL_HOME, checks=[chk_timer_action("cancel")]),
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
    case("route_lamp_no_home", "turn on the bedroom lamp", must_not=["tv_remote", "device"],
         checks=[answer(CANNOT + r"|smart.home|set up|settings", "that no lamps are set up")]),

    # Radio from the user's own stations, against the music library and with no stations at all.
    case("radio_by_nickname", "play radio 2", must=["radio"], must_not=["music"], radio=EVAL_RADIO,
         checks=[radio_play("Northern Radio 2")]),
    case("radio_by_name", "put on harbour fm", must=["radio"], must_not=["music"], radio=EVAL_RADIO,
         checks=[radio_play("Harbour FM")]),
    case("radio_nickname_phrase", "can you put the jazz station on", must=["radio"], must_not=["music"], radio=EVAL_RADIO,
         checks=[radio_play("Night Jazz 24")]),
    case("radio_stop", ["play radio 2", "stop the radio"], must=["radio|music"], radio=EVAL_RADIO, checks=[stops_player]),
    case("radio_vs_music", "play some abba", must=["music"], must_not=["radio"], radio=EVAL_RADIO,
         checks=[lambda t: arg(t, "music", "action", has("play"), "play")]),
    case("radio_unknown_station", "play radio 4", must_not=["music"], radio=EVAL_RADIO,
         checks=[radio_never_plays, not_answer(r"(now )?playing radio 4|here'?s radio 4|radio 4 is (now )?(on|playing)",
                                                  "claim Radio 4 is playing"),
                 answer(CANNOT + r"|not (one of|in|on)|no (station|radio 4)|isn'?t (one|in|on|a)|aren'?t", "that Radio 4 is not one of the stations")]),
    case("radio_none_set_up", "play radio 2", must_not=["radio"],
         checks=[not_answer(r"(now )?playing radio 2|here'?s radio 2|radio 2 is (now )?(on|playing)", "claim Radio 2 is playing"),
                 answer(RADIO_NOT_SET_UP, "that radio is not set up")]),
]

# ---------------------------------------------------------------- running and scoring
TOOL_RE = re.compile(r"^\s+-> (\w+) (.*)$")
RESULT_RE = re.compile(r"^\s+<- (.*)$")
FIRST_RE = re.compile(r"\[first words \+([\d.]+)s\]")
ANSWER_RE = re.compile(r"^\[answer ([\d.]+)s\] ?(.*)", re.S | re.M)
TURN_RE = re.compile(r"^(?:> )*\[user\] ", re.M)
# Words that say something was done. Said with no tool call in the turn, the claim is false.
CLAIM_RE = re.compile(r"\b(i'?ve (added|saved|set|scheduled|put|noted|cancell?ed|deleted|moved|sent|booked|turned|changed|updated)|"
                      r"(is|it'?s) (now )?(on|in) your calendar|(timer|reminder|alarm) (is |has been )?(set|cancell?ed)|"
                      r"now playing|i'?ll remember|saved (that|it)|got it,? (saved|noted))", re.I)
EMOJI_RE = re.compile("[\U0001F000-\U0001FAFF☀-➿⬀-⯿️]")

def parse_turn(text):
    turn = {"tools": [], "results": [], "answer": "", "total": None, "first": None, "error": None}
    for line in text.splitlines():
        m = TOOL_RE.match(line)
        if m: turn["tools"].append((m.group(1), m.group(2)))
        m = RESULT_RE.match(line)
        if m: turn["results"].append(m.group(1))
        m = FIRST_RE.search(line)
        if m: turn["first"] = float(m.group(1))
        if line.startswith("[error]"): turn["error"] = line[8:]
    m = ANSWER_RE.search(text)
    if m:
        turn["total"] = float(m.group(1))
        turn["answer"] = re.sub(r"(\n> )*\s*$", "", m.group(2)).strip()
    return turn

def run_case(binary, base_env, c):
    # A fresh state directory per case: no calendar entries, memories or time zone carried over.
    state = tempfile.mkdtemp(prefix="zb-eval-")
    os.makedirs(state + "/config/zero-bot")
    settings = {"home_city": "London", "user_name": "Sam"}
    for key in ("llm_model", "stt_model", "tts_model", "music_dir"):
        if os.environ.get("ZB_EVAL_" + key.upper()): settings[key] = os.environ["ZB_EVAL_" + key.upper()]
    json.dump(settings, open(state + "/config/zero-bot/settings.json", "w"))
    env = dict(base_env, XDG_CONFIG_HOME=state + "/config", XDG_DATA_HOME=state + "/data", XDG_CACHE_HOME=state + "/cache")
    if c.get("radio"):
        json.dump(c["radio"], open(state + "/config/zero-bot/radio.json", "w"))
    if c.get("home"):
        # The made-up home lives on the stand-in network, where only 127.x addresses could ever be
        # contacted; with ZB_DRY_RUN none is.
        os.makedirs(state + "/data/zero-bot")
        json.dump(c["home"], open(state + "/data/zero-bot/smarthome.json", "w"))
        env["ZB_TEST_NETWORK"] = "1"
    for k, v in c["env"].items():
        if v is None: env.pop(k, None)
        else: env[k] = v
    cmd = [binary, "--chat"] + (["--steer", c["steer"]] if c["steer"] else [])
    t0 = time.time()
    try:
        out = subprocess.run(cmd, input="\n".join(c["turns"]) + "\n", env=env, capture_output=True, text=True,
                             timeout=90 * len(c["turns"])).stdout
        failed = None
    except subprocess.TimeoutExpired as e:
        out, failed = (e.stdout or b"").decode("utf-8", "replace") if isinstance(e.stdout, bytes) else (e.stdout or ""), "timeout"
    shutil.rmtree(state, ignore_errors=True)
    turns = [parse_turn(seg) for seg in TURN_RE.split(out)[1:]]
    expected = sum(1 for t in c["turns"] if t != "/new")
    while len(turns) < expected:
        turns.append({"tools": [], "results": [], "answer": "", "total": time.time() - t0, "first": None,
                      "error": failed or "no output for this turn"})
    return turns

def score(c, turns):
    problems = []
    for i, t in enumerate(turns):
        where = "" if len(turns) == 1 else "turn %d: " % (i + 1)
        a = t["answer"]
        if t["error"]: problems.append(where + "error: " + t["error"][:80])
        if not a: problems.append(where + "empty answer")
        if re.search(r"[*#`]|^\s*[-•]\s|^\s*\d+[.)]\s", a, re.M): problems.append(where + "markdown in answer")
        if re.search(r"</?think", a): problems.append(where + "reasoning marker in answer")
        if EMOJI_RE.search(a): problems.append(where + "emoji in answer")
        if re.search(r"https?://|www\.", a): problems.append(where + "web address in answer")
        if len(a) > 420: problems.append(where + "answer too long (%d chars)" % len(a))
        if not t["tools"] and CLAIM_RE.search(a): problems.append(where + "claims an action but called no tool")
    last = turns[-1]
    called = [n for n, _ in last["tools"]]
    for want in c["must"]:
        if not any(n in want.split("|") for n in called): problems.append("did not call " + want)
    for bad in c["must_not"]:
        if bad == "*" and called: problems.append("called tools when none were needed: " + ",".join(called))
        elif bad in called: problems.append("should not call " + bad)
    for check in c["checks"]:
        try: problems += check(last)
        except Exception as e: problems.append("check could not run: %r" % e)
    return problems

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="build/cli/zero_bot")
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--only", default="", help="run cases whose name contains this (comma-separated alternatives)")
    ap.add_argument("--fast", default=None, help="force fast mode on (1) or off (0)")
    ap.add_argument("--jobs", type=int, default=1, help="cases to run at once (timings are only comparable at 1)")
    ap.add_argument("--json", default="", help="write full results to this file")
    ap.add_argument("--rescore", default="", help="score a results file written by --json again instead of running (same day; clock-time checks will fail)")
    args = ap.parse_args()
    saved = {}
    if args.rescore:
        for r in json.load(open(args.rescore)): saved.setdefault(r["case"], []).append(r["turns"])
    elif not os.environ.get("ZB_API_BASE") or not os.environ.get("ZB_API_KEY"):
        sys.exit("ZB_API_BASE and ZB_API_KEY must be set")

    env = dict(os.environ, ZB_DRY_RUN="1", ZB_NO_AUDIO="1")
    if os.environ.get("ZB_EVAL_CAMERA"): env["ZB_FAKE_CAMERA"] = os.environ["ZB_EVAL_CAMERA"]
    if args.fast is not None: env["ZB_FAST_MODE"] = args.fast

    wanted = [w for w in args.only.split(",") if w]
    cases = [c for c in CASES if not wanted or any(w in c["name"] for w in wanted)]
    jobs = [c for c in cases for _ in range(len(saved.get(c["name"], [])) if args.rescore else args.repeat)]
    run = (lambda c: saved[c["name"]].pop(0)) if args.rescore else (lambda c: run_case(args.bin, env, c))
    results, failures, failed_names = [], 0, {}
    with concurrent.futures.ThreadPoolExecutor(max(1, args.jobs)) as pool:
        for c, turns in zip(jobs, pool.map(run, jobs)):
            problems = score(c, turns)
            failures += bool(problems)
            if problems: failed_names[c["name"]] = failed_names.get(c["name"], 0) + 1
            for i, t in enumerate(turns):
                shown = ", ".join("%s%s" % (n, a if len(a) < 70 else a[:67] + "...") for n, a in t["tools"]) or "-"
                first = "%.1f" % t["first"] if t["first"] is not None else " - "
                tag = ("PASS" if not problems else "FAIL") if i == len(turns) - 1 else "    "
                print("%-4s %-21s first %4ss total %5.1fs  [%s]" % (tag, c["name"], first, t["total"] or 0, shown))
                print("       \"%s\"" % t["answer"][:220].replace("\n", " / "))
            for p in problems: print("       !! " + p)
            sys.stdout.flush()
            results.append({"case": c["name"], "requests": c["turns"], "problems": problems, "turns": turns})
    n = len(results)
    totals = sorted(t["total"] for r in results for t in r["turns"] if t["total"] is not None)
    firsts = sorted(t["first"] for r in results for t in r["turns"] if t["first"] is not None)
    med = lambda v: v[len(v) // 2] if v else float("nan")
    p90 = lambda v: v[min(len(v) - 1, int(len(v) * 0.9))] if v else float("nan")
    print("\n%d/%d passed | per request: total median %.1fs, mean %.1fs, 90th %.1fs, worst %.1fs | first words: median %.1fs, 90th %.1fs"
          % (n - failures, n, med(totals), sum(totals) / max(1, len(totals)), p90(totals), totals[-1] if totals else 0,
             med(firsts), p90(firsts)))
    if failed_names:
        print("failed: " + ", ".join("%s%s" % (k, " x%d" % v if args.repeat > 1 else "") for k, v in sorted(failed_names.items())))
    if args.json: json.dump(results, open(args.json, "w"), indent=1)
    sys.exit(1 if failures else 0)

if __name__ == "__main__":
    main()
