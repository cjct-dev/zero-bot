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
--json OUT (full results), --rescore OUT (score saved results again; same day only). It needs
ZB_API_BASE and ZB_API_KEY (and model names in ZB_EVAL_LLM_MODEL etc.); cases tagged "search" are
left out without ZB_SEARCH_KEY. Every case starts from an empty state with ZB_DRY_RUN, as in the
scored set. The summary gives the pass rate per tag and the slowest cases.

Checks are declarative in cases.json ({"kind": ...}); KINDS below compiles each one. Expected
dates, times, amounts and ages are worked out when the case runs. A check looks at the last turn,
or at the one its "turn" names (0 is the first, -1 the last; "/new" is not a turn); "long_ok" lists
the turns whose answer may be longer than the usual cap (analytical follow-ups).
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
sys.dont_write_bytecode = True   # importing agent_eval leaves no __pycache__ in tests/
import agent_eval as ev   # noqa: E402

HOMES = {"eval": ev.EVAL_HOME, "tv": ev.TV_HOME, "sockets": ev.SOCKETS_HOME}
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


def says_date(text, d):
    t = text.lower()
    if d.isoformat() in t: return True
    month = MONTHS[d.month - 1]
    return bool(re.search(r"\b(%s|%s)\b" % (month, month[:3]), t) and re.search(r"\b%d(st|nd|rd|th)?\b" % d.day, t)) or \
        bool(re.search(r"\b(%s|%s)\b" % (month, month[:3]), t) and re.search(r"\b%s\b" % ev.words(d.day), t))


def says_number(text, values, tol):
    for v in values:
        if ev.says_about(text, v, tol): return True
        if float(v).is_integer() and ev.says_int(text, int(v)): return True
    return False


# ------------------------------------------------------------------ check kinds
def k_answer(s): return ev.answer(s["re"], s.get("what"))
def k_not_answer(s): return ev.not_answer(s["re"], s.get("what") or "match /%s/" % s["re"])


def k_arg(s):
    tool, key = s["tool"], s["key"]
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
    seconds = s["seconds"]

    def check(t):
        if any(abs(ev.timer_seconds(c) - seconds) < 1 for c in ev.timer_calls(t, "start")): return []
        for c in ev.calls(t, "reminder_set"):
            try:
                if c.get("in_minutes") is not None and abs(float(c["in_minutes"]) * 60 - seconds) < 1: return []
            except (TypeError, ValueError): pass
            if c.get("at"):
                try: due = datetime.datetime.strptime(str(c["at"])[:16], "%Y-%m-%d %H:%M")
                except ValueError: continue
                if abs((due - ev.today()).total_seconds() - seconds) < 180: return []
        return ["a timer or reminder should go off %d seconds from now, got %s" % (seconds, ev.calls(t, "timer|reminder_set"))]
    return check


def k_timer_becomes(s):
    """The running timer now has a new length: added to, or replaced by a new one of that length."""
    def check(t):
        if any(abs(ev.timer_seconds(c) - s["added"]) < 1 for c in ev.timer_calls(t, "add_time")): return []
        if any(abs(ev.timer_seconds(c) - s["seconds"]) < 1 for c in ev.timer_calls(t, "start")): return []
        return ["the timer should gain %d seconds or be restarted for %d, got %s" % (s["added"], s["seconds"], ev.calls(t, "timer"))]
    return check


def k_at(s):
    def check(t):
        ok = stamps_for(s["day"], s["times"])
        for c in ev.calls(t, s["tools"]):
            when = str(c.get("at", c.get("start")))[:16]
            if when in ok and (not s.get("weekly") or c.get("repeat") == "weekly"): return []
        return ["%s should be for %s%s, got %s" % (s["tools"], " or ".join(ok), " repeating weekly" if s.get("weekly") else "",
                                                   ev.calls(t, s["tools"]))]
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
    return lambda t: ["should not call %s" % s["tool"]] if ev.calls(t, s["tool"]) else []


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


def k_home_call(s): return ev.home_call(s["action"], s.get("targets"), **s.get("values", {}))
def k_home_never_on(s): return ev.home_never_on
def k_home_asked(s): return ev.home_asked
def k_home_switched(s): return ev.home_switched(s["name"])
def k_home_not_switched(s): return ev.home_not_switched(s["name"])
def k_home_all_off(s): return ev.home_all_off
def k_home_step(s): return ev.home_step(s["sign"])


KINDS = {name[2:]: f for name, f in globals().items() if name.startswith("k_") and callable(f)}


def compile_check(s):
    """One check; with "turn", it carries that turn's number for tests/agent_eval.py's score()."""
    f = KINDS[s["kind"]](s)
    if "turn" not in s: return f
    def on_turn(t): return f(t)
    on_turn.turn = s["turn"]
    return on_turn


def compile_case(c):
    """A case from cases.json in the form tests/agent_eval.py runs and scores."""
    out = dict(c, checks=[compile_check(s) for s in c["checks"]], home=HOMES[c["home"]] if c.get("home") else None)
    return out


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
    ap.add_argument("--slowest", type=int, default=10, help="how many of the slowest cases to list")
    args = ap.parse_args()

    cases = json.load(open(args.cases, encoding="utf-8"))
    chosen = selected(cases, args)
    if not args.rescore and not args.list and not os.environ.get("ZB_SEARCH_KEY"):
        dropped = [c for c in chosen if "search" in c["tags"] and not c.get("env", {}).get("ZB_SEARCH_KEY", "x") is None]
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
        for r in json.load(open(args.rescore)): saved.setdefault(r["case"], []).append(r["turns"])
        chosen = [c for c in chosen if c["name"] in saved]
    elif not os.environ.get("ZB_API_BASE") or not os.environ.get("ZB_API_KEY"):
        sys.exit("ZB_API_BASE and ZB_API_KEY must be set")

    # An empty home folder: the music library (~/Music unless ZB_EVAL_MUSIC_DIR names one) and anything
    # else under the home of whoever runs this stay out of the results.
    home = tempfile.mkdtemp(prefix="zb-suite-home-")
    env = dict(os.environ, ZB_DRY_RUN="1", ZB_NO_AUDIO="1", HOME=home)
    if os.environ.get("ZB_EVAL_CAMERA"): env["ZB_FAKE_CAMERA"] = os.environ["ZB_EVAL_CAMERA"]
    if args.fast is not None: env["ZB_FAST_MODE"] = args.fast
    compiled = {c["name"]: compile_case(c) for c in chosen}
    jobs = [c for c in chosen for _ in range(len(saved.get(c["name"], [])) if args.rescore else args.repeat)]
    run = (lambda c: saved[c["name"]].pop(0)) if args.rescore else (lambda c: ev.run_case(args.bin, env, compiled[c["name"]]))

    results, failures, failed_names, per_tag = [], 0, {}, {}
    with concurrent.futures.ThreadPoolExecutor(max(1, args.jobs)) as pool:
        for c, turns in zip(jobs, pool.map(run, jobs)):
            problems = ev.score(compiled[c["name"]], turns)
            failures += bool(problems)
            if problems: failed_names[c["name"]] = failed_names.get(c["name"], 0) + 1
            for t in c["tags"]:
                ok, n = per_tag.get(t, (0, 0))
                per_tag[t] = (ok + (not problems), n + 1)
            for i, t in enumerate(turns):
                shown = ", ".join("%s%s" % (n, a if len(a) < 70 else a[:67] + "...") for n, a in t["tools"]) or "-"
                first = "%.1f" % t["first"] if t["first"] is not None else " - "
                tag = ("PASS" if not problems else "FAIL") if i == len(turns) - 1 else "    "
                print("%-4s %-16s first %4ss total %5.1fs  [%s]" % (tag, c["name"], first, t["total"] or 0, shown))
                print("       \"%s\"" % t["answer"][:220].replace("\n", " / "))
            for p in problems: print("       !! " + p)
            sys.stdout.flush()
            results.append({"case": c["name"], "tags": c["tags"], "requests": c["turns"], "problems": problems, "turns": turns,
                            "seconds": sum(t["total"] or 0 for t in turns)})
    n = len(results)
    if not n:
        print("no cases selected")
        shutil.rmtree(home, ignore_errors=True)
        return
    totals = sorted(t["total"] for r in results for t in r["turns"] if t["total"] is not None)
    firsts = sorted(t["first"] for r in results for t in r["turns"] if t["first"] is not None)
    med = lambda v: v[len(v) // 2] if v else float("nan")
    p90 = lambda v: v[min(len(v) - 1, int(len(v) * 0.9))] if v else float("nan")
    print("\n%d/%d passed (%.0f%%) | per request: total median %.1fs, mean %.1fs, 90th %.1fs, worst %.1fs | first words: median %.1fs, 90th %.1fs"
          % (n - failures, n, 100.0 * (n - failures) / n, med(totals), sum(totals) / max(1, len(totals)), p90(totals),
             totals[-1] if totals else 0, med(firsts), p90(firsts)))
    print("by tag: " + ", ".join("%s %d/%d" % (t, ok, k) for t, (ok, k) in sorted(per_tag.items(), key=lambda kv: (kv[1][0] / kv[1][1], kv[0]))))
    slow = sorted(results, key=lambda r: -r["seconds"])[:args.slowest]
    print("slowest: " + ", ".join("%s %.1fs" % (r["case"], r["seconds"]) for r in slow))
    if failed_names:
        print("failed: " + ", ".join("%s%s" % (k, " x%d" % v if args.repeat > 1 else "") for k, v in sorted(failed_names.items())))
    if args.json: json.dump(results, open(args.json, "w"), indent=1)
    shutil.rmtree(home, ignore_errors=True)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
