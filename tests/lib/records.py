"""The command-line build's account of a run, read as turns: its records file (ZB_EVAL_RECORDS, one JSON
object a line, written apart from the answer) and, for older builds and saved runs, its printed output. Shared by
the live sets' scorer (tests/agent/agent_eval.py) and the request loop's offline test (tests/sim/agent_loop_test.py).
Standard library only.
"""
import json
import re

TOOL_RE = re.compile(r"^\s+-> (\w+) (.*)$")
RESULT_RE = re.compile(r"^\s+<- (.*)$")
FIRST_RE = re.compile(r"\[first words \+([\d.]+)s\]")
ANSWER_RE = re.compile(r"^\[answer ([\d.]+)s\] ?(.*)", re.S | re.M)
TURN_RE = re.compile(r"^(?:> )*\[user\] ", re.M)
# Older runs printed the call and state records on stdout among the readable lines; the cli now writes
# them to a file of their own (ZB_EVAL_RECORDS; see turns_from_records), and stdout is only for reading.
CALL_RE = re.compile(r"^\[call\] (\{.*\})\s*$")
STATE_RE = re.compile(r"^\[state\] (\{.*\})\s*$")

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

def blank_turn(**kw):
    return dict({"tools": [], "results": [], "answer": "", "total": None, "first": None, "error": None}, **kw)

def _stop(turns, expected, stopped, failed, elapsed):
    """Marks where a run that stopped early stopped (see turns_from_output) and adds the turns it never started."""
    if stopped is not None:
        done = sum(t["total"] or 0 for t in turns if t is not stopped)
        stopped["error"] = stopped["error"] or failed or "no answer: the app stopped during this turn"
        if failed == "timeout": stopped["timed_out"] = True
        stopped["total"] = max(0.0, elapsed - done) if elapsed is not None and failed == "timeout" else None
    while len(turns) < expected:
        turns.append(blank_turn(error="not started: an earlier turn %s" % ("timed out" if failed == "timeout" else "did not finish"),
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
        stopped = blank_turn()
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
            cur = blank_turn(calls=[], began=r.get("mono"), began_wall=r.get("wall"))
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
        stopped = blank_turn()
        turns.append(stopped)
    return _stop(turns, expected, stopped, failed, process.get("seconds")), reasons
