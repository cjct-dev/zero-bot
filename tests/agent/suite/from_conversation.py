#!/usr/bin/env python3
"""Turns a saved conversation into a draft multi-turn case for the generated request set, printed
for a person to edit. Nothing is written, and nothing is added to the set automatically.

A conversation is saved on the device with SPACE twice on the chat screen (or "/save" in
zero_bot --chat), as a JSON file in ~/.local/share/zero-bot/conversations/ (AGENTS.md section 7
says how to pull one over ADB). This prints a case in the form tests/agent/suite/generate.py builds
(see its docstring): the user's turns as the input, with the time stamps, automatic notes and
spelt-out dates the app added taken off again; the tools the last turn called as "must"; the turns
answered at length as "long_ok"; and, as comments, what each turn called and how long its answer
was, with checks worth considering.

The words are the user's own. Before a case goes into tests/agent/suite/generate.py (the repository is
public), replace every name, place and event with others that test the same thing, and choose the
checks: a word cap (max_words) only for simple factual turns, never for analytical ones.

usage: tests/agent/suite/from_conversation.py FILE [--json] [--category NAME]
  --json        print the case as an entry of tests/agent/suite/cases.json instead of a call for generate.py
  --category    the category to file it under (default: dialogue)

Diagnostic and temporary, like saved conversations themselves (src/core/activity.h). Standard
library only.
"""
import argparse
import json
import re
import sys

STAMP_RE = re.compile(r"^\[(Mon|Tue|Wed|Thu|Fri|Sat|Sun) \d\d [A-Z][a-z]{2} \d{4}, \d\d:\d\d\] ")
NOTE_RE = re.compile(r"\n\n\((Automatic note, not from the user)[^\n]*\)\s*$", re.S)
SPELT_DATE_RE = re.compile(r" \((Mon|Tues|Wednes|Thurs|Fri|Satur|Sun)day,? [^)]*\d{4}\)")
LONG_CHARS = 420   # the scored set's usual cap on an answer (tests/agent/agent_eval.py score())
SHORT_WORDS = 40   # an answer this short may have been a simple fact


def said(content):
    """What the user said, from the message the model saw."""
    text = STAMP_RE.sub("", content or "", count=1)
    while True:
        cut = NOTE_RE.sub("", text)
        if cut == text: break
        text = cut
    return SPELT_DATE_RE.sub("", text).strip()


def words(text):
    return len(re.findall(r"[^\W_]+(?:['’.,:-][^\W_]+)*", text or ""))


def turns_of(messages):
    """The conversation as turns: what the user said, the tools called and the final reply."""
    turns = []
    for m in messages:
        role = m.get("role")
        if role == "user":
            turns.append({"said": said(m.get("content", "")), "time": m.get("time", ""), "tools": [], "answer": ""})
        elif not turns:
            continue   # the start of the conversation was not kept
        elif role == "assistant":
            calls = m.get("tool_calls") or []
            for c in calls:
                turns[-1]["tools"].append((c.get("tool", "?"), c.get("arguments")))
            if not calls and m.get("content"):
                turns[-1]["answer"] = m["content"]
    return turns


def draft(record, category, source):
    turns = turns_of(record.get("messages", []))
    if not turns:
        sys.exit("%s: no user message in this conversation" % source)
    last = turns[-1]
    must = []
    for name, _ in last["tools"]:
        if name not in must: must.append(name)
    used = {name for t in turns for name, _ in t["tools"]}
    tags = ["search"] if "web_search" in used else []
    long_ok = [i for i, t in enumerate(turns) if len(t["answer"]) > LONG_CHARS]
    hints = []
    for i, t in enumerate(turns):
        counts = {}
        for name, _ in t["tools"]: counts[name] = counts.get(name, 0) + 1
        for name, n in sorted(counts.items()):
            if n > 2:
                hint = 'max_calls("%s", 2)' % name
                hints.append((hint if i == len(turns) - 1 else "on_turn(%d, %s)" % (i, hint),
                              "turn %d called %s %d times" % (i + 1, name, n)))
        n = words(t["answer"])
        if t["answer"] and n <= SHORT_WORDS and i < len(turns) - 1:
            hints.append(("on_turn(%d, max_words(%d))" % (i, SHORT_WORDS), "turn %d was answered in %d words: only if it is a simple fact" % (i + 1, n)))
        if t["answer"].rstrip().endswith("?") and not t["tools"]:
            hints.append(('on_turn(%d, nans(r"\\?\\s*$", "ask instead of answering"))' % i if i < len(turns) - 1
                          else 'nans(r"\\?\\s*$", "ask instead of answering")',
                          "turn %d asked back without using a tool: was a question right?" % (i + 1)))
    return turns, {"category": category, "turns": [t["said"] for t in turns], "tags": tags, "must": must, "long_ok": long_ok,
                   "hints": hints}


def summary_lines(record, turns, source):
    out = ["Draft from %s (saved %s, %d turn%s). EDIT BEFORE USE: these are the user's own words. Put other names,"
           % (source, record.get("saved_at", "?"), len(turns), "" if len(turns) == 1 else "s"),
           "places and events in their place (the repository is public), choose the checks, and add it to",
           "tests/agent/suite/generate.py by hand."]
    if record.get("earlier_messages_not_kept"):
        out.append("The start of this conversation was not kept (%d earlier messages): the first turn may lean on it."
                   % record["earlier_messages_not_kept"])
    for i, t in enumerate(turns):
        counts = {}
        for name, _ in t["tools"]: counts[name] = counts.get(name, 0) + 1
        tools = ", ".join(name + (" x%d" % n if n > 1 else "") for name, n in counts.items()) or "no tool"
        answer = ("answer %d words%s" % (words(t["answer"]), ", ends with a question" if t["answer"].rstrip().endswith("?") else "")
                  if t["answer"] else "no answer before the next message (said while it was working?)" if i < len(turns) - 1
                  else "no answer yet (saved while it was working?)")
        out.append("  turn %d%s: %s; %s" % (i + 1, " (%s)" % t["time"][11:] if t["time"] else "", tools, answer))
    return out


def as_python(case, notes):
    lines = ["# " + n for n in notes]
    turns = case["turns"]
    first = 'case("%s", %s,' % (case["category"], json.dumps(turns[0] if len(turns) == 1 else turns, ensure_ascii=False))
    lines.append(first)
    lines.append("     tags=%s," % json.dumps(case["tags"]))
    lines.append("     must=%s,%s" % (json.dumps(case["must"]), "" if case["must"] else "   # the last turn called no tool: must_not=[\"*\"]?"))
    if case["long_ok"]:
        lines.append("     long_ok=%s,   # answered at length: keep only where a long answer was wanted" % case["long_ok"])
    lines.append("     checks=[")
    for check, why in case["hints"]:
        lines.append("         # %s,   # %s" % (check, why))
    lines.append("     ])")
    return "\n".join(lines)


def as_json(case):
    entry = {"name": "%s-draft" % case["category"], "tags": [case["category"]] + case["tags"], "turns": case["turns"], "must": case["must"],
             "must_not": [], "checks": [], "env": {}, "home": None, "steer": None}
    if len(case["turns"]) > 1 and "followup" not in entry["tags"]: entry["tags"].append("followup")
    if case["long_ok"]: entry["long_ok"] = case["long_ok"]
    return json.dumps(entry, indent=1, ensure_ascii=False)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("file", help="a saved conversation (JSON)")
    ap.add_argument("--json", action="store_true", help="print a cases.json entry instead of a call for generate.py")
    ap.add_argument("--category", default="dialogue")
    args = ap.parse_args()
    try:
        record = json.load(open(args.file, encoding="utf-8"))
    except (OSError, ValueError) as e:
        sys.exit("%s: %s" % (args.file, e))
    if not isinstance(record, dict) or record.get("format") != 1 or not isinstance(record.get("messages"), list):
        sys.exit("%s: not a saved conversation (format 1)" % args.file)
    source = args.file.rsplit("/", 1)[-1]
    turns, case = draft(record, args.category, source)
    notes = summary_lines(record, turns, source)
    if args.json:
        print("\n".join("// " + n for n in notes), file=sys.stderr)
        print(as_json(case))
    else:
        print(as_python(case, notes))


if __name__ == "__main__":
    main()
