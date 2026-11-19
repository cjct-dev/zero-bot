#!/usr/bin/env python3
"""Summarises results files written by tests/agent_eval.py or tests/agent_suite.py with --json (and,
for the model-call figures, --trace): pass rates overall and by tag, latency, model calls, tokens and
upload per request, the second look and the guards, tool errors, answer lengths, and the cases whose
result differs between files (flaky). Standard library only; reads files, calls nothing.

usage: tests/exp_report.py RUN.json [RUN.json ...] [--tags] [--cases N] [--markdown]

  --tags       pass rates per tag (suite files carry tags; a scored-set file has none)
  --cases N    list up to N failing cases per file with their problems (default 0)
  --markdown   tables for a report instead of plain text

Several files are pooled for the latency and token figures; pass rates are given per file, and the
cases that passed in one file and failed in another are listed as flaky.

Every latency figure has the completion rate beside it: a turn counts as completed when it started,
did not time out and ended with an answer and a time. A percentile is taken over the completed turns
only, so a variant that loses its slowest requests to time-outs reads faster than it is; read the
"completed" column with it (tests/exp_compare.py compares latency on the turns completed on both sides).
"""
import argparse
import json
import os
import re
import sys

MALFORMED = re.compile(r"^ERROR: (the arguments were not valid JSON|the arguments must be one JSON object|there is no tool called|"
                       r"\w+ must be (text|a number|a whole number|true or false)[^.]*(, not | is not one))")
UPLOAD_RATE = 10000.0   # bytes a second on a phone's Bluetooth tether (AGENTS.md section 8)


def pct(v, q):
    v = sorted(x for x in v if x is not None)
    if not v:
        return None
    return v[min(len(v) - 1, int(len(v) * q))]


def med(v):
    return pct(v, 0.5)


def p90(v):
    return pct(v, 0.9)


def fmt(x, unit="", nd=1):
    if x is None:
        return "-"
    return ("%." + str(nd) + "f%s") % (x, unit)


def load(path):
    data = json.load(open(path, encoding="utf-8"))
    if not isinstance(data, list):
        sys.exit("%s: not a results file" % path)
    return data


def turn_trace(t):
    tr = t.get("trace") or []
    calls = [e for e in tr if e.get("ev") == "call"]
    reqs = [e for e in tr if e.get("ev") == "request"]
    return calls, (reqs[0] if reqs else None), [e for e in tr if e.get("ev") == "tts"]


def completed(t):
    """The turn ran to an answer: started, not timed out, with a time and an answer (agent_eval.py's
    timing summary counts the same way; results saved before "unstarted" and "timed_out" have neither)."""
    return not t.get("unstarted") and not t.get("timed_out") and t.get("total") is not None and bool(t.get("answer"))


def done(turns):
    """'completed/all (rate)' for a list of turns."""
    n = len(turns)
    k = sum(1 for t in turns if completed(t))
    return "%d/%d (%.0f%%)" % (k, n, 100.0 * k / n) if n else "-"


def kind(t, calls):
    """What a turn was: no tool, a tool confirmed with one call, a tool then an answer, or more; or a
    turn never started (an earlier one timed out)."""
    if t.get("unstarted"):
        return "not started"
    if not t["tools"]:
        return "no tool"
    # The parallel check (E3, ZB_EXP_DECIDE=parallel) runs beside the deciding call: not a step of the answer.
    n = len([c for c in calls if c.get("role", "main") == "main" and c.get("purpose") != "parallel_check"]) if calls else None
    if n is None:
        return "tool (no trace)"
    return "tool, 1 call" if n == 1 else "tool, 2 calls" if n == 2 else "tool, 3+ calls"


def usage(c, key):
    u = c.get("usage") or {}
    if key == "cached":
        return (u.get("prompt_tokens_details") or {}).get("cached_tokens")
    return u.get(key)


class Table:
    def __init__(self, markdown, out=None):
        self.md = markdown
        self.out = out          # None: standard output

    def __call__(self, head, rows):
        say = lambda line: print(line, file=self.out or sys.stdout)
        if self.md:
            say("| " + " | ".join(head) + " |")
            say("|" + "---|" * len(head))
            for r in rows:
                say("| " + " | ".join(str(x) for x in r) + " |")
        else:
            w = [max(len(str(x)) for x in col) for col in zip(head, *rows)] if rows else [len(h) for h in head]
            say("  ".join(str(h).ljust(n) for h, n in zip(head, w)))
            for r in rows:
                say("  ".join(str(x).ljust(n) for x, n in zip(r, w)))
        say("")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--tags", action="store_true")
    ap.add_argument("--cases", type=int, default=0)
    ap.add_argument("--markdown", action="store_true")
    a = ap.parse_args()
    table = Table(a.markdown)
    runs = [(os.path.basename(f), load(f)) for f in a.files]

    # ---------------------------------------------------------------- pass rates
    print("## Pass rates\n")
    rows = []
    for name, res in runs:
        ok = sum(1 for r in res if not r["problems"] and not r.get("unscorable"))
        turns = [t for r in res for t in r["turns"]]
        rows.append([name, "%d/%d" % (ok, len(res)), "%.1f%%" % (100.0 * ok / max(1, len(res))), len(turns), done(turns)])
    table(["file", "passed", "rate", "turns", "completed"], rows)

    if a.tags:
        tags = {}
        for name, res in runs:
            for r in res:
                for t in r.get("tags", []):
                    ok, n = tags.setdefault(t, {}).get(name, (0, 0))
                    tags[t][name] = (ok + (not r["problems"] and not r.get("unscorable")), n + 1)
        if tags:
            print("## By tag (the first tag of a suite case is its category)\n")
            names = [n for n, _ in runs]
            rows = []
            for t in sorted(tags, key=lambda t: (-sum(n for _, n in tags[t].values()), t)):
                cells = []
                for n in names:
                    ok, k = tags[t].get(n, (0, 0))
                    cells.append("%d/%d (%.0f%%)" % (ok, k, 100.0 * ok / k) if k else "-")
                rows.append([t] + cells)
            table(["tag"] + names, rows)

    # ---------------------------------------------------------------- flaky cases
    outcome = {}
    for name, res in runs:
        for r in res:
            outcome.setdefault(r["case"], []).append(not r["problems"] and not r.get("unscorable"))
    flaky = sorted(c for c, v in outcome.items() if any(v) and not all(v))
    always = sorted(c for c, v in outcome.items() if not any(v))
    print("## Cases\n")
    print("failed every time (%d): %s" % (len(always), ", ".join(always) or "-"))
    print("passed and failed (%d): %s\n" % (len(flaky), ", ".join("%s %d/%d" % (c, sum(outcome[c]), len(outcome[c])) for c in flaky) or "-"))
    if a.cases:
        for name, res in runs:
            bad = [r for r in res if r["problems"]][:a.cases]
            if bad:
                print("failures in %s:" % name)
                for r in bad:
                    print("  %-22s %s" % (r["case"], "; ".join(r["problems"])[:240]))
                print()

    # ---------------------------------------------------------------- latency
    turns = [t for _, res in runs for r in res for t in r["turns"]]
    traced = [t for t in turns if t.get("trace")]
    print("## Latency (pooled over the files; seconds)\n")
    by_kind = {}
    for t in turns:
        calls, req, tts = turn_trace(t)
        by_kind.setdefault(kind(t, calls), []).append((t, calls, req, tts))
    rows = []
    for k in ["all"] + sorted(by_kind):
        items = [x for v in by_kind.values() for x in v] if k == "all" else by_kind[k]
        # Percentiles over the completed turns only; the "completed" column says how many those are.
        ran = [x for x in items if completed(x[0])]
        firsts = [x[0]["first"] for x in ran]
        totals = [x[0]["total"] for x in ran]
        sent = [x[2]["first_sentence_ms"] / 1000.0 for x in ran if x[2] and x[2].get("first_sentence_ms", -1) >= 0]
        tts = [(x[2]["first_sentence_ms"] + x[3][0]["tts_first_ms"]) / 1000.0 for x in ran
               if x[2] and x[3] and x[2].get("first_sentence_ms", -1) >= 0]
        rows.append([k, len(items), done([x[0] for x in items]), fmt(med(firsts)), fmt(p90(firsts)), fmt(med(sent)), fmt(p90(sent)),
                     fmt(med(totals)), fmt(p90(totals))]
                    + ([fmt(med(tts)), fmt(p90(tts))] if any(x[3] for x in items) else []))
    head = ["turns", "n", "completed", "first words med", "p90", "first sentence med", "p90", "total med", "p90"]
    if any(len(r) > 9 for r in rows):
        head += ["first spoken med", "p90"]
        rows = [r + ["-", "-"] if len(r) == 9 else r for r in rows]
    table(head, rows)

    if not traced:
        print("(no trace in these files: run with --trace for model calls, tokens and upload)")
        return

    # ---------------------------------------------------------------- model calls
    calls = [c for t in traced for c in turn_trace(t)[0]]
    print("## Model calls (%d traced turns, %d calls)\n" % (len(traced), len(calls)))
    rows = []

    def cold(c):   # the service reused less than half of the prompt from its cache
        pt, ca = usage(c, "prompt_tokens"), usage(c, "cached")
        return pt is not None and ca is not None and ca < pt / 2

    groups = [("all", calls)] + [(p, [c for c in calls if (c.get("purpose") or "-") == p])
                                 for p in sorted({c.get("purpose") or "-" for c in calls})]
    groups += [("warm prefix", [c for c in calls if not cold(c)]), ("cold prefix", [c for c in calls if cold(c)])]
    for p, cs in groups:
        if not cs:
            continue
        ttfb = [c["ttfb_ms"] / 1000.0 for c in cs if c.get("ttfb_ms", -1) >= 0]
        gen = [(c["done_ms"] - c["ttfb_ms"]) / 1000.0 for c in cs if c.get("ttfb_ms", -1) >= 0]
        done = [c["done_ms"] / 1000.0 for c in cs]
        pt = [usage(c, "prompt_tokens") for c in cs]
        ct = [usage(c, "completion_tokens") for c in cs]
        cached = [usage(c, "cached") for c in cs]
        share = sum(x or 0 for x in cached) / max(1, sum(x or 0 for x in pt))
        rows.append([p, len(cs), fmt(med(ttfb)), fmt(p90(ttfb)), fmt(med(gen)), fmt(med(done)), fmt(p90(done)),
                     fmt(med(pt), "", 0), fmt(med(ct), "", 0), "%.0f%%" % (100 * share),
                     fmt(med([c["body_bytes"] / 1000.0 for c in cs])), sum(1 for c in cs if c.get("attempts", 1) > 1),
                     sum(1 for c in cs if c.get("outcome") == "error")])
    table(["purpose", "calls", "ttfb med", "ttfb p90", "after first byte med", "call med", "call p90", "prompt tok med",
           "completion tok med", "cached", "kB sent med", "resent", "errors"], rows)
    asked = [c for c in calls if c.get("usage_asked")]
    with_usage = [c for c in calls if usage(c, "prompt_tokens") is not None]
    print("usage asked in %d calls, reported in %d; cached-token counts reported in %d\n"
          % (len(asked), len(with_usage), sum(1 for c in calls if usage(c, "cached") is not None)))

    # ---------------------------------------------------------------- per request
    print("## Per request\n")
    rows = []
    for k in ["all"] + sorted(by_kind):
        items = [x for v in by_kind.values() for x in v] if k == "all" else by_kind[k]
        items = [x for x in items if x[1]]
        if not items:
            continue
        n = [len(x[1]) for x in items]
        pt = [sum(usage(c, "prompt_tokens") or 0 for c in x[1]) for x in items]
        ct = [sum(usage(c, "completion_tokens") or 0 for c in x[1]) for x in items]
        cached = [sum(usage(c, "cached") or 0 for c in x[1]) for x in items]
        up = [sum(c.get("body_bytes", 0) for c in x[1]) for x in items]
        rows.append([k, len(items), done([x[0] for x in items]), fmt(med(n), "", 0), fmt(sum(n) / len(n), "", 2), fmt(med(pt), "", 0),
                     fmt(sum(pt) / len(pt), "", 0), "%.0f%%" % (100.0 * sum(cached) / max(1, sum(pt))), fmt(med(ct), "", 0),
                     fmt(med(up) / 1000.0), fmt(med(up) / UPLOAD_RATE), fmt(sum(up) / len(up) / UPLOAD_RATE)])
    table(["turns", "n", "completed", "calls med", "calls mean", "prompt tok med", "prompt tok mean", "cached", "completion tok med",
           "kB sent med", "upload s at 10 kB/s med", "mean"], rows)

    # ---------------------------------------------------------------- second look, guards, tools
    reqs = [turn_trace(t)[1] for t in traced]
    reqs = [q for q in reqs if q]
    looks = [q["second_look"] for q in reqs if q.get("second_look")]
    flips = [x for x in looks if x not in ("ok", "error")]
    print("## Second look and guards\n")
    print("second looks: %d of %d requests; let the draft through %d, turned it into a tool call %d (%.1f%%), failed %d"
          % (len(looks), len(reqs), looks.count("ok"), len(flips), 100.0 * len(flips) / max(1, len(looks)), looks.count("error")))
    if flips:
        names = {}
        for x in flips:
            for n in x.split(","):
                names[n] = names.get(n, 0) + 1
        print("tools the second look asked for: " + ", ".join("%s %d" % kv for kv in sorted(names.items(), key=lambda kv: -kv[1])))
    guards = {}
    for q in reqs:
        for g in q.get("guards", []):
            guards[g["guard"]] = guards.get(g["guard"], 0) + 1
    print("guards fired: " + (", ".join("%s %d" % kv for kv in sorted(guards.items())) or "none"))
    print("requests steered: %d" % sum(1 for q in reqs if q.get("steered")))
    checks = [q["parallel_check"] for q in reqs if q.get("parallel_check")]
    if checks:   # E3: the check beside the deciding call, and what the loop did with its answer
        count = lambda key: ", ".join("%s %d" % kv for kv in sorted(
            {v: sum(1 for c in checks if c.get(key) == v) for v in {c.get(key) for c in checks}}.items(), key=lambda kv: -kv[1]))
        held = [c for c in checks if c.get("used") != "main_tool"]
        print("parallel checks: %d; verdicts: %s; used: %s" % (len(checks), count("verdict"), count("used")))
        print("parallel check answered in: median %s s, 90th %s s; drafts cleared without the second look: %d of %d held"
              % (fmt(med([c["ms"] / 1000.0 for c in checks if c.get("ms", -1) >= 0])),
                 fmt(p90([c["ms"] / 1000.0 for c in checks if c.get("ms", -1) >= 0])),
                 sum(1 for c in held if c.get("used") in ("stream", "after")), len(held)))
    print()
    tools = [x for q in reqs for x in q.get("tools", [])]
    by_tool = {}
    for x in tools:
        by_tool.setdefault(x["name"], []).append(x)
    rows = [[n, len(v), sum(1 for x in v if x["outcome"] == "error"), sum(1 for x in v if x["outcome"] == "confirm"),
             fmt(med([x["ms"] / 1000.0 for x in v]), "", 2), fmt(med([x["result_bytes"] for x in v]), "", 0)]
            for n, v in sorted(by_tool.items(), key=lambda kv: -len(kv[1]))]
    table(["tool", "runs", "errors", "confirm", "time med", "result bytes med"], rows)
    # Tool results that say the call itself was malformed: broken JSON, an unknown tool, an argument of
    # the wrong type (tools::run's schema check; the transcript has the result's words).
    results = [r for t in turns for r in t.get("results", [])]
    bad = [r for r in results if MALFORMED.search(r)]
    again = [r for r in results if r.startswith("ERROR: this exact call already failed")]
    errors = [r for r in results if r.startswith("ERROR")]
    print("tool results: %d, errors %d, of which the call was malformed %d, a failed call repeated %d\n"
          % (len(results), len(errors), len(bad), len(again)))

    # ---------------------------------------------------------------- answers
    lens = [len(t["answer"]) for t in turns if t["answer"]]
    print("## Answers\n")
    print("answer length in characters: median %s, 90th %s, longest %s"
          % (fmt(med(lens), "", 0), fmt(p90(lens), "", 0), max(lens) if lens else "-"))


if __name__ == "__main__":
    main()
