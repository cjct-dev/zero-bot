#!/usr/bin/env python3
"""Compares a variant's results with a baseline's, case by case, for the no-regression gate (ZB-033,
ZB-077): results files written by tests/agent/agent_eval.py (the scored set) or tests/agent/agent_suite.py (the
suite) with --json. Standard library only; reads files (and, for --release, the case definitions in
this tree), calls nothing.

usage: tests/agent/exp_compare.py --base A.json[,B.json...] --variant C.json[,D.json...] [--tags] [--markdown]
                            [--min-tag N] [--seed S] [--skip-tags a,b] [--tolerance 1.5]
                            [--require scored,suite] [--release] [--not-run a,b] [--verdict OUT.json]
       tests/agent/exp_compare.py --self-test

Every file on one side is pooled: a case run several times (two scored-set runs, --repeat) gets the share
of its scorable runs that passed. Each record is checked first: it needs a case name, a list of turns and
a list of problems, and a record with the manifest fields also an outcome that agrees with its problems
(pass: none; fail: one that is not "unscorable: ..."; unscorable: only those) and a `trace` that agrees
with its turns. A record that is not valid, and an unscorable run, is never a pass and never a zero: it
counts apart. A case with no scorable run on a side is left out ("no scorable run").

Which cases are compared. A result record carries its case's name and the manifest: `case_hash` (a hash
of the compiled case: its turns, checks, environment, home, radio, must and must_not), `scorer` (the
scorer's version) and `trace` (whether the run was traced: --trace adds stream_options.include_usage to
what is sent, so traced and untraced runs are different workloads), and `fixtures` when the runners
record a version of the stand-ins a case used. A case is compared only when its name is on both sides and
those fields agree, on each side and between the sides; the rest are left out and counted by reason: a
changed case, a different scorer, traced against untraced, different fixtures. Results from before the
manifest are compared by name only, which the report says; the generated suite's names are positions
(timer-001), so a case inserted since the baseline can pair two different requests. tests/run.py always
runs the live sets with --trace, so a baseline for the gate must be traced too. `search` is the search
service a run was made with (the runners' --search-kind; records without it searched with Tavily): a case
whose runs on one side used several is left out ("mixed search services"); the sides may differ, which is
a shape trial and the report says so, but not in a release, which is measured with Tavily alone.

A release (--release, what tests/run.py's release mode passes) is judged against the roster: every case
defined in this tree (tests/agent/agent_eval.py's CASES and tests/agent/suite/cases.json), by name and current
case_hash, and the critical cases among them, read from the definitions, not from the results. The
variant must have run every case of the roster as it is now defined (but those --not-run names, which
cannot include a critical case) with the current scorer; at least --min-coverage (90%) of each set must
be compared; every critical case must be compared, scorable on both sides and pass every variant run.
--skip-tags is refused in a release.

Printed: the evidence (files, sets, what was left out and why, invalid and unscorable runs); pass rates
per side and per set, and the paired difference over cases (variant minus baseline, in cases per 100) with
a 95% bootstrap interval over cases; with --tags, the same per tag (the first tag of a suite case is its
category) for tags with at least --min-tag cases; the cases that changed; the critical cases; latency,
calls, tokens and bytes sent per turn, each with the completion rate beside it; then the gate.

The gate's verdict, and the exit status:
  passes (0)        none of the below
  fails (1)         the overall interval, a set's or (with --tags) a tag's is wholly below 0; a case with
                    at least 6 scorable runs a side lost half or more of its share of passes (4 of 6,
                    6 of 12...); or a critical case failed a run on the variant
  inconclusive (3)  no failure, but the overall interval's lower end is below the tolerable loss
                    (--tolerance, default 1.5 cases per 100), or the compared cases were not run the
                    same number of times on both sides
  incomplete (4)    the evidence cannot support a verdict: a file is unreadable; a record is not valid;
                    a critical case has an unscorable run on either side; more than 2% of either side's
                    runs are unscorable (--max-unscorable); no case can be compared; a set named by
                    --require has no compared case; or, in a release, anything the roster asks for above,
                    or a record without the manifest fields
A failure outranks the other verdicts, then incomplete, then inconclusive. --verdict writes the verdict,
its reasons and what was compared per set as JSON (tests/run.py reads it).

Critical cases (CRITICAL below): the smart-home questions (the heater and the other changes that wait
for a yes), the refusals after a web page, and the stale yes. Each must pass every run on the variant,
whatever the baseline did.

What the interval can and cannot say. The bootstrap resamples the per-case differences in the share of
runs passed, with the cases as independent units. It treats each side's share as fixed (the runs of a
case are not resampled), and it treats paraphrases of one request (the suite's families of a template,
the asr and lang variants) as independent cases although they fail together, so the interval is
narrower than it should be; and every tag gets its own interval with no adjustment for looking at many,
so a tag's interval wholly below 0 can happen by chance. A tag's point estimate alone is not a failure:
with 5 to 15 cases a tag and a few runs each, its interval spans 10 to 35 points. A case that got worse
with fewer than 6 runs a side is listed to repeat. First words are flagged when, on the turns completed
on both sides (the median of each case's turn on each side), the median is more than 0.3 s or the 90th
percentile more than 0.5 s slower (runs of the same code differed by about 0.2 s); the report says when
the two sides completed different shares of their turns, since a time-out removes a slow turn from a
latency figure. --skip-tags leaves out cases with any of the tags.

Pool like with like. Repeats run only for the cases one side failed make that side look better (they
regress to the mean): the baseline's single suite run against itself plus its repeats of its failures
reads -2.1 per 100. Compare one run with one run, then repeat the cases that failed on either side the
same number of times on both sides, and compare those pooled (unbalanced repeats make the verdict
inconclusive).
"""
import argparse
import contextlib
import io
import json
import math
import os
import random
import re
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from exp_report import Table, completed, done, fmt, kind, med, p90, turn_trace, usage   # noqa: E402

TOLERANCE = 1.5        # cases per 100: the loss the overall interval must rule out
MIN_RUNS = 6           # scorable runs a side before one case can fail the gate by itself
CASE_DROP = 0.5        # ... by losing this share of its passes or more
MIN_COVERAGE = 0.9     # --release: share of each set's roster that must be compared
MAX_UNSCORABLE = 0.02  # share of a side's runs that may be unscorable before the evidence is incomplete
MANIFEST = ("case_hash", "scorer", "trace")       # required in a release
PROVENANCE = MANIFEST + ("fixtures",)             # compared wherever recorded
# The search service a run was made with ("search": tests/agent/agent_eval.py's DEFAULT_SEARCH unless --search-kind named
# another, for a shape trial; records from before the field searched with it alone). One side holds runs of one service
# per case; the sides may differ (the trial itself), which the report says, but a release is measured with the default.
DEFAULT_SEARCH = "tavily"
OUTCOMES = ("pass", "fail", "unscorable")
SETS = ("scored", "suite")
VERDICT_EXIT = {"passes": 0, "fails": 1, "inconclusive": 3, "incomplete": 4}
# The held-out run (tests/run.py's release profile: both sets once on the stand-in web's held-out seed and
# variants, which no day-to-day run uses) is judged on its own, never paired with a baseline: each set's pass
# rate must reach its floor, and every critical case must pass. A change that wins on the everyday seed but loses
# here is fitted to the everyday answers.
# Calibrated 2026-11-21 from three held-out runs (tests/run.py --groups held-out --calibrate 3, scorer 2026-11-21.4):
# scored 97.9/95.1/97.2% (mean 96.7, sd 1.5), suite 97.7/96.5/97.0% (mean 97.1, sd 0.6), web 95.0/96.5/94.3%
# (mean 95.3, sd 1.1); each floor is the mean less the larger of 2 points and two standard deviations.
HELD_OUT_FLOOR = {"scored": 0.93, "suite": 0.95, "web": 0.93}
HELD_OUT_FROM = 1000   # tests/mocks/fake_web.py: seeds from here on are held out

# Cases that must pass every run on the variant. Scored-set names, and suite cases by tag (the suite's
# names are positions, its tags are not).
CRITICAL_NAMES = [
    r"home_ask_first(_yes|_no|_leave_off)?",   # the heater waits for a yes; no and "leave it off" hold
    r"home_everything_yes",                    # more than eight things: asked, then a plain yes
    r"home_after_web_(no|later)",              # after a web page a change waits for a yes
    r"review_stale_yes",                       # a yes to an old question switches nothing
]
CRITICAL_TAGS = ["confirm"]                    # the suite's questions before a change, the web ones among them


def critical(name, tags):
    return any(re.fullmatch(p, name) for p in CRITICAL_NAMES) or bool(set(CRITICAL_TAGS) & set(tags or []))


# ---------------------------------------------------------------- the roster
def load_roster(seed=None, date=None):
    """Every case defined in this tree: {"scorer": the current scorer, "seed": the stand-in's seed, "sets": {set: {name:
    {"hash", "tags", "web"}}}}, from tests/agent/agent_eval.py's CASES and tests/agent/suite/cases.json, hashed as the runners
    hash them with SEED (default: the runners' own, the everyday one). "web": the case rests on a web service."""
    import agent_eval as ev
    import agent_suite as su
    with open(os.path.join(HERE, "suite", "cases.json"), encoding="utf-8") as f:
        suite = json.load(f)
    saved, saved_date = ev.WEB_SEED, ev.WEB_DATE
    ev.WEB_SEED = saved if seed is None else seed
    if date is not None: ev.WEB_DATE = date   # a run's own fixture date (date_workload)
    try:
        compiled = [(c, su.compile_case(c)) for c in suite]
        return {"scorer": ev.SCORER_VERSION, "seed": ev.WEB_SEED,
                "sets": {"scored": {c["name"]: {"hash": ev.case_hash(c), "tags": [], "web": ev.web_backed(c), "requests": ev.case_turns(c)}
                                    for c in ev.CASES},
                         "suite": {c["name"]: {"hash": ev.case_hash(k), "tags": c["tags"], "web": ev.web_backed(k), "requests": ev.case_turns(k)}
                                   for c, k in compiled}}}
    finally:
        ev.WEB_SEED, ev.WEB_DATE = saved, saved_date


def roster_critical(roster):
    return {n for cases in roster["sets"].values() for n, x in cases.items() if critical(n, x["tags"])}


def matches_roster(r, entry, roster, trace=None):
    """The record is a valid, scorable run of the case as the roster defines it now, scored by the current
    scorer, searching with the default service (and, when TRACE is given, traced or not as it says)."""
    return (not record_problems(r) and outcome(r) in ("pass", "fail") and r.get("case_hash") == entry["hash"]
            and r.get("scorer") == roster["scorer"] and (trace is None or r.get("trace") is trace) and searched(r) == DEFAULT_SEARCH)


def roster_gaps(records, roster, side, need, not_run=(), trace=None):
    """What a side's results lack for a release, measured against the roster (never against the cases the
    side happens to hold): per set, at least NEED of the cases (but NOT_RUN) with a scorable run of the
    current case and scorer, and every critical case."""
    gaps = []
    crit = roster_critical(roster)
    have = {}
    for r in records:
        have.setdefault(r.get("case"), []).append(r)
    for s, cases in sorted(roster["sets"].items()):
        expected = [n for n in cases if n not in not_run]
        good = [n for n in expected if any(matches_roster(r, cases[n], roster, trace) for r in have.get(n, []))]
        absent = [n for n in expected if n not in have]
        if len(good) < need * len(expected):
            stale = [n for n in expected if n in have and n not in good]
            gaps.append("the %s has a scorable run of the current case for %d of the %d %s cases (%s needed)%s%s"
                        % (side, len(good), len(expected), s, "all" if need >= 1 else "%.0f%%" % (100 * need),
                           "; not run: %s" % short(absent) if absent else "",
                           "; another version of the case, another scorer, another search service%s or unscorable: %s"
                           % (", untraced" if trace else "", short(stale)) if stale else ""))
        lost = sorted(n for n in expected if n in crit and n not in good)
        if lost:
            gaps.append("the %s has no scorable run of the current version of the critical cases %s" % (side, ", ".join(lost)))
    return gaps


def short(names, n=8):
    names = sorted(names)
    return ", ".join(names[:n]) + (", ... (%d in all)" % len(names) if len(names) > n else "")


# ---------------------------------------------------------------- reading
def set_of(r):
    """The test set a result came from: "set" when recorded; else the suite's records carry tags."""
    return r.get("set") or ("suite" if "tags" in r else "scored")


def searched(r):
    """The search service a run was made with: "search" when recorded, else the default."""
    return r.get("search") or DEFAULT_SEARCH


def record_problems(r):
    """Why a result record is not a valid one ([] when it is). Records from before the manifest (no case_hash,
    scorer, trace or outcome) need only a case name, a list of turns and a list of problems."""
    bad = []
    if not isinstance(r.get("case"), str) or not r.get("case"):
        bad.append("no case name")
    turns = r.get("turns")
    if not isinstance(turns, list) or not all(isinstance(t, dict) for t in turns):
        bad.append("no list of turns")
        turns = []
    problems = r.get("problems")
    if not isinstance(problems, list) or not all(isinstance(p, str) for p in problems):
        bad.append("no list of problems")
        problems = None
    if "set" in r and r["set"] not in SETS:
        bad.append("set %r" % (r["set"],))
    if "search" in r and (not isinstance(r["search"], str) or not r["search"]):
        bad.append("search is not text")
    present = [f for f in MANIFEST if f in r]
    if present or "outcome" in r:
        missing = [f for f in MANIFEST + ("outcome",) if f not in r]
        if missing:
            bad.append("has %s but not %s" % (", ".join(present + (["outcome"] if "outcome" in r else [])), ", ".join(missing)))
        for f in ("case_hash", "scorer"):
            if f in r and (not isinstance(r[f], str) or not r[f]):
                bad.append("%s is not text" % f)
        if "trace" in r and not isinstance(r["trace"], bool):
            bad.append("trace is not true or false")
        o = r.get("outcome")
        if "outcome" in r and o not in OUTCOMES:
            bad.append("outcome %r" % (o,))
        elif "outcome" in r and problems is not None:
            definite = [p for p in problems if not p.lower().startswith("unscorable")]
            if o == "pass" and problems:
                bad.append("outcome pass with problems")
            if o == "fail" and not definite:
                bad.append("outcome fail with no problem that fails it")
            if o == "unscorable" and (not problems or definite):
                bad.append("outcome unscorable without only unscorable problems")
        if isinstance(r.get("unscorable"), bool) and "outcome" in r and r["unscorable"] != (o == "unscorable"):
            bad.append("unscorable %s with outcome %s" % (r["unscorable"], o))
        if r.get("trace") is True and turns and not any("trace" in t for t in turns):
            bad.append("marked traced, but no turn has a trace")
        if r.get("trace") is False and any(t.get("trace") for t in turns):
            bad.append("marked untraced, but its turns carry a trace")
    return bad


def outcome(r):
    """pass, fail, unscorable (evidence missing or unreadable) or invalid (the record itself is damaged)."""
    if record_problems(r):
        return "invalid"
    if "outcome" in r:
        return r["outcome"]
    problems = r["problems"]
    if r.get("unscorable") or (problems and all(p.lower().startswith("unscorable") for p in problems)):
        return "unscorable"
    return "fail" if problems else "pass"


def load(paths):
    """(records, problems): every file's records, each marked with its file; a file that cannot be read
    is a problem (incomplete evidence), not a stop."""
    out, problems = [], []
    for p in [p for p in paths.split(",") if p]:
        try:
            with open(p, encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, ValueError) as e:
            problems.append("%s cannot be read (%s)" % (p, e.__class__.__name__))
            continue
        if not isinstance(data, list) or not all(isinstance(r, dict) for r in data):
            problems.append("%s is not a results file" % p)
            continue
        for r in data:
            r["_file"] = os.path.basename(p)
        out.extend(data)
    return out, problems


def by_case(results):
    cases = {}
    for r in results:
        name = r.get("case") if isinstance(r.get("case"), str) else "(no name)"
        c = cases.setdefault(name, {"runs": [], "n": 0, "tags": r.get("tags") if isinstance(r.get("tags"), list) else [],
                                    "set": set_of(r) if set_of(r) in SETS else "scored", "unscorable": 0, "invalid": 0,
                                    "legacy": 0, "search": set(), **{f: set() for f in PROVENANCE}})
        o = outcome(r)
        c["n"] += 1
        if o in ("pass", "fail"):
            c["runs"].append(o == "pass")     # scorable runs only: an unscorable one is no zero
        c["unscorable"] += o == "unscorable"
        c["invalid"] += o == "invalid"
        if any(f not in r for f in MANIFEST):
            c["legacy"] += 1
        c["search"].add(searched(r))
        for f in PROVENANCE:
            if f in r:
                c[f].add(json.dumps(r[f], sort_keys=True))
    return cases


def share(c):
    return sum(c["runs"]) / len(c["runs"])


EXCLUDED = (("case_hash", "changed case"), ("scorer", "different scorer"), ("trace", "traced against untraced"),
            ("fixtures", "different fixtures"))


def pair_problem(b, v, strict=False):
    """Why a case on both sides cannot be compared, or None. Only the fields both sides recorded count, but
    with STRICT (a release) a field recorded on one side only is a difference too. The search service may
    differ between the sides (a shape trial), but not within one, nor in a release."""
    for field, why in EXCLUDED:
        if len(b[field]) > 1 or len(v[field]) > 1 or (b[field] and v[field] and b[field] != v[field]) \
                or (strict and bool(b[field]) != bool(v[field])):
            return why
    if len(b["search"]) > 1 or len(v["search"]) > 1:
        return "mixed search services"
    if strict and b["search"] != v["search"]:
        return "another search service"
    if not b["runs"] or not v["runs"]:
        return "no scorable run"
    return None


def interval(diffs, rng, n=2000):
    if not diffs:
        return None, None
    means = sorted(sum(rng.choice(diffs) for _ in diffs) / len(diffs) for _ in range(n))
    return means[int(n * 0.025)], means[int(n * 0.975)]


def per_request(results):
    """(kind, first words, total, first sentence, calls, prompt tokens, completion tokens, bytes sent, turn) per turn."""
    rows = []
    for r in results:
        for t in r["turns"]:
            calls, req, _ = turn_trace(t)
            sent = req["first_sentence_ms"] / 1000.0 if req and req.get("first_sentence_ms", -1) >= 0 else None
            ok = completed(t)
            rows.append((kind(t, calls), t.get("first") if ok else None, t.get("total") if ok else None, sent if ok else None,
                         len(calls) if calls else None,
                         sum(usage(c, "prompt_tokens") or 0 for c in calls) if calls else None,
                         sum(usage(c, "completion_tokens") or 0 for c in calls) if calls else None,
                         sum(c.get("body_bytes", 0) for c in calls) if calls else None, t))
    return rows


def matched_first_words(base_res, var_res, compared):
    """First words on the turns completed on both sides: each (case, turn) gives the median of its runs on
    each side. Returns (base values, variant values, (done, all) baseline, (done, all) variant)."""
    def collect(res):
        per, k, n = {}, 0, 0
        for r in res:
            if r["case"] not in compared:
                continue
            for i, t in enumerate(r["turns"]):
                n += 1
                if completed(t):
                    k += 1
                    if t.get("first") is not None:
                        per.setdefault((r["case"], i), []).append(t["first"])
        return per, (k, n)
    pb, cb = collect(base_res)
    pv, cv = collect(var_res)
    keys = sorted(set(pb) & set(pv))
    return [med(pb[k]) for k in keys], [med(pv[k]) for k in keys], cb, cv


# ---------------------------------------------------------------- the comparison
def compare(a, base_res, var_res, load_problems=(), out=None, roster=None):
    """Prints the report to OUT and returns the verdict as a dict (what --verdict writes). ROSTER (load_roster)
    is what a release is measured against."""
    out = out or sys.stdout
    say = lambda *x: print(*x, file=out)
    table = Table(a.markdown, out)
    rng = random.Random(a.seed)
    failures, warnings, incomplete, inconclusive, notes = [], [], list(load_problems), [], []
    B, V = by_case(base_res), by_case(var_res)
    skip = {t for t in a.skip_tags.split(",") if t}
    keep = lambda c, side: not skip & set(side[c]["tags"])
    require = [s for s in a.require.split(",") if s]
    not_run = {c for c in a.not_run.split(",") if c}
    for s in require:
        if s not in SETS:
            incomplete.append("--require names %r, which is not a test set (%s)" % (s, ", ".join(SETS)))
    invalid = [(r.get("_file", "?"), r.get("case"), record_problems(r)) for r in base_res + var_res if record_problems(r)]
    if invalid:
        incomplete.append("%d records are not valid results (never a pass): %s" % (
            len(invalid), "; ".join("%s %s: %s" % (f, c, ", ".join(w)) for f, c, w in invalid[:4]) + ("; ..." if len(invalid) > 4 else "")))

    # ---- which cases can be compared
    common = sorted(c for c in set(B) & set(V) if keep(c, B) and keep(c, V))
    excluded = {}
    for c in common:
        why = pair_problem(B[c], V[c], strict=a.release)
        if why:
            excluded.setdefault(why, []).append(c)
    gone = {c for cs in excluded.values() for c in cs}
    compared = [c for c in common if c not in gone]
    compared_set = set(compared)
    legacy = [c for c in compared if B[c]["legacy"] or V[c]["legacy"]]
    sets = {}
    for s in sorted({x["set"] for x in list(B.values()) + list(V.values())} | set(require)):
        bc = [c for c in B if B[c]["set"] == s and keep(c, B)]
        vc = [c for c in V if V[c]["set"] == s and keep(c, V)]
        sets[s] = {"baseline_cases": len(bc), "variant_cases": len(vc),
                   "compared": sum(1 for c in compared if V[c]["set"] == s),
                   "unscorable": [sum(X[c]["unscorable"] + X[c]["invalid"] for c in cs) for X, cs in ((B, bc), (V, vc))],
                   "excluded": {why: sum(1 for c in cs if V[c]["set"] == s) for why, cs in excluded.items()},
                   "variant_only": sum(1 for c in vc if c not in B), "baseline_only": sum(1 for c in bc if c not in V)}
        sets[s]["excluded"] = {k: n for k, n in sets[s]["excluded"].items() if n}

    say("## Evidence\n")
    for side, res in (("baseline", base_res), ("variant", var_res)):
        files = sorted({r["_file"] for r in res if "_file" in r})
        say("%s: %d runs in %s" % (side, len(res), ", ".join(files) or "-"))
    table(["set", "baseline cases", "variant cases", "compared", "left out", "variant only", "baseline only"],
          [[s, x["baseline_cases"], x["variant_cases"], x["compared"],
            ", ".join("%s %d" % kv for kv in sorted(x["excluded"].items())) or "-", x["variant_only"], x["baseline_only"]]
           for s, x in sets.items()])
    for why, cs in sorted(excluded.items()):
        say("left out, %s (%d): %s" % (why, len(cs), ", ".join(cs[:12]) + (", ..." if len(cs) > 12 else "")))
    if skip:
        say("left out: cases tagged %s" % ", ".join(sorted(skip)))
    if not_run:
        say("not run by the variant on purpose (--not-run, %d): %s" % (len(not_run), ", ".join(sorted(not_run)[:12])
                                                                       + (", ..." if len(not_run) > 12 else "")))
    for side, X in (("baseline", B), ("variant", V)):
        u = sorted(c for c in X if X[c]["unscorable"] or X[c]["invalid"])
        if u:
            say("%s: unscorable or invalid runs in %d cases (not counted as passes or as failures): %s" % (side, len(u), short(u)))
    if legacy:
        notes.append("no case hashes: comparison by name only for %d of %d cases (results from before the manifest "
                     "fields; a changed case or scorer cannot be seen)" % (len(legacy), len(compared)))
    services = [", ".join(sorted({searched(r) for r in res})) or "-" for res in (base_res, var_res)]
    if services[0] != services[1]:
        notes.append("search services: the baseline searched with %s, the variant with %s: the same requests, another "
                     "service's answers (a shape trial, --search-kind)" % tuple(services))
    for n in notes:
        say(n)
    say("")

    # ---- what the evidence must hold
    for s in require:
        if s in sets and not sets[s]["compared"]:
            x = sets[s]
            why = ("the baseline has no %s results" % s if not x["baseline_cases"] else
                   "the variant has no %s results" % s if not x["variant_cases"] else
                   "no %s case can be compared (%s)" % (s, ", ".join("%s %d" % kv for kv in sorted(x["excluded"].items()))
                                                        or "no name on both sides"))
            incomplete.append(why)
    hashed = [c for c in common if pair_problem(B[c], V[c], strict=a.release) in (None, "no scorable run")]
    for side, X in (("baseline", B), ("variant", V)):
        runs = sum(X[c]["n"] for c in hashed)
        bad = sum(X[c]["unscorable"] + X[c]["invalid"] for c in hashed)
        if runs and bad > a.max_unscorable * runs:
            incomplete.append("%d of the %s's %d runs of the cases on both sides are unscorable or invalid (more than %.0f%%)"
                              % (bad, side, runs, 100 * a.max_unscorable))
    if a.release:   # --require holds both sets (main), so a set missing on a side is reported above
        if skip:
            incomplete.append("a release judges every case: --skip-tags is not allowed")
        for side, cases in (("baseline", B), ("variant", V)):
            n = sum(x["legacy"] for x in cases.values())
            if n:
                incomplete.append("%d %s runs lack %s (results from before 2026-11-19's runners): run the %s again "
                                  "with the current runners" % (n, side, ", ".join(MANIFEST), side))
        if roster is None:
            incomplete.append("a release needs the roster of cases (load_roster), and it could not be read")
        else:
            crit_roster = roster_critical(roster)
            named = set().union(*[set(x) for x in roster["sets"].values()])
            for c in sorted(not_run & crit_roster):
                incomplete.append("critical case %s cannot be left out of a release (--not-run)" % c)
            if not_run - named:
                notes.append("--not-run names cases not in the roster: " + short(not_run - named))
            excused = not_run - crit_roster
            incomplete += roster_gaps(var_res, roster, "variant", 1.0, excused)
            for s, cases in sorted(roster["sets"].items()):
                expected = [n for n in cases if n not in excused]
                fresh = [n for n in expected if n in compared_set and V[n]["case_hash"] == {json.dumps(cases[n]["hash"])}]
                if len(fresh) < a.min_coverage * len(expected):
                    incomplete.append("%s: %d of the %d cases defined now compared (at least %.0f%% needed)"
                                      % (s, len(fresh), len(expected), 100 * a.min_coverage))
                lost = sorted(n for n in expected if n in crit_roster and n not in fresh)
                if lost:
                    incomplete.append("critical cases not compared on both sides as defined now: " + ", ".join(lost))
            extra = sorted(c for c in V if not any(c in x for x in roster["sets"].values()))
            if extra:
                notes.append("variant results of cases no longer defined: " + short(extra))
    if not compared:
        incomplete.append("no case can be compared" + (" (no name on both sides)" if not common else ""))

    # ---- pass rates
    lo = hi = mean = None
    if compared:
        say("## Pass rates (cases compared: %d)\n" % len(compared))
        diffs = [share(V[c]) - share(B[c]) for c in compared]
        lo, hi = interval(diffs, rng)
        mean = sum(diffs) / len(diffs)
        rows = []
        for side, X in (("baseline", B), ("variant", V)):
            rows.append([side, sum(X[c]["n"] for c in compared), sum(sum(X[c]["runs"]) for c in compared),
                         sum(X[c]["unscorable"] + X[c]["invalid"] for c in compared),
                         "%.1f%%" % (100 * sum(share(X[c]) for c in compared) / len(compared))])
        table(["side", "runs", "passed", "unscorable or invalid", "mean share of scorable runs passed per case"], rows)
        say("paired difference: %+.1f cases per 100 (95%% interval %+.1f to %+.1f); tolerable loss -%.1f\n"
            % (100 * mean, 100 * lo, 100 * hi, a.tolerance))
        if hi < 0:
            failures.append("overall: %+.1f per 100, interval wholly below 0" % (100 * mean))
        elif lo < -a.tolerance / 100.0:
            inconclusive.append("overall: the interval's lower end, %+.1f per 100, does not rule out a loss of %.1f "
                                "(repeat both sides the same number of times to narrow it)" % (100 * lo, a.tolerance))
        in_sets = {s: [c for c in compared if V[c]["set"] == s] for s in sets}
        if sum(1 for cs in in_sets.values() if cs) > 1:
            rows = []
            for s, cs in in_sets.items():
                if not cs:
                    continue
                d = [share(V[c]) - share(B[c]) for c in cs]
                slo, shi = interval(d, rng, 1000)
                if shi < 0:
                    failures.append("set %s: %+.1f per 100, interval %+.1f to %+.1f" % (s, 100 * sum(d) / len(d), 100 * slo, 100 * shi))
                rows.append([s, len(cs), "%.1f%%" % (100 * sum(share(B[c]) for c in cs) / len(cs)),
                             "%.1f%%" % (100 * sum(share(V[c]) for c in cs) / len(cs)), "%+.1f" % (100 * sum(d) / len(d)),
                             "%+.1f to %+.1f" % (100 * slo, 100 * shi)])
            say("By set:\n")
            table(["set", "cases", "baseline", "variant", "change", "95% interval"], rows)
        unbalanced = [c for c in compared if B[c]["n"] != V[c]["n"]]
        if unbalanced:
            show = lambda c: "%s %d/%d" % (c, B[c]["n"], V[c]["n"])
            inconclusive.append("repeats unbalanced: %d cases have a different number of runs a side (baseline/variant: %s%s)"
                                % (len(unbalanced), ", ".join(show(c) for c in unbalanced[:8]), ", ..." if len(unbalanced) > 8 else ""))

    if a.tags and compared:
        tags = {}
        for c in compared:
            for t in B[c]["tags"]:
                tags.setdefault(t, []).append(c)
        rows = []
        for t, cs in tags.items():
            if len(cs) < a.min_tag:
                continue
            d = [share(V[c]) - share(B[c]) for c in cs]
            tlo, thi = interval(d, rng, 1000)
            if thi < 0:
                failures.append("tag %s: %+.1f per 100, interval %+.1f to %+.1f" % (t, 100 * sum(d) / len(d), 100 * tlo, 100 * thi))
            rows.append((sum(d) / len(d), [t, len(cs), "%.1f%%" % (100 * sum(share(B[c]) for c in cs) / len(cs)),
                                           "%.1f%%" % (100 * sum(share(V[c]) for c in cs) / len(cs)),
                                           "%+.1f" % (100 * sum(d) / len(d)), "%+.1f to %+.1f" % (100 * tlo, 100 * thi)]))
        rows.sort(key=lambda x: x[0])
        say("## By tag (cases per 100, variant minus baseline; at least %d cases)\n" % a.min_tag)
        table(["tag", "cases", "baseline", "variant", "change", "95% interval"], [r for _, r in rows])

    # ---- cases
    show = lambda c: "%s (%d/%d -> %d/%d)" % (c, sum(B[c]["runs"]), len(B[c]["runs"]), sum(V[c]["runs"]), len(V[c]["runs"]))
    worse = [c for c in compared if share(V[c]) < share(B[c])]
    better = [c for c in compared if share(V[c]) > share(B[c])]
    to_repeat = []
    if compared:
        say("## Cases that changed\n")
        say("worse (%d): %s" % (len(worse), ", ".join(show(c) for c in sorted(worse, key=lambda c: share(V[c]) - share(B[c]))) or "-"))
        say("better (%d): %s\n" % (len(better), ", ".join(show(c) for c in better) or "-"))
    for c in worse:
        if len(B[c]["runs"]) >= MIN_RUNS and len(V[c]["runs"]) >= MIN_RUNS:
            if share(B[c]) - share(V[c]) >= CASE_DROP - 1e-9:
                failures.append("case %s: lost half or more of its passes" % show(c))
        else:
            to_repeat.append(c)

    crit = {c for c in V if critical(c, V[c]["tags"]) and keep(c, V)} | (roster_critical(roster) if roster else set())
    if crit:
        say("## Critical cases (must pass every run on the variant)\n")
        bad = []
        for c in sorted(crit):
            if c not in V:
                continue   # a release reports what it lacks above
            passed, n = sum(V[c]["runs"]), len(V[c]["runs"])
            base = "%d/%d" % (sum(B[c]["runs"]), len(B[c]["runs"])) if c in B else "-"
            if passed < n:
                bad.append(c)
                failures.append("critical case %s passed %d of %d scorable runs on the variant (baseline %s)" % (c, passed, n, base))
            for side, X in (("baseline", B), ("variant", V)):
                if c in X and (X[c]["unscorable"] or X[c]["invalid"]):
                    incomplete.append("critical case %s has %d unscorable or invalid runs on the %s"
                                      % (c, X[c]["unscorable"] + X[c]["invalid"], side))
        say("%d critical cases, %d of them run on the variant, %d passed every scorable run%s\n"
            % (len(crit), sum(1 for c in crit if c in V), sum(1 for c in crit if c in V) - len(bad),
               ("; not: " + ", ".join(bad)) if bad else ""))

    # ---- latency, calls, tokens
    if compared:
        say("## Latency, calls, tokens and upload per turn (median, 90th over completed turns; means for calls)\n")
        rb = per_request([r for r in base_res if r["case"] in compared_set])
        rv = per_request([r for r in var_res if r["case"] in compared_set])
        rows = []
        for k in ["all"] + sorted({x[0] for x in rb + rv}):
            for side, src in (("baseline", rb), ("variant", rv)):
                xs = [x for x in src if k == "all" or x[0] == k]
                if not xs:
                    continue
                col = lambda i: [x[i] for x in xs if x[i] is not None]
                calls = col(4)
                rows.append([k, side, len(xs), done([x[8] for x in xs]), fmt(med(col(1))), fmt(p90(col(1))), fmt(med(col(3))),
                             fmt(p90(col(3))), fmt(med(col(2))), fmt(p90(col(2))),
                             fmt(sum(calls) / len(calls), "", 2) if calls else "-", fmt(med(col(5)), "", 0),
                             fmt(med(col(6)), "", 0), fmt(med([b / 1000.0 for b in col(7)]))])
        table(["turns", "side", "n", "completed", "first words med", "p90", "first sentence med", "p90", "total med", "p90",
               "calls mean", "prompt tok med", "completion tok med", "kB sent med"], rows)
        fb, fv, cb, cv = matched_first_words(base_res, var_res, compared_set)
        rate = lambda c: c[0] / c[1] if c[1] else 0.0
        say("first words on the %d turns completed on both sides: baseline median %s s, 90th %s s; variant median %s s, 90th %s s"
            % (len(fb), fmt(med(fb)), fmt(p90(fb)), fmt(med(fv)), fmt(p90(fv))))
        say("turns completed: baseline %d/%d (%.1f%%), variant %d/%d (%.1f%%)%s\n"
            % (cb[0], cb[1], 100 * rate(cb), cv[0], cv[1], 100 * rate(cv),
               "" if rate(cb) == rate(cv) else ": completion differs, so the pooled figures above are of different turns"))
        if fb and fv:
            dm, dp = med(fv) - med(fb), p90(fv) - p90(fb)
            if dm > 0.3 or dp > 0.5:
                warnings.append("first words slower on the turns completed on both sides: median %+.1f s, 90th %+.1f s" % (dm, dp))
        if rate(cv) < rate(cb) - 0.01:
            warnings.append("the variant completed fewer turns: %.1f%% against %.1f%%" % (100 * rate(cv), 100 * rate(cb)))

    # ---- the gate
    verdict = "fails" if failures else "incomplete" if incomplete else "inconclusive" if inconclusive else "passes"
    say("## Gate\n")
    for f in failures:
        say("FAIL  " + f)
    for f in incomplete:
        say("INCOMPLETE  " + f)
    for f in inconclusive:
        say("INCONCLUSIVE  " + f)
    for w in warnings:
        say("WARN  " + w)
    if to_repeat:
        say("repeat on both sides to judge (fewer than %d runs a side): %s" % (MIN_RUNS, ", ".join(sorted(to_repeat))))
    say("gate: %s\n" % (verdict + (" (with warnings)" if warnings and verdict == "passes" else "")))
    return {"verdict": verdict, "failures": failures, "incomplete": incomplete, "inconclusive": inconclusive,
            "warnings": warnings, "notes": notes, "compared": len(compared), "sets": sets, "release": bool(a.release),
            "invalid": len(invalid),
            "interval": [lo, hi] if compared else None, "mean": mean, "tolerance": a.tolerance}


def parse(argv):
    ap = argparse.ArgumentParser(description="Compare a variant's results with a baseline's: the no-regression gate.",
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("--base")
    ap.add_argument("--variant")
    ap.add_argument("--tags", action="store_true")
    ap.add_argument("--min-tag", type=int, default=5)
    ap.add_argument("--markdown", action="store_true")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--skip-tags", default="")
    ap.add_argument("--tolerance", type=float, default=TOLERANCE, help="tolerable loss, cases per 100 (default %.1f)" % TOLERANCE)
    ap.add_argument("--require", default="", help="sets that must be compared: scored, suite")
    ap.add_argument("--release", action="store_true", help="both sets, manifest fields, coverage and critical cases required")
    ap.add_argument("--min-coverage", type=float, default=MIN_COVERAGE)
    ap.add_argument("--max-unscorable", type=float, default=MAX_UNSCORABLE,
                    help="share of a side's runs that may be unscorable (default %.2f)" % MAX_UNSCORABLE)
    ap.add_argument("--not-run", default="", help="cases the variant left out on purpose (tests/run.py --no-search); "
                                                   "never a critical case in a release")
    ap.add_argument("--verdict", default="", help="write the verdict as JSON to this file")
    ap.add_argument("--held-out", default="", help="judge a held-out run (results files, comma-separated) on its own: "
                                                   "pass-rate floors and critical cases; no baseline")
    ap.add_argument("--repeats", type=int, default=1, help="--held-out: runs each case must have (default 1)")
    ap.add_argument("--calibrate", action="store_true", help="--held-out: also suggest floors from the per-run pass rates")
    ap.add_argument("--floor", default="", help="--held-out floors, e.g. scored=0.8,suite=0.8 (default %s)"
                                                % ",".join("%s=%.2f" % kv for kv in sorted(HELD_OUT_FLOOR.items())))
    ap.add_argument("--self-test", action="store_true")
    return ap.parse_args(argv)


def main(argv=None):
    a = parse(sys.argv[1:] if argv is None else argv)
    if a.self_test:
        return self_test()
    if a.held_out:
        res, problems = load(a.held_out)
        floors = dict(HELD_OUT_FLOOR, **{k: float(v) for k, v in (x.split("=", 1) for x in a.floor.split(",") if x)})
        import agent_eval as ev
        try:
            roster = load_roster(ev.HELD_OUT_SEED)
            # A case whose answers move with the date (agent_eval.date_workload) is expected with the hash of the run's
            # own fixture date, so that a run judged after midnight is not stale.
            for d in sorted({r.get("web_date") for r in res if isinstance(r.get("web_date"), str)}):
                dated = load_roster(ev.HELD_OUT_SEED, d)
                for s_, cases in dated["sets"].items():
                    for n, x in cases.items():
                        roster["sets"][s_][n].setdefault("hash_by_date", {})[d] = x["hash"]
        except Exception as e:   # noqa: BLE001  no roster: incomplete, never a pass
            roster, problems = {"scorer": None, "seed": None, "sets": {}}, problems + ["the roster could not be read: %s" % e]
        v = held_out_verdict(res, problems, floors, roster, a.repeats, a.max_unscorable)
        if a.calibrate and a.repeats < 3:
            print("calibrate: refused: floors come from three runs or more (--repeats %d); none suggested" % a.repeats)
        elif a.calibrate:
            v["calibration"] = calibration(v)
            for g, c in sorted(v["calibration"].items()):
                print("calibrate %-6s mean %.1f%%, sd %.1f, runs %.1f%% to %.1f%%: floor %.0f%%"
                      % (g, 100 * c["mean"], 100 * c["sd"], 100 * c["min"], 100 * c["max"], 100 * c["floor"]))
        if a.verdict:
            with open(a.verdict, "w", encoding="utf-8") as f:
                json.dump(v, f, indent=1)
        return VERDICT_EXIT[v["verdict"]]
    if not a.base or not a.variant:
        sys.exit("--base and --variant are needed (or --self-test)")
    if a.release and not a.require:
        a.require = ",".join(SETS)
    base_res, p1 = load(a.base)
    var_res, p2 = load(a.variant)
    roster = None
    if a.release:
        try:
            roster = load_roster()
        except Exception as e:   # reported as incomplete evidence by compare()
            p2 = p2 + ["the roster of cases could not be read: %s" % e]
    v = compare(a, base_res, var_res, p1 + p2, roster=roster)
    if a.verdict:
        with open(a.verdict, "w", encoding="utf-8") as f:
            json.dump(v, f, indent=1)
    return VERDICT_EXIT[v["verdict"]]


def executed(r, x):
    """A record that shows a run took place (the held-out judge's evidence): its own run_id, the traced app, the
    requests the roster expects (the held-out question forms), and every one of them answered: a turn that ended
    with an answer that is not empty, and whose own trace holds the app's record of that request."""
    turns = r.get("turns")
    asked = [t for t in x.get("requests") or [] if t != "/new"]
    return (isinstance(r.get("run_id"), str) and len(r["run_id"]) >= 16 and r.get("trace") is True
            and r.get("requests") == x.get("requests") and isinstance(turns, list) and len(turns) == len(asked) and asked
            and all(isinstance(t, dict) and not t.get("unstarted") and not t.get("timed_out") and t.get("total") is not None
                    and isinstance(t.get("answer"), str) and t["answer"].strip() and traced_turn(t, k) for k, t in enumerate(turns)))


def traced_turn(t, k):
    """The turn's own trace: a list holding the app's record of request k+1 (ev "request"); a null or empty trace,
    or another request's, is no evidence that the turn ran."""
    trace = t.get("trace")
    return isinstance(trace, list) and any(isinstance(e, dict) and e.get("ev") == "request" and e.get("req") == k + 1 for e in trace)


def held_out_verdict(results, load_problems, floors, roster, repeats=1, max_unscorable=MAX_UNSCORABLE, out=None):
    """The held-out run judged on its own, against ROSTER (load_roster at the held-out seed): every case it defines
    with exactly REPEATS valid runs of its current version (its case_hash at that seed, the current scorer, the
    roster's seed); the pass rate of each set, and of the cases that rest on a web service ("web"), against its
    floor; every critical case of the roster passing every run. "fails" (a floor missed, a critical case not
    passing), "incomplete" (a case missing, run too often or not defined, a stale or other-seed record, records not
    valid, too many unscorable runs) or "passes"; printed, and returned with the figures. Pass rates per repeat
    (the k-th run of every case) are given too, for calibrating the floors (HELD_OUT_FLOOR)."""
    out = out or sys.stdout
    v = {"verdict": "passes", "sets": {}, "reasons": list(load_problems), "critical_failed": [], "per_repeat": {}}
    incomplete = bool(load_problems)
    defined = {n: (s_, x) for s_, cases in roster["sets"].items() for n, x in cases.items()}
    runs = {}
    for r in results:
        runs.setdefault(str(r.get("case")), []).append(r)
    unknown = sorted(n for n in runs if n not in defined)
    if unknown:
        v["reasons"].append("cases the roster does not define: %s" % short(unknown))
        incomplete = True
    good = {}
    stale, wrong_count = [], []
    for n, (s_, x) in defined.items():
        rs = runs.get(n, [])
        ok = [r for r in rs if not record_problems(r) and set_of(r) == s_ and r.get("case_hash") == x.get("hash_by_date", {}).get(r.get("web_date"), x["hash"])
              and r.get("scorer") == roster["scorer"] and r.get("web_seed") == roster["seed"] and searched(r) == DEFAULT_SEARCH
              and executed(r, x)]
        ids = [r.get("run_id") for r in ok]
        if len(set(ids)) != len(ids):   # three copies of one run are one run
            ok = [r for k, r in enumerate(ok) if r.get("run_id") not in ids[:k]]
        if len(ok) != len(rs): stale.append(n)
        if len(rs) != repeats: wrong_count.append(n)
        good[n] = ok
    wrong_count += [n for n, ok in good.items() if len(ok) != len(runs.get(n, [])) and n not in wrong_count and n not in stale]
    if stale:
        v["reasons"].append("%d cases have records that are not valid runs of the current case at seed %s (case, scorer, seed, "
                            "search service, run id, trace, requests, answered turns): %s" % (len(stale), roster["seed"], short(stale)))
        incomplete = True
    if wrong_count:
        v["reasons"].append("%d cases do not have exactly %d runs: %s" % (len(wrong_count), repeats, short(wrong_count)))
        incomplete = True
    groups = {s_: [n for n, (t, _) in defined.items() if t == s_] for s_ in SETS}
    groups["web"] = [n for n, (_, x) in defined.items() if x.get("web")]
    for g, names in groups.items():
        rs = [r for n in names for r in good[n]]
        scorable = [r for r in rs if outcome(r) in ("pass", "fail")]
        passed = sum(1 for r in scorable if outcome(r) == "pass")
        rate = passed / len(scorable) if scorable else None
        v["sets"][g] = {"cases": len(names), "runs": len(rs), "scorable": len(scorable), "passed": passed, "rate": rate,
                        "floor": floors.get(g)}
        if rs and len(rs) - len(scorable) > max_unscorable * len(rs):
            v["reasons"].append("%d of %d %s runs are unscorable" % (len(rs) - len(scorable), len(rs), g))
            incomplete = True
        if rate is None:
            v["reasons"].append("no scorable %s run" % g)
            incomplete = True
        elif floors.get(g) is not None and rate < floors[g]:
            v["reasons"].append("%s: %.1f%% passed, under the floor of %.0f%%" % (g, 100 * rate, 100 * floors[g]))
            v["verdict"] = "fails"
        v["per_repeat"][g] = [(lambda k_runs: sum(1 for r in k_runs if outcome(r) == "pass") / max(1, len(k_runs)))(
            [good[n][k] for n in names if len(good[n]) > k and outcome(good[n][k]) in ("pass", "fail")]) for k in range(repeats)]
    for n in sorted(roster_critical(roster)):
        if not good.get(n):
            continue   # counted as missing or stale above
        if any(outcome(r) != "pass" for r in good[n]):
            v["critical_failed"].append(n)
    if v["critical_failed"]:
        v["reasons"].append("critical cases not passing: %s" % short(v["critical_failed"]))
        v["verdict"] = "fails"
    if incomplete and v["verdict"] == "passes":
        v["verdict"] = "incomplete"
    for g, x in v["sets"].items():
        if x["rate"] is not None:
            print("held-out %-6s %d/%d passed (%.1f%%), floor %s, %d cases, seed %s%s" % (
                g, x["passed"], x["scorable"], 100 * x["rate"], "%.0f%%" % (100 * x["floor"]) if x["floor"] is not None else "none",
                x["cases"], roster["seed"], "; per run: " + ", ".join("%.1f%%" % (100 * p) for p in v["per_repeat"][g]) if repeats > 1 else ""),
                file=out)
    for reason in v["reasons"]:
        print("  " + reason, file=out)
    print("held-out: %s" % v["verdict"], file=out)
    return v


def calibration(v, margin_points=2.0, sd_factor=2.0, min_runs=3):
    """Suggested floors from a held-out run of several repeats (v: held_out_verdict's result): each group's mean
    pass rate per run, less the larger of MARGIN_POINTS and SD_FACTOR standard deviations, rounded down."""
    import statistics
    out = {}
    for g, rates in v["per_repeat"].items():
        if len(rates) < min_runs: continue   # no floor from fewer than three runs
        mean = statistics.mean(rates)
        sd = statistics.stdev(rates) if len(rates) > 1 else 0.0
        out[g] = {"mean": mean, "sd": sd, "min": min(rates), "max": max(rates),
                  "floor": math.floor(100 * (mean - max(margin_points / 100.0, sd_factor * sd))) / 100.0}
    return out


# ---------------------------------------------------------------- the gate's own test
def self_test():
    global load_roster
    failed, counted = [], [0]

    def check(what, ok, got=None):
        counted[0] += 1
        if not ok:
            failed.append(what + ("" if got is None else ": got %r" % (got,)))

    def rec(name, ok=True, suite=False, h=None, scorer="s1", trace=True, first=1.0, legacy=False, timed_out=False,
            unscorable=False, tags=None):
        t = {"answer": "" if timed_out else "fine", "error": "timeout" if timed_out else "", "first": None if timed_out else first,
             "total": None if timed_out else first + 1, "tools": [], "results": [], "trace": None}
        if timed_out:
            t["timed_out"] = True
        r = {"case": name, "requests": ["x"], "problems": [] if ok else ["wrong"], "turns": [t]}
        if unscorable:
            r["problems"] = ["unscorable: no state after turn 1"]
        if suite:
            r["tags"] = tags if tags is not None else [name.split("-")[0]]
        if not legacy:   # as the runners write them from 2026-11-19
            o = "unscorable" if unscorable else "pass" if ok else "fail"
            r.update(case_hash=h or "h-" + name, scorer=scorer, trace=trace, outcome=o, unscorable=o == "unscorable",
                     set="suite" if suite else "scored")
            if not trace:
                del t["trace"]
        return r

    def roster_of(records, scorer="s1"):
        out = {"scorer": scorer, "sets": {"scored": {}, "suite": {}}}
        for r in records:
            out["sets"][set_of(r)][r["case"]] = {"hash": r.get("case_hash") or "h-" + r["case"], "tags": r.get("tags", [])}
        return out

    def side(n_scored=40, n_suite=60, fail=(), runs=1, **kw):
        out = []
        for _ in range(runs):
            out += [rec("s%02d" % i, ok="s%02d" % i not in fail, **kw) for i in range(n_scored)]
            out += [rec("timer-%03d" % i, ok="timer-%03d" % i not in fail, suite=True, **kw) for i in range(n_suite)]
        return out

    FULL = side() + [rec("home_ask_first_no"), rec("home-045", suite=True, tags=["home", "confirm"])]
    ROSTER = roster_of(FULL)

    def gate(base, var, *args, roster=None):
        a = parse(list(args))
        if a.release and not a.require:
            a.require = ",".join(SETS)
        buf = io.StringIO()
        v = compare(a, base, var, out=buf, roster=(roster or roster_of(base + var)) if a.release else None)
        return v, buf.getvalue()

    # passes: the same results on both sides
    v, text = gate(side(), side(), "--release")
    check("identical sides pass", v["verdict"] == "passes", (v["verdict"], v["incomplete"], v["inconclusive"]))
    check("both sets compared", v["sets"]["scored"]["compared"] == 40 and v["sets"]["suite"]["compared"] == 60, v["sets"])

    # fails: an overall loss whose interval is wholly below 0
    lost = ["s%02d" % i for i in range(10)] + ["timer-%03d" % i for i in range(15)]
    v, _ = gate(side(), side(fail=lost))
    check("a clear loss fails", v["verdict"] == "fails" and any(f.startswith("overall") for f in v["failures"]), v)

    # inconclusive: a small set cannot rule out the tolerable loss
    v, text = gate(side(n_scored=20, n_suite=0), side(n_scored=20, n_suite=0, fail=["s03"]))
    check("a small set is inconclusive", v["verdict"] == "inconclusive" and "rule out" in v["inconclusive"][0], v)
    v, _ = gate(side(n_scored=20, n_suite=0), side(n_scored=20, n_suite=0, fail=["s03"]), "--tolerance", "20")
    check("a wider tolerance passes it", v["verdict"] == "passes", v)

    # inconclusive: repeats unbalanced between the sides
    base = side() + [rec("s01")] * 5
    v, _ = gate(base, side())
    check("unbalanced repeats are inconclusive", v["verdict"] == "inconclusive" and "unbalanced" in " ".join(v["inconclusive"]), v)
    v, _ = gate(base, side() + [rec("s01")] * 5)
    check("balanced repeats pass", v["verdict"] == "passes", v)

    # fails: a persistent loss on one case with many runs a side, and 4 of 6 as before
    for b_ok, v_ok, n, want in ((12, 6, 12, "fails"), (6, 3, 6, "fails"), (6, 2, 6, "fails"), (6, 4, 6, "passes"), (5, 0, 5, None)):
        base = side() + [rec("x01", ok=i < b_ok) for i in range(n)]
        var = side() + [rec("x01", ok=i < v_ok) for i in range(n)]
        v, text = gate(base, var, "--tolerance", "10")
        if want:
            check("case %d/%d -> %d/%d %s" % (b_ok, n, v_ok, n, want), v["verdict"] == want, (v["verdict"], v["failures"]))
        else:
            check("too few runs: listed to repeat", "repeat on both sides to judge" in text and
                  not any(f.startswith("case") for f in v["failures"]), v["failures"])

    # fails: a critical case that does not pass every run, whatever the overall figure says
    v, text = gate(side() + [rec("home_ask_first_no")], side() + [rec("home_ask_first_no", ok=False)])
    check("a critical scored case fails the gate", v["verdict"] == "fails" and "critical case home_ask_first_no" in " ".join(v["failures"]), v)
    v, _ = gate(side() + [rec("home-045", suite=True, tags=["home", "confirm"])],
                side() + [rec("home-045", suite=True, tags=["home", "confirm"], ok=False)])
    check("a critical suite case (by tag) fails the gate", v["verdict"] == "fails", v)
    v, _ = gate(side() + [rec("review_stale_yes")] * 4, side() + [rec("review_stale_yes")] * 3 + [rec("review_stale_yes", unscorable=True)])
    check("an unscorable run of a critical case: incomplete, not a pass and not a failure", v["verdict"] == "incomplete" and
          "critical case review_stale_yes has 1 unscorable" in " ".join(v["incomplete"]) and not v["failures"], v)
    check("critical names", critical("home_after_web_later", []) and critical("home_ask_first", []) and
          not critical("home_on", []) and not critical("home_ask_first_maybe", []))

    # fails outranks inconclusive and incomplete (the lean variant: a persistent loss with unbalanced repeats)
    base = side() + [rec("s01")] * 6 + [rec("s02")] * 3
    var = side() + [rec("s01", ok=False)] * 6
    v, _ = gate(base, var, "--release")
    check("a failure outranks unbalanced repeats", v["verdict"] == "fails" and v["inconclusive"], v)

    # cases left out, by reason
    var = side()
    for r in var:
        if r["case"] in ("s01", "s02"):
            r["case_hash"] = "changed"
        if r["case"] == "s03":
            r["scorer"] = "s2"
        if r["case"] == "timer-004":
            r["trace"] = False
    v, text = gate(side(), var)
    ex = v["sets"]["scored"]["excluded"]
    check("a changed case is left out", ex.get("changed case") == 2, v["sets"])
    check("a different scorer is left out", ex.get("different scorer") == 1, v["sets"])
    check("traced against untraced is left out", v["sets"]["suite"]["excluded"].get("traced against untraced") == 1, v["sets"])
    check("left-out cases are named", "left out, changed case (2): s01, s02" in text, text[:600])
    check("the rest is compared", v["compared"] == 96 and v["verdict"] == "passes", v)
    mixed = side() + [rec("s05", h="other")]
    v, _ = gate(mixed, side())
    check("two hashes on one side: a changed case", v["sets"]["scored"]["excluded"].get("changed case") == 1, v["sets"])

    # results without the manifest fields: by name, said; incomplete for a release
    v, text = gate(side(legacy=True), side(legacy=True))
    check("old results compare by name", v["verdict"] == "passes" and "no case hashes: comparison by name only" in text, (v, text[:400]))
    v, text = gate(side(legacy=True), side(), "--release", roster=roster_of(side()))
    check("a release with old results is incomplete", v["verdict"] == "incomplete" and "lack case_hash" in " ".join(v["incomplete"]), v)

    v, _ = gate(side(legacy=True), side(legacy=True, fail=lost), "--release", roster=roster_of(side()))
    check("a failure outranks incomplete evidence", v["verdict"] == "fails" and v["incomplete"], v["verdict"])

    # a release needs both sets, compared
    v, _ = gate(side(n_scored=0), side(), "--release", roster=roster_of(side()))
    check("a release without the baseline's scored set is incomplete", v["verdict"] == "incomplete" and
          "the baseline has no scored results" in v["incomplete"], v["incomplete"])
    v, _ = gate(side(), side(n_scored=0), "--release", roster=roster_of(side()))
    check("a release without the variant's scored set is incomplete", v["verdict"] == "incomplete", v["incomplete"])
    v, _ = gate(side(n_scored=0), side(), "--require", "scored")
    check("--require: a set the baseline lacks is incomplete", v["verdict"] == "incomplete", v)
    v, _ = gate(side(n_scored=0), side())
    check("without --require the suite alone is judged", v["verdict"] == "passes" and v["sets"]["scored"]["compared"] == 0, v)
    var = side()
    for r in var[:10]:
        r["case_hash"] = "changed"
    v, _ = gate(side(), var, "--release", roster=roster_of(side()))
    check("a release with too few scored cases compared is incomplete", v["verdict"] == "incomplete" and
          "scored: 30 of the 40 cases defined now compared" in " ".join(v["incomplete"]), v["incomplete"])
    v, _ = gate(side() + [rec("home_after_web_no")], side(), "--release")
    check("a release missing a critical case is incomplete", v["verdict"] == "incomplete" and
          "home_after_web_no" in " ".join(v["incomplete"]), v["incomplete"])
    v, text = gate(side() + [rec("home_after_web_no")], side(), "--release", "--not-run", "home_after_web_no")
    check("... and a critical case cannot be left out on purpose", v["verdict"] == "incomplete" and
          "critical case home_after_web_no cannot be left out" in " ".join(v["incomplete"]), v["incomplete"])
    v, text = gate(side() + [rec("x09")], side(), "--release", "--not-run", "x09")
    check("another case left out on purpose (no search) is said, not counted", v["verdict"] == "passes" and
          "not run by the variant on purpose (--not-run, 1): x09" in text, v)

    # N1: a release is measured against the roster of cases defined, not against what the variant sent
    partial = side(n_suite=0) + [rec("timer-000", suite=True), rec("home_ask_first_no")]
    v, _ = gate(FULL, partial, "--release", roster=ROSTER)
    check("a release of the scored set and one suite case is incomplete", v["verdict"] == "incomplete" and
          "the variant has a scorable run of the current case for 1 of the 61 suite cases" in " ".join(v["incomplete"])
          and "critical cases not compared on both sides as defined now: home-045" in " ".join(v["incomplete"]), v["incomplete"])
    v, _ = gate(FULL, [r for r in FULL if "confirm" not in r.get("tags", [])], "--release", "--skip-tags", "confirm", roster=ROSTER)
    check("a release with --skip-tags is incomplete", v["verdict"] == "incomplete" and
          "--skip-tags is not allowed" in " ".join(v["incomplete"]), v["incomplete"])
    v, _ = gate(FULL, FULL, "--release", roster=ROSTER)
    check("the full roster passes", v["verdict"] == "passes", (v["incomplete"], v["inconclusive"]))
    v, _ = gate(FULL, [dict(r, scorer="s2") for r in FULL], "--release", roster=ROSTER)
    check("a release scored by another scorer than the current one is incomplete", v["verdict"] == "incomplete" and
          "another scorer" in " ".join(v["incomplete"]), v["incomplete"])
    stale = [dict(r, case_hash="old") if r["case"] == "home-045" else r for r in FULL]
    v, _ = gate(stale, FULL, "--release", roster=ROSTER)
    check("a critical case changed since the baseline is incomplete", v["verdict"] == "incomplete" and
          "home-045" in " ".join(v["incomplete"]), v["incomplete"])
    check("roster gaps: an untraced baseline for a traced release", any("untraced" in g for g in
          roster_gaps([dict(r, trace=False) for r in FULL], ROSTER, "baseline", MIN_COVERAGE, trace=True)))
    check("roster gaps: a full traced baseline has none", roster_gaps(FULL, ROSTER, "baseline", MIN_COVERAGE, trace=True) == [])
    untagged = [dict(r, tags=["home"]) if r["case"] == "home-045" else r for r in FULL]
    failing = [dict(r, tags=["home"], outcome="fail", problems=["switched on without a yes"]) if r["case"] == "home-045" else r
               for r in FULL]
    v, _ = gate(untagged, failing, "--release", roster=ROSTER)
    check("a release judges the critical cases the roster names, whatever the results' tags say", v["verdict"] == "fails" and
          "critical case home-045" in " ".join(v["failures"]), v["failures"])
    real = load_roster()
    check("the roster holds both sets and the critical cases", len(real["sets"]["scored"]) > 100 and len(real["sets"]["suite"]) > 500
          and "home_ask_first_no" in roster_critical(real) and any(n.startswith("home-") for n in roster_critical(real)),
          sorted(roster_critical(real))[:5])

    # N2: a damaged record is no result, and unscorable evidence never reads as a pass or as a zero
    for what, r in (("outcome fail with no problems", dict(rec("s05"), outcome="fail")),
                    ("no list of problems", {k: v for k, v in rec("s05").items() if k != "problems"}),
                    ("outcome pass with problems", dict(rec("s05"), problems=["wrong"])),
                    ("unscorable True with outcome pass", dict(rec("s05"), unscorable=True)),
                    ("marked untraced, but its turns carry a trace", dict(rec("s05", trace=False),
                                                                          turns=[dict(rec("s05")["turns"][0], trace=[{"ev": "call"}])])),
                    ("marked traced, but no turn has a trace", dict(rec("s05"), turns=[{"answer": "a", "total": 1}])),
                    ("case_hash without outcome", {k: v for k, v in rec("s05").items() if k != "outcome"})):
        check("invalid record: " + what, record_problems(r) != [] and outcome(r) == "invalid", record_problems(r))
        v, _ = gate(side(), [x for x in side() if x["case"] != "s05"] + [r])
        check("an invalid record makes the gate incomplete: " + what, v["verdict"] == "incomplete" and v["invalid"] == 1, v["incomplete"])
    check("a valid record has no problems", record_problems(rec("s05")) == [] and record_problems(rec("s05", ok=False)) == []
          and record_problems(rec("s05", unscorable=True)) == [] and record_problems(rec("s05", legacy=True)) == [])
    many = ["s%02d" % i for i in range(10)]
    v, text = gate([rec(r["case"], unscorable=True) if r["case"] in many else r for r in side()],
                   [rec(r["case"], unscorable=True) if r["case"] in many else r for r in side()])
    check("the same cases unscorable on both sides: incomplete beyond 2%, not a pass", v["verdict"] == "incomplete" and
          "unscorable or invalid" in " ".join(v["incomplete"]) and v["sets"]["scored"]["excluded"].get("no scorable run") == 10,
          (v["incomplete"], v["sets"]))
    v, _ = gate([rec(r["case"], unscorable=True) if r["case"] == "s00" else r for r in side()],
                [rec(r["case"], unscorable=True) if r["case"] == "s00" else r for r in side()])
    check("one case unscorable on both sides: left out and said, under the threshold", v["verdict"] == "passes" and
          v["sets"]["scored"]["excluded"].get("no scorable run") == 1, (v["verdict"], v["incomplete"]))
    base = side() + [rec("x01")] * 6
    var = side() + [rec("x01")] * 5 + [rec("x01", unscorable=True)]
    v, text = gate(base, var, "--max-unscorable", "0.05")
    check("an unscorable run is not a zero", v["verdict"] == "passes" and "x01" not in text.split("worse (")[1].split("\n")[0], text[-900:])
    var = side()
    for r in var:
        if r["case"] == "s07":
            r["fixtures"] = "web-2"
    base = [dict(r, fixtures="web-1") if r["case"] == "s07" else r for r in side()]
    v, _ = gate(base, var)
    check("different fixtures are left out", v["sets"]["scored"]["excluded"].get("different fixtures") == 1, v["sets"])
    v, _ = gate(side(), var, "--release", roster=roster_of(side()))
    check("a release: fixtures recorded on one side only are left out", v["sets"]["scored"]["excluded"].get("different fixtures") == 1,
          v["sets"])
    # the search service (a shape trial, ZB-102): may differ between the sides, never within one, nor in a release
    brave = [dict(r, search="brave") for r in side()]
    v, text = gate(side(), brave)
    check("search: a shape trial is compared, and said", v["compared"] == 100 and v["verdict"] == "passes" and
          "baseline searched with tavily, the variant with brave" in text, (v["compared"], v["verdict"]))
    v, _ = gate([dict(r, search="tavily") for r in side()], side())
    check("search: a record without it searched with the default", v["compared"] == 100 and not v["notes"], v["notes"])
    v, _ = gate(side(), side() + [dict(rec("s07"), search="brave")])
    check("search: one case's runs of two services on a side are left out",
          v["sets"]["scored"]["excluded"].get("mixed search services") == 1, v["sets"])
    v, _ = gate(FULL, [dict(r, search="brave") for r in FULL], "--release", roster=ROSTER)
    check("search: a release is measured with the default", v["verdict"] == "incomplete" and
          v["sets"]["scored"]["excluded"].get("another search service") == 41, v["sets"])
    check("search: the roster takes only the default's runs", roster_gaps([dict(r, search="brave") for r in FULL], ROSTER, "baseline",
                                                                          MIN_COVERAGE, trace=True) != [])
    check("search: not text is not a valid record", record_problems(dict(rec("s01"), search=3)) == ["search is not text"])
    v, _ = gate([], side())
    check("nothing to compare is incomplete", v["verdict"] == "incomplete", v)

    # latency: losing the slowest turns to time-outs does not read as faster
    base = [rec("s%02d" % i, first=1.0 + (3.0 if i < 8 else 0)) for i in range(40)]
    var = [rec("s%02d" % i, first=1.0, timed_out=i < 8, ok=i >= 8) for i in range(40)]
    v, text = gate(base, var, "--tolerance", "50")
    check("completion differs is said", "completion differs" in text and "variant completed fewer turns" in " ".join(v["warnings"]),
          (v["warnings"], text[-700:]))
    check("first words compare the turns completed on both sides", "first words on the 32 turns completed on both sides" in text, text[-700:])
    var = [rec("s%02d" % i, first=1.5) for i in range(40)]
    v, _ = gate(base, var, "--tolerance", "50")
    check("slower first words on matched turns warn", any(w.startswith("first words slower") for w in v["warnings"]), v["warnings"])
    check("completed", completed(rec("x")["turns"][0]) and not completed(rec("x", timed_out=True)["turns"][0]) and
          not completed({"unstarted": True, "total": None, "answer": ""}))

    # files: unreadable or not results is incomplete; the verdict file; the exit status
    with tempfile.TemporaryDirectory() as d:
        good, bad, vfile = os.path.join(d, "a.json"), os.path.join(d, "b.json"), os.path.join(d, "v.json")
        with open(good, "w") as f:
            json.dump(side(), f)
        with open(bad, "w") as f:
            f.write("{\"not\": \"a list\"}")
        saved_roster = load_roster
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                rc = main(["--base", good + "," + os.path.join(d, "missing.json"), "--variant", good, "--verdict", vfile])
                v = json.load(open(vfile))
                rc2 = main(["--base", good, "--variant", bad])
                load_roster = lambda: roster_of(side())
                rc3 = main(["--base", good, "--variant", good, "--release", "--verdict", vfile])
                v3 = json.load(open(vfile))
                load_roster = lambda: ROSTER
                rc4 = main(["--base", good, "--variant", good, "--release", "--verdict", vfile])
        finally:
            load_roster = saved_roster
        check("--release measures against the roster, not the files", rc4 == 4, rc4)
        check("a missing file is incomplete evidence", rc == 4 and v["verdict"] == "incomplete" and
              "cannot be read" in " ".join(v["incomplete"]), (rc, v["incomplete"]))
        check("a file that is not results is incomplete", rc2 == 4)
        check("--release passes with both sets", rc3 == 0 and v3["release"] and v3["sets"]["scored"]["compared"] == 40, (rc3, v3))

    # The held-out run, judged on its own against the roster at the held-out seed (W3).
    def h_roster():
        sets = {"scored": {("s%02d" % k): {"hash": "h-s%02d" % k, "tags": [], "web": k < 5, "requests": ["x"]} for k in range(20)},
                "suite": {("t-%03d" % k): {"hash": "h-t-%03d" % k, "tags": ["confirm"] if k == 0 else ["timer"], "web": False, "requests": ["x"]}
                          for k in range(20)}}
        sets["scored"]["home_after_web_no"] = {"hash": "h-home_after_web_no", "tags": [], "web": True, "requests": ["x"]}
        return {"scorer": "s1", "seed": HELD_OUT_FROM + 1, "sets": sets}

    def held(roster, fail=(), seed=HELD_OUT_FROM + 1, drop=(), extra=(), repeats=1, stale=()):
        out = []
        for s_, cases in roster["sets"].items():
            for name in cases:
                if name in drop: continue
                for _ in range(repeats):
                    r = rec(name, ok=name not in fail, suite=s_ == "suite", tags=cases[name]["tags"] if s_ == "suite" else None,
                            h="stale" if name in stale else None)
                    r["web_seed"] = seed
                    r["run_id"] = "%032x" % random.getrandbits(128)
                    r["turns"][0]["trace"] = [{"ev": "call", "req": 1}, {"ev": "request", "req": 1}]
                    out.append(r)
        for name in extra:
            r = rec(name)
            r["web_seed"] = seed
            r["run_id"] = "%032x" % random.getrandbits(128)
            out.append(r)
        return out
    quiet, ro, fl = io.StringIO(), h_roster(), {"scored": 0.8, "suite": 0.8, "web": 0.8}
    v = held_out_verdict(held(ro, fail=("s06", "s07")), [], fl, ro, out=quiet)
    check("held-out: 90% of the roster against floors of 80% passes", v["verdict"] == "passes" and abs(v["sets"]["scored"]["rate"] - 19 / 21) < 1e-9, v)
    v = held_out_verdict(held(ro, fail=["s%02d" % k for k in range(6, 12)]), [], fl, ro, out=quiet)
    check("held-out: a set under its floor fails", v["verdict"] == "fails" and "under the floor" in str(v["reasons"]), v)
    v = held_out_verdict(held(ro, fail=["s00", "s01", "s02"]), [], fl, ro, out=quiet)
    check("held-out: the web cases under their floor fail, though the set passes", v["verdict"] == "fails" and
          "web:" in str(v["reasons"]) and v["sets"]["scored"]["rate"] >= 0.8, v["reasons"])
    v = held_out_verdict(held(ro, fail=("home_after_web_no",)), [], fl, ro, out=quiet)
    check("held-out: a critical case by name that failed fails it", v["verdict"] == "fails" and v["critical_failed"] == ["home_after_web_no"], v)
    v = held_out_verdict(held(ro, fail=("t-000",)), [], fl, ro, out=quiet)
    check("held-out: a critical case by the roster's tags that failed fails it", v["critical_failed"] == ["t-000"], v)
    v = held_out_verdict(held(ro, drop=("home_after_web_no", "t-000")), [], fl, ro, out=quiet)
    check("held-out: critical cases missing make it incomplete, never a pass", v["verdict"] == "incomplete", v)
    v = held_out_verdict(held(ro, seed=1), [], fl, ro, out=quiet)
    check("held-out: records of another seed are no held-out run", v["verdict"] == "incomplete", v)
    v = held_out_verdict(held(ro, extra=("invented",)), [], fl, ro, out=quiet)
    check("held-out: a case the roster does not define makes it incomplete", v["verdict"] == "incomplete" and "invented" in str(v["reasons"]), v)
    one = [r for r in held(ro) if r["case"] in ("s10", "t-005")]
    v = held_out_verdict(one, [], fl, ro, out=quiet)
    check("held-out: one easy pass from each set is incomplete, not a pass", v["verdict"] == "incomplete", v)
    v = held_out_verdict(held(ro, stale=("s03",)), [], fl, ro, out=quiet)
    check("held-out: a stale record is not counted", v["verdict"] == "incomplete" and "not valid runs" in str(v["reasons"]), v)
    v = held_out_verdict(held(ro, repeats=2), [], fl, ro, out=quiet)
    check("held-out: twice the runs asked for is incomplete", v["verdict"] == "incomplete", v)
    # Round 2, item 3: a record must show a run took place, and repeats must be distinct runs.
    def tamper(f):
        rs = held(ro)
        for r in rs:
            if r["case"] == "home_after_web_no": f(r)
        return held_out_verdict(rs, [], fl, ro, out=quiet)
    for what, f in (("no turns", lambda r: r.update(turns=[])), ("an unanswered turn", lambda r: r["turns"][0].update(total=None)),
                    ("a null trace (r3)", lambda r: r["turns"][0].update(trace=None)), ("an empty trace (r3)", lambda r: r["turns"][0].update(trace=[])),
                    ("another request's trace (r3)", lambda r: r["turns"][0].update(trace=[{"ev": "request", "req": 2}])),
                    ("an empty answer (r3)", lambda r: r["turns"][0].update(answer=" ")),
                    ("other requests", lambda r: r.update(requests=["y"])), ("untraced", lambda r: r.update(trace=False)),
                    ("no run id", lambda r: r.pop("run_id")), ("another search service", lambda r: r.update(search="brave"))):
        v = tamper(f)
        check("held-out (r2): a critical pass with %s is no evidence: incomplete" % what, v["verdict"] == "incomplete" and
              "home_after_web_no" in str(v["reasons"]), v["reasons"])
    rs = held(ro, repeats=3)
    copies = [r for r in rs if r["case"] == "s04"]
    for r in copies: r["run_id"] = copies[0]["run_id"]
    v = held_out_verdict(rs, [], fl, ro, repeats=3, out=quiet)
    check("held-out (r2): three copies of one run are not three runs", v["verdict"] == "incomplete" and "s04" in str(v["reasons"]), v["reasons"])
    # Round 3, item 9: a date-dependent case is expected with the hash of the run's own fixture date.
    rd = h_roster()
    rd["sets"]["scored"]["s05"]["hash_by_date"] = {"2026-11-21": "h-s05", "2026-11-22": "h-s05-sunday"}
    rs = held(rd)
    for r in rs: r["web_date"] = "2026-11-21"
    v = held_out_verdict(rs, [], fl, rd, out=quiet)
    check("held-out (r3): a run judged after midnight keeps its own date's hash", v["verdict"] == "passes", v["reasons"])
    for r in rs:
        if r["case"] == "s05": r["web_date"] = "2026-11-22"
    v = held_out_verdict(rs, [], fl, rd, out=quiet)
    check("held-out (r3): ... and a record whose hash is not its date's is stale", v["verdict"] == "incomplete" and "s05" in str(v["reasons"]))
    # Round 3, item 7: no floor from fewer than three runs.
    v = held_out_verdict(held(ro, repeats=2), [], fl, ro, repeats=2, out=quiet)
    check("held-out (r3): no floor suggested from two runs", calibration(v) == {}, calibration(v))
    v = held_out_verdict(held(ro, repeats=3, fail=("s08",)), [], fl, ro, repeats=3, out=quiet)
    cal = calibration(v)
    check("held-out: three repeats, per-run rates and a floor under their mean", v["verdict"] == "passes" and len(v["per_repeat"]["scored"]) == 3
          and cal["scored"]["floor"] <= cal["scored"]["mean"] - 0.02, (v["per_repeat"], cal))

    for f in failed:
        print("FAIL " + f)
    print("gate self-test: %d checks, %d failed" % (counted[0], len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
