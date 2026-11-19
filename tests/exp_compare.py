#!/usr/bin/env python3
"""Compares a variant's results with a baseline's, case by case, for the no-regression gate (ZB-033,
ZB-077): results files written by tests/agent_eval.py (the scored set) or tests/agent_suite.py (the
suite) with --json. Standard library only; reads files, calls nothing.

usage: tests/exp_compare.py --base A.json[,B.json...] --variant C.json[,D.json...] [--tags] [--markdown]
                            [--min-tag N] [--seed S] [--skip-tags a,b] [--tolerance 1.5]
                            [--require scored,suite] [--release] [--verdict OUT.json]
       tests/exp_compare.py --self-test

Every file on one side is pooled: a case run several times (two scored-set runs, --repeat) gets the share
of its runs that passed. A run passes when it has no problem and was scorable (a result marked
"unscorable", or whose problems all say so, never counts as a pass).

Which cases are compared. A result record carries its case's name and, from the runners of
2026-11-19 on, three manifest fields: `case_hash` (a hash of the compiled case: its turns, checks,
environment, home, radio, must and must_not), `scorer` (the scorer's version) and `trace` (whether the
run was traced: --trace adds stream_options.include_usage to what is sent, so traced and untraced runs
are different workloads). A case is compared only when its name is on both sides and those fields agree,
on each side and between the sides; the rest are left out and counted by reason: a changed case, a
different scorer, or traced against untraced. Results from before (no manifest fields) are compared by
name only, which the report says; the generated suite's names are positions (timer-001), so a case
inserted since the baseline can pair two different requests. tests/run.py always runs the live sets
with --trace, so a baseline for the gate must be traced too.

Printed: the evidence (files, sets, what was left out and why, repeats per side); pass rates per side
and per set, and the paired difference over cases (variant minus baseline, in cases per 100) with a 95%
bootstrap interval over cases; with --tags, the same per tag (the first tag of a suite case is its
category) for tags with at least --min-tag cases; the cases that changed; the critical cases; latency,
calls, tokens and bytes sent per turn, each with the completion rate beside it; then the gate.

The gate's verdict, and the exit status:
  passes (0)        none of the below
  fails (1)         the overall interval, a set's or (with --tags) a tag's is wholly below 0; a case with
                    at least 6 runs a side lost half or more of its share of passes (4 of 6, 6 of 12...);
                    or a critical case did not pass every one of its runs on the variant
  inconclusive (3)  no failure, but the overall interval's lower end is below the tolerable loss
                    (--tolerance, default 1.5 cases per 100), or the compared cases were not run the
                    same number of times on both sides
  incomplete (4)    the evidence cannot support a verdict: a file is unreadable; no case can be
                    compared; a set named by --require has no compared case; and with --release, a
                    set without results on either side, a record without the manifest fields, fewer
                    than --min-coverage (default 90%) of a set's variant cases compared, or a critical
                    case the baseline has and the variant lacks
A failure outranks the other verdicts, then incomplete, then inconclusive. --verdict writes the verdict,
its reasons and what was compared per set as JSON (tests/run.py reads it). --release is what
tests/run.py's release profile passes, with --require scored,suite.

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
import os
import random
import re
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from exp_report import Table, completed, done, fmt, kind, med, p90, turn_trace, usage   # noqa: E402

TOLERANCE = 1.5        # cases per 100: the loss the overall interval must rule out
MIN_RUNS = 6           # runs a side before one case can fail the gate by itself
CASE_DROP = 0.5        # ... by losing this share of its passes or more
MIN_COVERAGE = 0.9     # --release: share of a set's variant cases that must be compared
MANIFEST = ("case_hash", "scorer", "trace")
SETS = ("scored", "suite")
VERDICT_EXIT = {"passes": 0, "fails": 1, "inconclusive": 3, "incomplete": 4}

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


# ---------------------------------------------------------------- reading
def set_of(r):
    """The test set a result came from: the suite's records carry tags, the scored set's do not."""
    return r.get("set") or ("suite" if "tags" in r else "scored")


def outcome(r):
    """pass, fail or unscorable (evidence missing or unreadable: never a pass)."""
    problems = r.get("problems") or []
    if r.get("unscorable") or (problems and all(str(p).lower().startswith("unscorable") for p in problems)):
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
        if not isinstance(data, list) or not all(isinstance(r, dict) and "case" in r and "turns" in r for r in data):
            problems.append("%s is not a results file" % p)
            continue
        for r in data:
            r["_file"] = os.path.basename(p)
        out.extend(data)
    return out, problems


def by_case(results):
    cases = {}
    for r in results:
        c = cases.setdefault(r["case"], {"runs": [], "tags": r.get("tags", []), "set": set_of(r), "unscorable": 0,
                                          "legacy": 0, "case_hash": set(), "scorer": set(), "trace": set()})
        o = outcome(r)
        c["runs"].append(o == "pass")
        c["unscorable"] += o == "unscorable"
        if any(f not in r for f in MANIFEST):
            c["legacy"] += 1
        for f in MANIFEST:
            if f in r:
                c[f].add(json.dumps(r[f], sort_keys=True))
    return cases


def share(c):
    return sum(c["runs"]) / len(c["runs"])


EXCLUDED = (("case_hash", "changed case"), ("scorer", "different scorer"), ("trace", "traced against untraced"))


def pair_problem(b, v):
    """Why a case on both sides cannot be compared, or None. Only the fields both sides recorded count."""
    for field, why in EXCLUDED:
        if len(b[field]) > 1 or len(v[field]) > 1 or (b[field] and v[field] and b[field] != v[field]):
            return why
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
def compare(a, base_res, var_res, load_problems=(), out=None):
    """Prints the report to OUT and returns the verdict as a dict (what --verdict writes)."""
    out = out or sys.stdout
    say = lambda *x: print(*x, file=out)
    table = Table(a.markdown, out)
    rng = random.Random(a.seed)
    failures, warnings, incomplete, inconclusive, notes = [], [], list(load_problems), [], []
    B, V = by_case(base_res), by_case(var_res)
    skip = {t for t in a.skip_tags.split(",") if t}
    keep = lambda c, side: not skip & set(side[c]["tags"])
    require = [s for s in a.require.split(",") if s]
    for s in require:
        if s not in SETS:
            incomplete.append("--require names %r, which is not a test set (%s)" % (s, ", ".join(SETS)))

    # ---- which cases can be compared
    common = sorted(c for c in set(B) & set(V) if keep(c, B) and keep(c, V))
    excluded = {}
    for c in common:
        why = pair_problem(B[c], V[c])
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
    if legacy:
        notes.append("no case hashes: comparison by name only for %d of %d cases (results from before the manifest "
                     "fields; a changed case or scorer cannot be seen)" % (len(legacy), len(compared)))
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
    if a.release:   # --require holds both sets (main), so a set missing on a side is reported above
        for side, cases in (("baseline", B), ("variant", V)):
            n = sum(x["legacy"] for x in cases.values())
            if n:
                incomplete.append("%d %s runs lack %s (results from before 2026-11-19's runners): run the %s again "
                                  "with the current runners" % (n, side, ", ".join(MANIFEST), side))
        for s, x in sets.items():
            if x["baseline_cases"] and x["variant_cases"] and x["compared"] < a.min_coverage * x["variant_cases"]:
                incomplete.append("%s: only %d of the variant's %d cases compared (at least %.0f%% needed)"
                                  % (s, x["compared"], x["variant_cases"], 100 * a.min_coverage))
        lost = sorted(c for c in B if critical(c, B[c]["tags"]) and keep(c, B) and c not in V)
        if lost:
            incomplete.append("critical cases the variant did not run: " + ", ".join(lost))
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
            rows.append([side, sum(len(X[c]["runs"]) for c in compared), sum(sum(X[c]["runs"]) for c in compared),
                         sum(X[c]["unscorable"] for c in compared),
                         "%.1f%%" % (100 * sum(share(X[c]) for c in compared) / len(compared))])
        table(["side", "runs", "passed", "unscorable", "mean share passed per case"], rows)
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
        unbalanced = [c for c in compared if len(B[c]["runs"]) != len(V[c]["runs"])]
        if unbalanced:
            show = lambda c: "%s %d/%d" % (c, len(B[c]["runs"]), len(V[c]["runs"]))
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

    crit = sorted(c for c in V if critical(c, V[c]["tags"]) and keep(c, V))
    if crit:
        say("## Critical cases (must pass every run on the variant)\n")
        bad = []
        for c in crit:
            passed, n = sum(V[c]["runs"]), len(V[c]["runs"])
            base = "%d/%d" % (sum(B[c]["runs"]), len(B[c]["runs"])) if c in B else "-"
            if passed < n:
                bad.append(c)
                failures.append("critical case %s passed %d of %d runs on the variant (baseline %s)" % (c, passed, n, base))
        say("%d critical cases, %d passed every run%s\n" % (len(crit), len(crit) - len(bad),
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
    ap.add_argument("--verdict", default="", help="write the verdict as JSON to this file")
    ap.add_argument("--self-test", action="store_true")
    return ap.parse_args(argv)


def main(argv=None):
    a = parse(sys.argv[1:] if argv is None else argv)
    if a.self_test:
        return self_test()
    if not a.base or not a.variant:
        sys.exit("--base and --variant are needed (or --self-test)")
    if a.release and not a.require:
        a.require = ",".join(SETS)
    base_res, p1 = load(a.base)
    var_res, p2 = load(a.variant)
    v = compare(a, base_res, var_res, p1 + p2)
    if a.verdict:
        with open(a.verdict, "w", encoding="utf-8") as f:
            json.dump(v, f, indent=1)
    return VERDICT_EXIT[v["verdict"]]


# ---------------------------------------------------------------- the gate's own test
def self_test():
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
            r["problems"], r["unscorable"] = [], ["no state"]
        if suite:
            r["tags"] = tags if tags is not None else [name.split("-")[0]]
        if not legacy:
            r.update(case_hash=h or "h-" + name, scorer=scorer, trace=trace)
        return r

    def side(n_scored=40, n_suite=60, fail=(), runs=1, **kw):
        out = []
        for _ in range(runs):
            out += [rec("s%02d" % i, ok="s%02d" % i not in fail, **kw) for i in range(n_scored)]
            out += [rec("timer-%03d" % i, ok="timer-%03d" % i not in fail, suite=True, **kw) for i in range(n_suite)]
        return out

    def gate(base, var, *args):
        a = parse(list(args))
        if a.release and not a.require:
            a.require = ",".join(SETS)
        buf = io.StringIO()
        v = compare(a, base, var, out=buf)
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
    v, _ = gate(side(), side() + [rec("review_stale_yes")] * 3 + [rec("review_stale_yes", unscorable=True)])
    check("an unscorable run of a critical case is not a pass", v["verdict"] == "fails", v)
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
    v, text = gate(side(legacy=True), side(), "--release")
    check("a release with old results is incomplete", v["verdict"] == "incomplete" and "lack case_hash" in " ".join(v["incomplete"]), v)

    v, _ = gate(side(legacy=True), side(legacy=True, fail=lost), "--release")
    check("a failure outranks incomplete evidence", v["verdict"] == "fails" and v["incomplete"], v["verdict"])

    # a release needs both sets, compared
    v, _ = gate(side(n_scored=0), side(), "--release")
    check("a release without the baseline's scored set is incomplete", v["verdict"] == "incomplete" and
          "the baseline has no scored results" in v["incomplete"], v["incomplete"])
    v, _ = gate(side(), side(n_scored=0), "--release")
    check("a release without the variant's scored set is incomplete", v["verdict"] == "incomplete", v["incomplete"])
    v, _ = gate(side(n_scored=0), side(), "--require", "scored")
    check("--require: a set the baseline lacks is incomplete", v["verdict"] == "incomplete", v)
    v, _ = gate(side(n_scored=0), side())
    check("without --require the suite alone is judged", v["verdict"] == "passes" and v["sets"]["scored"]["compared"] == 0, v)
    var = side()
    for r in var[:10]:
        r["case_hash"] = "changed"
    v, _ = gate(side(), var, "--release")
    check("a release with too few scored cases compared is incomplete", v["verdict"] == "incomplete" and
          "only 30 of the variant's 40" in " ".join(v["incomplete"]), v["incomplete"])
    v, _ = gate(side() + [rec("home_after_web_no")], side(), "--release")
    check("a release missing a critical case is incomplete", v["verdict"] == "incomplete" and
          "home_after_web_no" in " ".join(v["incomplete"]), v["incomplete"])
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
        with contextlib.redirect_stdout(io.StringIO()):
            rc = main(["--base", good + "," + os.path.join(d, "missing.json"), "--variant", good, "--verdict", vfile])
            v = json.load(open(vfile))
            rc2 = main(["--base", good, "--variant", bad])
            rc3 = main(["--base", good, "--variant", good, "--release", "--verdict", vfile])
            v3 = json.load(open(vfile))
        check("a missing file is incomplete evidence", rc == 4 and v["verdict"] == "incomplete" and
              "cannot be read" in " ".join(v["incomplete"]), (rc, v["incomplete"]))
        check("a file that is not results is incomplete", rc2 == 4)
        check("--release passes with both sets", rc3 == 0 and v3["release"] and v3["sets"]["scored"]["compared"] == 40, (rc3, v3))

    for f in failed:
        print("FAIL " + f)
    print("gate self-test: %d checks, %d failed" % (counted[0], len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
