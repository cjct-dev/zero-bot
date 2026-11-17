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

Side effects are disabled (ZB_DRY_RUN): no music plays and no infrared is sent. Every case
starts from an empty calendar, memory and settings.
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

def chk_ninety(t):
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
def case(name, request, must=(), must_not=(), checks=(), env=None, steer=None):
    """request: one utterance or a list (a conversation; "/new" starts a fresh one, memories are kept).
    must / must_not / checks apply to the last turn; speech-suitability rules apply to every turn."""
    return {"name": name, "turns": [request] if isinstance(request, str) else list(request), "must": list(must),
            "must_not": list(must_not), "checks": list(checks), "env": env or {}, "steer": steer}

FRI_8PM = lambda: stamps(next_weekday(4), "20:00")
CANNOT = r"can'?t|cannot|not able|unable|don'?t have|no way|not something|isn'?t something|not set up|only"
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
    case("timer", "set a timer for 12 minutes", must=["reminder_set"], must_not=["calendar_add"],
         checks=[answer(r"12|twelve"), lambda t: arg(t, "reminder_set", "in_minutes", num_is(12), "12")]),
    case("timer_seconds", "set a timer for 90 seconds", must=["reminder_set"],
         checks=[lambda t: arg(t, "reminder_set", "in_minutes", num_is(1.5), "1.5")]),
    case("reminder_clock", "remind me at 7 tomorrow morning to call the hotel", must=["reminder_set|calendar_add"],
         checks=[answer(r"7|seven"), lambda t: [] if any(str(c.get("at", c.get("start")))[:16] in stamps([tomorrow()], "07:00")
                                                         for c in calls(t, "reminder_set|calendar_add")) else ["should be set for tomorrow 07:00"]]),
    case("reminder_relative", "remind me in an hour and a half to take my tablets", must=["reminder_set"], checks=[chk_ninety]),
    case("calendar_add", "put dinner with marta in my calendar for friday at 8pm", must=["calendar_add"],
         checks=[answer(r"friday|8"), lambda t: arg(t, "calendar_add", "start", one_of(FRI_8PM()), "Friday 20:00")]),
    case("calendar_date", "add my flight to lisbon on the 3rd of december at 6 40 am", must=["calendar_add"], checks=[chk_flight]),
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
    case("followup_cancel", ["set a timer for 10 minutes", "cancel that"], must=["reminder_cancel"]),
    case("followup_convert", ["how much is 40 euros in pounds", "and in dollars"], must=["convert_currency"],
         checks=[lambda t: arg(t, "convert_currency", "to", has("usd"), "USD"), lambda t: arg(t, "convert_currency", "amount", num_is(40), "40")]),
    # --- input added while the request is running
    case("steer", "whats the weather in lisbon tomorrow", steer="sorry i meant porto", must=["weather"],
         checks=[lambda t: arg(t, "weather", "location", has("porto"), "Porto"), answer(r"porto")]),
    # --- requests that should get a question or an honest "no", not a guess
    case("clarify_reminder", "set a reminder", must_not=["reminder_set", "calendar_add"], checks=[answer(r"\?", "a clarifying question")]),
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
    case("german_timer", "stell einen timer auf fünf minuten", must=["reminder_set"],
         checks=[lambda t: arg(t, "reminder_set", "in_minutes", num_is(5), "5"), answer(r"minuten|timer|läuft|gestellt", "a German reply")]),
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
