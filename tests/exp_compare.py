#!/usr/bin/env python3
"""Compares a variant's results with a baseline's, case by case, for the experiments' no-regression gate
(ZB-033): results files written by tests/agent_eval.py or tests/agent_suite.py with --json (--trace for
the token and upload figures). Standard library only; reads files, calls nothing.

usage: tests/exp_compare.py --base A.json[,B.json...] --variant C.json[,D.json...] [--tags] [--markdown]
                            [--min-tag N] [--seed S] [--skip-tags a,b]

Every file on one side is pooled: a case run several times (two scored-set runs, --repeat) gets the share
of its runs that passed. Only cases on both sides are compared. Printed:

  - pass rates per side, and the paired difference over cases (variant minus baseline, in cases per 100)
    with a 95% bootstrap interval over cases;
  - with --tags, the same per tag (the first tag of a suite case is its category), for tags with at least
    --min-tag cases (default 5), sorted by the change;
  - the cases that changed: worse (lower share passed) and better, with each side's runs;
  - first words and whole-turn time by kind of turn, and calls, prompt tokens, completion tokens and bytes
    sent per request, for each side;
  - the gate (ZB-077, from the wave 1 critic): it fails on an overall interval wholly below 0, a tag
    (with --tags) whose interval is wholly below 0, or a case that lost 4 of 6 runs or more with at least
    6 runs a side. A tag's point estimate alone is not a failure: with 5 to 15 cases a tag and a few runs
    each, its interval spans 10 to 35 points and some tag drops by chance in every comparison. A case that
    got worse with fewer runs is listed to repeat. First words are flagged when the median is more than
    0.3 s or the 90th percentile more than 0.5 s slower (runs of the same code differed by about 0.2 s).
    Exit status 1 when the gate fails. --skip-tags leaves out cases with any of the tags (for example
    search while the search key is over quota).

Pool like with like. Repeats run only for the cases one side failed make that side look better (they
regress to the mean): the baseline's single suite run against itself plus its repeats of its failures
reads -2.1 per 100. Compare one run with one run, then repeat the cases that failed on either side the
same number of times on both sides, and compare those pooled.
"""
import argparse
import json
import os
import random
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from exp_report import Table, fmt, kind, med, p90, turn_trace, usage   # noqa: E402


def load(paths):
    out = []
    for p in paths.split(","):
        data = json.load(open(p, encoding="utf-8"))
        if not isinstance(data, list):
            sys.exit("%s: not a results file" % p)
        out.extend(data)
    return out


def by_case(results):
    cases = {}
    for r in results:
        c = cases.setdefault(r["case"], {"runs": [], "tags": r.get("tags", [])})
        c["runs"].append(not r["problems"])
    return cases


def share(c):
    return sum(c["runs"]) / len(c["runs"])


def interval(diffs, rng, n=2000):
    if not diffs:
        return None, None
    means = sorted(sum(rng.choice(diffs) for _ in diffs) / len(diffs) for _ in range(n))
    return means[int(n * 0.025)], means[int(n * 0.975)]


def per_request(results):
    """(kind, first words, total, first sentence, calls, prompt tokens, completion tokens, bytes sent) per turn."""
    rows = []
    for r in results:
        for t in r["turns"]:
            calls, req, _ = turn_trace(t)
            sent = req["first_sentence_ms"] / 1000.0 if req and req.get("first_sentence_ms", -1) >= 0 else None
            rows.append((kind(t, calls), t.get("first"), t.get("total"), sent, len(calls) if calls else None,
                         sum(usage(c, "prompt_tokens") or 0 for c in calls) if calls else None,
                         sum(usage(c, "completion_tokens") or 0 for c in calls) if calls else None,
                         sum(c.get("body_bytes", 0) for c in calls) if calls else None))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--variant", required=True)
    ap.add_argument("--tags", action="store_true")
    ap.add_argument("--min-tag", type=int, default=5)
    ap.add_argument("--markdown", action="store_true")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--skip-tags", default="")
    a = ap.parse_args()
    table = Table(a.markdown)
    rng = random.Random(a.seed)
    base_res, var_res = load(a.base), load(a.variant)
    B, V = by_case(base_res), by_case(var_res)
    skip = {t for t in a.skip_tags.split(",") if t}
    common = sorted(c for c in set(B) & set(V) if not skip & set(B[c]["tags"]))
    if not common:
        sys.exit("no case is on both sides")
    if skip:
        print("left out: cases tagged %s\n" % ", ".join(sorted(skip)))
    failures, warnings = [], []

    print("## Pass rates (cases on both sides: %d)\n" % len(common))
    diffs = [share(V[c]) - share(B[c]) for c in common]
    lo, hi = interval(diffs, rng)
    runs_b, runs_v = sum(len(B[c]["runs"]) for c in common), sum(len(V[c]["runs"]) for c in common)
    table(["side", "runs", "passed", "mean share passed per case"],
          [["baseline", runs_b, sum(sum(B[c]["runs"]) for c in common), "%.1f%%" % (100 * sum(share(B[c]) for c in common) / len(common))],
           ["variant", runs_v, sum(sum(V[c]["runs"]) for c in common), "%.1f%%" % (100 * sum(share(V[c]) for c in common) / len(common))]])
    print("paired difference: %+.1f cases per 100 (95%% interval %+.1f to %+.1f)\n"
          % (100 * sum(diffs) / len(diffs), 100 * lo, 100 * hi))
    if hi < 0:
        failures.append("overall: %+.1f per 100, interval wholly below 0" % (100 * sum(diffs) / len(diffs)))

    if a.tags:
        tags = {}
        for c in common:
            for t in B[c]["tags"]:
                tags.setdefault(t, []).append(c)
        rows = []
        for t, cs in tags.items():
            if len(cs) < a.min_tag:
                continue
            d = [share(V[c]) - share(B[c]) for c in cs]
            lo, hi = interval(d, rng, 1000)
            if hi < 0:
                failures.append("tag %s: %+.1f per 100, interval %+.1f to %+.1f" % (t, 100 * sum(d) / len(d), 100 * lo, 100 * hi))
            rows.append((sum(d) / len(d), [t, len(cs), "%.1f%%" % (100 * sum(share(B[c]) for c in cs) / len(cs)),
                                           "%.1f%%" % (100 * sum(share(V[c]) for c in cs) / len(cs)),
                                           "%+.1f" % (100 * sum(d) / len(d)), "%+.1f to %+.1f" % (100 * lo, 100 * hi)]))
        rows.sort(key=lambda x: x[0])
        print("## By tag (cases per 100, variant minus baseline; at least %d cases)\n" % a.min_tag)
        table(["tag", "cases", "baseline", "variant", "change", "95% interval"], [r for _, r in rows])

    worse = [c for c in common if share(V[c]) < share(B[c])]
    better = [c for c in common if share(V[c]) > share(B[c])]
    show = lambda c: "%s (%d/%d -> %d/%d)" % (c, sum(B[c]["runs"]), len(B[c]["runs"]), sum(V[c]["runs"]), len(V[c]["runs"]))
    print("## Cases that changed\n")
    print("worse (%d): %s" % (len(worse), ", ".join(show(c) for c in sorted(worse, key=lambda c: share(V[c]) - share(B[c]))) or "-"))
    print("better (%d): %s\n" % (len(better), ", ".join(show(c) for c in better) or "-"))
    to_repeat = []
    for c in worse:
        nb, nv = len(B[c]["runs"]), len(V[c]["runs"])
        if nb >= 6 and nv >= 6:
            if share(B[c]) - share(V[c]) >= 4 / 6 - 1e-9:
                failures.append("case %s" % show(c))
        else:
            to_repeat.append(c)

    print("## Latency, calls, tokens and upload per turn (median, 90th; means for calls)\n")
    rb, rv = per_request([r for r in base_res if r["case"] in set(common)]), per_request([r for r in var_res if r["case"] in set(common)])
    rows = []
    for k in ["all"] + sorted({x[0] for x in rb + rv}):
        for side, src in (("baseline", rb), ("variant", rv)):
            xs = [x for x in src if k == "all" or x[0] == k]
            if not xs:
                continue
            col = lambda i: [x[i] for x in xs if x[i] is not None]
            calls = col(4)
            rows.append([k, side, len(xs), fmt(med(col(1))), fmt(p90(col(1))), fmt(med(col(3))), fmt(p90(col(3))), fmt(med(col(2))),
                         fmt(p90(col(2))), fmt(sum(calls) / len(calls), "", 2) if calls else "-", fmt(med(col(5)), "", 0),
                         fmt(med(col(6)), "", 0), fmt(med([b / 1000.0 for b in col(7)]))])
    table(["turns", "side", "n", "first words med", "p90", "first sentence med", "p90", "total med", "p90", "calls mean",
           "prompt tok med", "completion tok med", "kB sent med"], rows)

    fb, fv = [x[1] for x in rb if x[1] is not None], [x[1] for x in rv if x[1] is not None]
    if fb and fv:
        dm, dp = med(fv) - med(fb), p90(fv) - p90(fb)
        if dm > 0.3 or dp > 0.5:
            warnings.append("first words slower: median %+.1f s, 90th %+.1f s" % (dm, dp))
    print("## Gate\n")
    for f in failures:
        print("FAIL  " + f)
    for w in warnings:
        print("WARN  " + w)
    if to_repeat:
        print("repeat on both sides to judge (fewer than 6 runs a side): " + ", ".join(sorted(to_repeat)))
    print("gate: %s\n" % ("fails" if failures else "passes" + (" (with warnings)" if warnings else "")))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
