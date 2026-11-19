#!/usr/bin/env python3
"""One entry point for Zero Bot's tests: choose groups by name, by profile, or by what changed, and get
one line per group and a short summary instead of each script's own output.

    tests/run.py --list                          the groups, the profiles and what each needs
    tests/run.py --profile quick                 every offline group but the slow ones (a few minutes)
    tests/run.py --profile change [--base REF]   the groups that cover the files changed since REF
    tests/run.py --profile offline               every offline group
    tests/run.py --profile release --baseline B.json [--no-search]
                                                 offline, the scored set and the suite once, the gate
    tests/run.py --groups unit,hue               these groups only
    tests/run.py --groups live-suite --tags timer --repeat 5 --no-search
    tests/run.py ... --plan                      say what would run and why; run nothing

Each group's whole output goes to build/test-runs/<date-time>/<group>.log, with plan.txt and
summary.txt beside it. The exit status is 0 only when every group passed (a group whose test file is not
in this tree is skipped, and does not count). With --baseline, the live runs are judged by the
no-regression gate (tests/exp_compare.py) rather than by every case passing.

Groups that need a build fail with a clear message when it is missing; --build builds what the chosen
groups need first (cmake --preset X the first time, then cmake --build --preset X -j2; the unit test
programs through tests/run_unit.sh). ZB_BUILD_WRAPPER prefixes every compile (also honoured by
tests/run_unit.sh); ZB_EVAL_WRAPPER prefixes every run against the live service (for example a lock
that also sets ZB_API_BASE and ZB_API_KEY). Live runs always use --jobs 1. Each group gets fresh
XDG_CONFIG_HOME, XDG_DATA_HOME and XDG_CACHE_HOME folders, removed afterwards.

    --self-test                                  check this runner (path map, profiles, --plan, summaries)

Standard library only.
"""
import argparse
import contextlib
import datetime
import io
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PY = sys.executable or "python3"
FAMILIES = ("tapo", "hue", "firetv", "homekit", "kasa", "wiz", "shelly")
MAX_LIVE_SECONDS = 6 * 3600


def py(*args):
    """A Python test script, run without writing bytecode into the tree."""
    return [PY, "-B"] + list(args)


def pyi(*args):
    """The same, isolated (-I) as the scripts that ask for it are run elsewhere."""
    return [PY, "-B", "-I"] + list(args)


# ---------------------------------------------------------------- the groups
# Run in this order. needs: "cli" (build/cli/zero_bot), "sim" (build/sim/zero_bot), "unit:NAME"
# (build/unit/NAME, built by tests/run_unit.sh), "live" (the AI service). secs: rough time once built.
# file: the group is skipped when this file is not in the tree. slow: left out of the quick profile.
# A command may be a function of the options and the run folder (the live groups).
def _eval_cmd(o, run):
    return eval_command(o, run)


def _suite_cmd(o, run):
    return suite_command(o, run)


def _gate_cmd(o, run):
    return gate_command(o, run)


GROUPS = [
    dict(name="runner", secs=2, needs=[], cmds=[py("tests/run.py", "--self-test")],
         about="this runner: the path map, the profiles, --plan, the summaries"),
    dict(name="unit", secs=90, needs=[], uses=["cli"], timeout=2400,
         cmds=[["sh", "tests/run_unit.sh"]], env={"ZB_UNIT_SKIP_SCRIPTS": "1"},
         about="tests/run_unit.sh: the C++ unit tests (the first build takes about 7 min), and the timer "
               "and radio tools through build/cli"),
    dict(name="cases", secs=2, needs=[],
         cmds=[pyi("tests/agent_suite.py", "--list"), pyi("tests/suite/generate.py", "--check")],
         about="the generated suite loads, and tests/suite/cases.json matches generate.py"),
    dict(name="scorer", secs=5, needs=[], file="tests/scorer_test.py",
         cmds=[pyi("tests/scorer_test.py")],
         about="the live sets' scoring, checked offline on saved answers"),
    dict(name="tools", secs=2, needs=["cli"], cmds=[pyi("tests/tools_cli.py", "build/cli/zero_bot")],
         about="the offline tools through the cli build: good, wrong and hostile arguments"),
    dict(name="loop", secs=35, needs=["cli"],
         cmds=[py("tests/home_chat_e2e.py"), py("tests/selfheal_chat_e2e.py"),
               pyi("tests/agent_loop_test.py", "-q", "build/cli/zero_bot")],
         about="the request loop against scripted and fake services: home_chat_e2e, selfheal_chat_e2e, "
               "agent_loop_test"),
    dict(name="tapo", secs=5, needs=["cli"], cmds=[py("tests/tapo_e2e.py")],
         about="Tapo end to end against two simulated plugs"),
    dict(name="wiz", secs=15, needs=["unit:wiz_cli", "cli"], cmds=[py("tests/wiz_e2e.py")],
         about="WiZ end to end against simulated lights and plugs"),
    dict(name="kasa", secs=15, needs=["unit:kasa_cli", "cli"], cmds=[py("tests/kasa_e2e.py")],
         about="Kasa end to end against simulated plugs, strip, dimmer, bulbs"),
    dict(name="shelly", secs=20, needs=["unit:shelly_cli", "cli"], cmds=[py("tests/shelly_e2e.py")],
         about="Shelly end to end against simulated Gen1 and Gen2+ devices"),
    dict(name="hue", secs=40, needs=["unit:hue_cli", "cli"], cmds=[py("tests/hue_e2e.py")],
         about="Hue end to end against a simulated bridge"),
    dict(name="homekit", secs=55, needs=["unit:homekit_cli"], cmds=[py("tests/homekit_e2e.py")],
         about="HomeKit end to end against a simulated bridge"),
    dict(name="screens", secs=15, needs=["sim"], cmds=[pyi("tests/shots_compare.py")],
         about="every screen rendered by the sim build and compared with tests/golden/shots/"),
    dict(name="firetv", secs=330, needs=["unit:firetv_cli"], slow=True, timeout=1500,
         cmds=[py("tests/firetv_e2e.py")],
         about="Fire TV end to end (waits out real wake-up times: about 5 min)"),
    dict(name="live-scored", secs=480, needs=["cli", "live"], live=True, timeout=MAX_LIVE_SECONDS,
         cmds=[_eval_cmd], about="tests/agent_eval.py, the scored set, against the live service (--jobs 1)"),
    dict(name="live-suite", secs=2100, needs=["cli", "live"], live=True, timeout=MAX_LIVE_SECONDS,
         cmds=[_suite_cmd],
         about="tests/agent_suite.py, the generated suite (601 cases), against the live service (--jobs 1)"),
    dict(name="gate", secs=5, needs=["baseline"], auto=True, cmds=[_gate_cmd],
         about="tests/exp_compare.py's no-regression gate: this run's live results against --baseline "
               "(added when --baseline is given with a live group)"),
]
BY_NAME = {g["name"]: g for g in GROUPS}
OFFLINE = [g["name"] for g in GROUPS if not g.get("live") and not g.get("auto")]
LIVE = [g["name"] for g in GROUPS if g.get("live")]

PROFILES = {
    "quick": ("every offline group but the slow ones; leaves out firetv (about 5 min) and the live sets",
              [n for n in OFFLINE if not BY_NAME[n].get("slow")]),
    "offline": ("every offline group, firetv included (about 12 min)", OFFLINE),
    "change": ("the groups that cover the files changed against --base (default main), the working tree "
               "included; for a change to what the model is told, which live suite tags to run five "
               "times (run only with --live)", None),
    "release": ("every offline group, then the scored set and the suite once, then the gate against "
                "--baseline", OFFLINE + LIVE),
}

# ---------------------------------------------------------------- what a changed file needs
# The first pattern that matches a path decides. "{family}" stands for the device family the pattern
# caught. A path that matches nothing gets the quick profile.
_F = "|".join(FAMILIES)
DRIVERS = list(FAMILIES)
QUICK = "<quick>"
PATH_RULES = [
    (r"^(docs/|licenses/|LICENSE$|[^/]*\.md$|.*/README[^/]*$|\.gitignore$)", [], "documentation"),
    (r"^(scripts/|packaging/|extras/|\.github/)", [], "packaging and deployment: no offline test covers it"),
    (r"^tests/run\.py$", ["runner"], "this runner"),
    (r"^tests/scorer_test\.py$", ["scorer"], "the scorer's own test"),
    (r"^tests/(agent_eval|agent_suite|exp_compare|exp_report)\.py$", ["scorer", "cases"],
     "the live sets' runners and scoring"),
    (r"^tests/suite/", ["cases"], "the generated suite"),
    (r"^tests/(golden/|shots/|shots_compare\.py$)", ["screens"], "the golden screenshots"),
    (r"^tests/mocks/mock_(?P<family>tapo)\.py$", ["{family}"], "a simulated device"),
    (r"^tests/mocks/(?P<family>%s)/" % _F, ["{family}"], "a simulated device"),
    (r"^tests/(?P<family>%s)_e2e\.py$" % _F, ["{family}"], "a device family's end-to-end test"),
    (r"^tests/(mocks/(mock_chat|fake_model)\.py|home_chat_e2e\.py|selfheal_chat_e2e\.py|agent_loop_test\.py)$",
     ["loop"], "the request loop's tests"),
    (r"^tests/tools_cli\.py$", ["tools"], "the offline tools' test"),
    (r"^tests/unit/(?P<family>%s)_(test|cli)\.cpp$" % _F, ["unit", "{family}"], "a driver's unit test"),
    (r"^tests/(unit/|snapshots/|run_unit\.sh$)", ["unit"], "the unit tests"),
    (r"^src/smarthome/drivers/(?P<family>%s)\.(h|cpp)$" % _F, ["unit", "{family}"], "one device driver"),
    (r"^src/smarthome/tapo\.(h|cpp)$", ["unit", "tapo"], "the Tapo transport"),
    (r"^src/smarthome/", ["unit"] + DRIVERS + ["loop"], "the shared smart-home layer"),
    (r"^src/net/", ["unit", "tools", "loop"] + DRIVERS, "the HTTP layer (service and devices)"),
    (r"^src/ui/", ["unit", "screens"], "the screens and keys"),
    (r"^src/platform/(display|input)", ["screens"], "the screen and keyboard (sim build)"),
    (r"^src/platform/", ["unit", "tools", "loop"], "audio, camera, helper programs"),
    (r"^src/(agent/|main\.cpp$)", ["unit", "loop", "tools"], "the request loop and the tools"),
    (r"^src/core/", ["unit", "tools", "loop"], "settings, stores, timers, radio, utilities"),
    (r"^(CMakeLists\.txt|CMakePresets\.json|cmake/|third_party/)", [QUICK], "the build itself"),
]

# Changes to what the model is told: which suite tags to run five times. "scan" reads the changed lines
# (and the function each change is in) for the tools they touch; "all" means a sample of every category.
MODEL_FACING = [
    (r"^src/agent/prompt\.cpp$", "all"),
    (r"^src/agent/(agent|llm)\.cpp$", "all"),
    (r"^src/agent/tools\.cpp$", "scan"),
    (r"^src/smarthome/(tool|control)\.cpp$", ["home", "tv"]),   # the schema; the sentences results carry
    (r"^src/core/timers\.cpp$", ["timer", "countdown"]),
    (r"^src/core/radio\.cpp$", ["music"]),
]
# Words in a changed line (or the function it is in) that point at a suite category.
TAG_WORDS = [
    ("timer", r"\btimers?\b|t_timer"), ("reminder", r"remind"), ("calendar", r"calendar"),
    ("weather", r"weather"), ("calc", r"\bcalc"), ("currency", r"currenc|exchange rate"),
    ("stock", r"\bstock"), ("route", r"\broute|directions"), ("search", r"web_search|search"),
    ("music", r"music|radio"), ("memory", r"memor|remember"), ("date", r"date_info"),
    ("world_time", r"world_time|time ?zone"), ("timezone", r"time ?zone"), ("breathe", r"breath"),
    ("look", r"\blook\b|camera|photo"), ("tv", r"\btv\b|infrared|\bir_"), ("home", r"smart_home"),
    ("device", r"battery|brightness|volume|wi-?fi"),
]


def match_path(path):
    """(groups, why) for one changed path: the first rule that matches, else the quick profile."""
    for pattern, groups, why in PATH_RULES:
        m = re.match(pattern, path)
        if m:
            fam = m.groupdict().get("family")
            out = []
            for g in groups:
                g = g.replace("{family}", fam or "")
                out += PROFILES["quick"][1] if g == QUICK else [g]
            return out, why
    return list(PROFILES["quick"][1]), "no rule for this path: the quick profile"


def groups_for_paths(paths):
    """The groups for a list of changed paths, in run order, and why each path asked for what."""
    wanted, reasons = set(), []
    for p in paths:
        groups, why = match_path(p)
        wanted.update(groups)
        reasons.append((p, why, groups))
    return [g["name"] for g in GROUPS if g["name"] in wanted], reasons


def model_facing_tags(paths, diff_text_of=None, categories=None):
    """Suite tags to run five times for the model-facing paths among PATHS: (tags or "all", [paths])."""
    tags, hit, everything = set(), [], False
    for p in paths:
        for pattern, rule in MODEL_FACING:
            if not re.match(pattern, p):
                continue
            hit.append(p)
            if rule == "all":
                everything = True
            elif rule == "scan":
                text = (diff_text_of(p) if diff_text_of else "").lower()
                found = [t for t, words in TAG_WORDS if re.search(words, text)]
                if found:
                    tags.update(found)
                else:
                    everything = True
            else:
                tags.update(rule)
            break
    if categories:
        tags = {t for t in tags if t in categories}
    return ("all" if everything else sorted(tags)), hit


def suite_categories():
    try:
        with open(os.path.join(HERE, "suite", "cases.json"), encoding="utf-8") as f:
            return {t for c in json.load(f) for t in c["tags"]}
    except (OSError, ValueError, KeyError):
        return None


# ---------------------------------------------------------------- git
def git(*args):
    r = subprocess.run(["git"] + list(args), cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("git %s failed: %s" % (" ".join(args), r.stderr.strip()))
    return r.stdout


def changed_paths(base):
    """Paths changed since the merge base with BASE, the working tree and untracked files included."""
    fork = git("merge-base", base, "HEAD").strip()
    paths = git("diff", "--name-only", fork).split()
    paths += git("ls-files", "--others", "--exclude-standard").split()
    return fork, sorted(set(paths))


def diff_reader(fork):
    def read(path):
        return "\n".join(l for l in git("diff", "-U0", fork, "--", path).splitlines()
                         if l.startswith(("+", "-", "@@")) and not l.startswith(("+++", "---")))
    return read


# ---------------------------------------------------------------- the live sets
def search_free_eval_cases(only):
    """The scored set's names that need no web search, as an --only that selects exactly them, or None
    when that cannot be done cleanly (names that contain one another)."""
    try:
        sys.path.insert(0, HERE)
        import agent_eval as ev
        cases = ev.CASES
    except Exception:  # the scored set could not be read: say the search cases are void instead
        return None
    finally:
        if sys.path and sys.path[0] == HERE:
            sys.path.pop(0)
    wanted = [w for w in only.split(",") if w]

    def picked(c, words):
        return not words or any(w in c["name"] for w in words)

    def needs_search(c):
        if "ZB_SEARCH_KEY" in c.get("env", {}) and c["env"]["ZB_SEARCH_KEY"] is None:
            return False
        said = " ".join(t for t in c["turns"] if isinstance(t, str))
        return "web_search" in c.get("must", []) or \
            bool(re.search(r"\blook (it )?up\b|\bsearch\b|\bgoogle\b", said, re.I))

    keep = [c["name"] for c in cases if picked(c, wanted) and not needs_search(c)]
    if not keep or [c["name"] for c in cases if picked(c, keep)] != keep:
        return None
    left_out = [c["name"] for c in cases if picked(c, wanted) and needs_search(c)]
    return keep, left_out


def live_prefix():
    w = os.environ.get("ZB_EVAL_WRAPPER", "").strip()
    return shlex.split(w) if w else []


def eval_command(o, run):
    cmd = live_prefix() + py("tests/agent_eval.py", "--jobs", "1", "--trace", "--json", os.path.join(run, "eval.json"))
    only = o.only
    if o.no_search:
        free = search_free_eval_cases(o.only)
        if free:
            only = ",".join(free[0])
    if only:
        cmd += ["--only", only]
    if o.repeat > 1:
        cmd += ["--repeat", str(o.repeat)]
    if o.fast is not None:
        cmd += ["--fast", o.fast]
    return cmd


def skip_tags(o):
    tags = [t for t in o.skip_tags.split(",") if t]
    if o.no_search and "search" not in tags:
        tags.append("search")
    return ",".join(tags)


def suite_command(o, run):
    cmd = live_prefix() + py("tests/agent_suite.py", "--jobs", "1", "--trace", "--json", os.path.join(run, "suite.json"))
    for flag, value in (("--tags", o.tags), ("--skip-tags", skip_tags(o)), ("--only", o.only)):
        if value:
            cmd += [flag, value]
    if o.sample:
        cmd += ["--sample", str(o.sample), "--seed", str(o.seed)]
    if o.repeat > 1:
        cmd += ["--repeat", str(o.repeat)]
    if o.fast is not None:
        cmd += ["--fast", o.fast]
    return cmd


def gate_command(o, run):
    variant = [os.path.join(run, n) for n in ("eval.json", "suite.json")
               if run == "<run>" or os.path.exists(os.path.join(run, n))]
    cmd = py("tests/exp_compare.py", "--base", o.baseline or "<baseline>", "--variant", ",".join(variant) or "<none>",
             "--tags")
    if skip_tags(o):
        cmd += ["--skip-tags", skip_tags(o)]
    return cmd


def live_notes(o, chosen):
    notes = []
    if o.no_search and "live-scored" in chosen:
        free = search_free_eval_cases(o.only)
        if free:
            notes.append("scored set without search: left out %s" % (", ".join(free[1]) or "nothing"))
        else:
            notes.append("scored set: its search cases could not be left out cleanly; count them as void")
    if o.no_search and ("live-suite" in chosen or "gate" in chosen):
        notes.append("suite and gate: --skip-tags search")
    exp = sorted(k for k in os.environ if k.startswith("ZB_EXP_"))
    if exp and any(n in chosen for n in LIVE):
        notes.append("experiment switches are set (%s): the live results are of that variant" % ", ".join(exp))
    return notes


# ---------------------------------------------------------------- what a group needs
def tool_path():
    tools = os.path.join(ROOT, ".tools", "bin")
    path = os.environ.get("PATH", "")
    return tools + os.pathsep + path if os.path.isdir(tools) else path


def need_path(need):
    if need in ("cli", "sim"):
        return "build/%s/zero_bot" % need
    if need.startswith("unit:"):
        return "build/unit/" + need[5:]
    return None


# What the driver test programs are built from (tests/run_unit.sh), besides every header in src/.
SIM_ONLY = ("src/ui/", "src/platform/display.", "src/platform/input.")    # not in the cli build
UNIT_SOURCES = ("src/smarthome/", "src/net/", "src/core/util.", "src/platform/proc.", "src/platform/sys.", "tests/unit/")


def stale_note(need):
    """Why a built program may be older than its sources, or ''. By modification times: a build relinks
    whenever a source it uses changed. (ninja -n cannot tell: the build's globs always look out of date.)"""
    if need in ("cli", "sim"):
        made = os.path.join(ROOT, need_path(need))
        tops, wanted = ("src", "CMakeLists.txt"), (lambda rel: need == "sim" or not rel.startswith(SIM_ONLY))
    elif need.startswith("unit:"):
        made = os.path.join(ROOT, "build", "unit", ".stamp")
        tops = ("src", os.path.join("tests", "unit"))
        wanted = (lambda rel: rel.endswith(".h") or rel.startswith(UNIT_SOURCES))
    else:
        return ""
    if not os.path.exists(made):
        return ""
    t = os.path.getmtime(made)
    for top in tops:
        walk = [(ROOT, [], [top])] if os.path.isfile(os.path.join(ROOT, top)) else os.walk(os.path.join(ROOT, top))
        for d, _, files in walk:
            for f in files:
                rel = os.path.relpath(os.path.join(d, f), ROOT).replace(os.sep, "/")
                if wanted(rel) and os.path.getmtime(os.path.join(d, f)) > t:
                    return "%s is older than %s" % (os.path.dirname(need_path(need)), rel)
    return ""


def missing(group, o, run):
    """What stops GROUP from running now, or ''."""
    for need in group["needs"]:
        if need == "live":
            if not live_prefix() and not (os.environ.get("ZB_API_BASE") and os.environ.get("ZB_API_KEY")):
                return "needs ZB_API_BASE and ZB_API_KEY, or ZB_EVAL_WRAPPER (a command that sets them)"
        elif need == "baseline":
            if not o.baseline:
                return "needs --baseline"
            if not any(os.path.exists(os.path.join(run, n)) for n in ("eval.json", "suite.json")):
                return "no live results to compare (the live runs wrote none)"
        else:
            p = need_path(need)
            if not os.path.exists(os.path.join(ROOT, p)):
                how = "the unit group or --build" if need.startswith("unit:") else \
                    "cmake --preset %s && cmake --build --preset %s, or --build" % (need, need)
                return "%s is missing (%s)" % (p, how)
    return ""


# ---------------------------------------------------------------- the plan
def choose(o):
    """(groups in run order, why each was chosen, notes, live recommendation or None)."""
    why, notes, advice = {}, [], None
    names = []
    if o.groups:
        for n in [n.strip() for n in o.groups.split(",") if n.strip()]:
            if n in PROFILES and n != "change":
                names += PROFILES[n][1]
                for m in PROFILES[n][1]:
                    why.setdefault(m, "profile " + n)
            elif n in BY_NAME:
                names.append(n)
                why.setdefault(n, "asked for")
            else:
                raise SystemExit("no group called %r (tests/run.py --list)" % n)
    profile = o.profile
    if profile == "change":
        fork, paths = changed_paths(o.base)
        groups, reasons = groups_for_paths(paths)
        notes.append("changed since %s (%s): %d files" % (o.base, fork[:9], len(paths)))
        notes += change_lines(reasons)
        for g in groups:
            why.setdefault(g, "changed files")
        names += groups
        tags, hit = model_facing_tags(paths, diff_reader(fork), suite_categories())
        if hit:
            advice = (tags, hit)
            if o.live and "live-suite" not in names:
                names.append("live-suite")
                why["live-suite"] = "--live: the model-facing change"
                if not (o.tags or o.sample or o.only):
                    if tags == "all" or not tags:
                        o.sample = 60
                    else:
                        o.tags = ",".join(tags)
                if o.repeat == 1:
                    o.repeat = 5
    elif profile:
        names += PROFILES[profile][1]
        for n in PROFILES[profile][1]:
            why.setdefault(n, "profile " + profile)
        if profile == "quick":
            notes.append("quick leaves out: " + ", ".join(n for n in OFFLINE if BY_NAME[n].get("slow"))
                         + " and the live sets")
    if not names:
        names = []
    if o.baseline and any(n in names for n in LIVE):
        names.append("gate")
        why["gate"] = "--baseline with a live run"
    chosen = [g["name"] for g in GROUPS if g["name"] in set(names)]
    return chosen, why, notes, advice


def change_lines(reasons):
    """The changed paths by the rule that placed them: '  why (n files: a, b, ...) -> groups'."""
    by = {}
    for p, why, gs in reasons:
        by.setdefault((why, tuple(gs)), []).append(p)
    out = []
    for (why, gs), ps in by.items():
        shown = ", ".join(ps[:3]) + (", ..." if len(ps) > 3 else "")
        out.append("  %s (%d: %s) -> %s" % (why, len(ps), shown, ", ".join(gs) or "nothing"))
    return out


def advice_lines(advice, o):
    if not advice:
        return []
    tags, hit = advice
    ns = " --no-search" if o.no_search else ""
    out = ["what the model is told changed (%s): run the suite five times on what it touches," % ", ".join(hit)]
    if tags == "all":
        out.append("  tests/run.py --groups live-suite --sample 60 --repeat 5%s   (a few from every category)" % ns)
    elif tags:
        out.append("  tests/run.py --groups live-suite --tags %s --repeat 5%s" % (",".join(tags), ns))
    out.append("  then the whole set once: tests/run.py --profile release --baseline <earlier suite.json>%s" % ns)
    return out


def command_text(cmd):
    words = []
    for w in cmd:
        w = os.path.relpath(w, ROOT) if os.path.isabs(w) and w.startswith(ROOT + os.sep) else w
        if w == PY:
            w = "python3"
        words.append(w if len(w) < 120 else w[:60] + "...(%d names)" % (w.count(",") + 1))
    return " ".join(shlex.quote(w) if not w.endswith("names)") else w for w in words)


def commands_of(group, o, run):
    return [c(o, run) if callable(c) else c for c in group["cmds"]]


def presets_needed(chosen):
    needs = set()
    for n in chosen:
        g = BY_NAME[n]
        needs.update(x for x in g["needs"] + g.get("uses", []) if x in ("cli", "sim"))
        if any(x.startswith("unit:") for x in g["needs"]) and "unit" not in chosen:
            needs.add("unit")
    return [p for p in ("cli", "sim", "unit") if p in needs]


def print_plan(chosen, why, notes, advice, o, out=None):
    out = out or sys.stdout
    for line in notes:
        print(line, file=out)
    if o.build:
        builds = presets_needed(chosen)
        print("build first: %s" % (", ".join(builds) or "nothing"), file=out)
    total = sum(BY_NAME[n]["secs"] for n in chosen)
    print("would run %d groups, about %s:" % (len(chosen), duration(total)), file=out)
    for n in chosen:
        g = BY_NAME[n]
        flags = []
        if g.get("file") and not os.path.exists(os.path.join(ROOT, g["file"])):
            flags.append("skipped: %s is not in this tree" % g["file"])
        print("  %-12s ~%-6s %s%s" % (n, duration(g["secs"]), why.get(n, ""),
                                     ("; " + "; ".join(flags)) if flags else ""), file=out)
        for c in commands_of(g, o, "<run>"):
            print("      " + command_text(c), file=out)
    for line in live_notes(o, chosen) + advice_lines(advice, o):
        print(line, file=out)


def duration(s):
    s = int(round(s))
    return "%d s" % s if s < 90 else "%d min" % round(s / 60.0) if s < 5400 else "%.1f h" % (s / 3600.0)


# ---------------------------------------------------------------- running
COUNT = re.compile(r"(\d+) (checks|passed), (\d+) failed")


def key_line(text):
    """The line of a group's output that counts what it checked."""
    lines = [l.strip() for l in text.splitlines() if l.strip()]
    # "N checks, M failed" counts every check; "N passed, M failed" only those that passed.
    counts = [(int(a) + (int(b) if kind == "passed" else 0), int(b)) for l in lines for a, kind, b in COUNT.findall(l)]
    if len(counts) > 1:
        return "%d checks, %d failed (%d parts)" % (sum(a for a, _ in counts), sum(b for _, b in counts), len(counts))
    for pattern in (r"^gate: .*", r"^\d+/\d+ passed[^|]*", r".*\d+ checks, \d+ failed.*", r".*\d+ screens, \d+ differ.*",
                    r".*(up to date|OUT OF DATE).*", r"^no cases selected$"):
        for l in reversed(lines):
            m = re.match(pattern, l)
            if m:
                return m.group(0).strip()[:100]
    return (lines[-1][:100] if lines else "(no output)")


def start(cmd, log, env, timeout):
    """Run one command with its output appended to LOG; its exit status (124 on time-out)."""
    log.write("\n$ %s\n" % command_text(cmd))
    log.flush()
    try:
        p = subprocess.Popen(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                             env=env, start_new_session=True)
    except OSError as e:
        log.write("could not start: %s\n" % e)
        return 127
    try:
        return p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        log.write("\n(timed out after %d s)\n" % timeout)
        stop(p)
        return 124
    except KeyboardInterrupt:
        stop(p)
        raise


def stop(p):
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(p.pid, sig)
        except OSError:
            return
        try:
            p.wait(timeout=5)
            return
        except subprocess.TimeoutExpired:
            pass


RUN_COMMAND = start   # the self-test puts a recorder here


def group_env(tmp, extra):
    env = dict(os.environ)
    for k in ("CONFIG", "DATA", "CACHE"):
        d = os.path.join(tmp, k.lower())
        os.makedirs(d, exist_ok=True)
        env["XDG_%s_HOME" % k] = d
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PATH"] = tool_path()
    env.update(extra or {})
    return env


def build(preset, run, o):
    """Build one preset (cli, sim) or the unit test programs; (ok, line)."""
    wrap = shlex.split(os.environ.get("ZB_BUILD_WRAPPER", ""))
    env = dict(os.environ, PATH=tool_path())
    t0 = time.time()
    with open(os.path.join(run, "build-%s.log" % preset), "w") as log:
        if preset == "unit":
            rc = RUN_COMMAND(["sh", "tests/run_unit.sh"], log, dict(env, ZB_UNIT_BUILD_ONLY="1"), 2400)
        else:
            rc = 0
            if not os.path.exists(os.path.join(ROOT, "build", preset, "CMakeCache.txt")):
                rc = RUN_COMMAND(["cmake", "--preset", preset], log, env, 600)
            if rc == 0:
                rc = RUN_COMMAND(wrap + ["cmake", "--build", "--preset", preset, "-j2"], log, env, 3600)
    return rc == 0, "%-5s %-11s %5d s  %s" % ("BUILT" if rc == 0 else "FAIL", preset, time.time() - t0,
                                             "" if rc == 0 else "log: " + os.path.relpath(log.name, ROOT))


def run_group(group, o, run, gate_follows, warnings):
    """(status, count line, seconds). status: pass, fail, ran (live, judged by the gate), skip."""
    name = group["name"]
    if group.get("file") and not os.path.exists(os.path.join(ROOT, group["file"])):
        return "skip", "%s is not in this tree" % group["file"], 0
    problem = missing(group, o, run)
    logpath = os.path.join(run, name + ".log")
    if problem:
        with open(logpath, "w") as log:
            log.write(problem + "\n")
        return "fail", problem, 0
    for need in group["needs"] + group.get("uses", []):
        note = stale_note(need) if need_path(need) and os.path.exists(os.path.join(ROOT, need_path(need))) else ""
        if note:
            warnings.setdefault(note, []).append(name)
    tmp = tempfile.mkdtemp(prefix="zb-run-%s-" % name)
    t0 = time.time()
    rc_all = 0
    try:
        env = group_env(tmp, group.get("env"))
        with open(logpath, "w") as log:
            log.write("# %s: %s\n" % (name, group["about"]))
            for cmd in commands_of(group, o, run):
                rc = RUN_COMMAND(cmd, log, env, group.get("timeout", 900))
                log.write("(exit %d)\n" % rc)
                rc_all = rc_all or rc
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    secs = time.time() - t0
    with open(logpath, errors="replace") as f:
        text = f.read()
    line = key_line(text)
    if rc_all == 0:
        return "pass", line, secs
    if group.get("live") and gate_follows and re.search(r"^\d+/\d+ passed", text, re.M):
        return "ran", line, secs
    if rc_all == 124:
        line = "timed out; " + line
    return "fail", line, secs


def new_run_folder():
    base = os.path.join(ROOT, "build", "test-runs")
    stamp = datetime.datetime.now().strftime("%Y-%m-%d-%H%M%S")
    path, n = os.path.join(base, stamp), 1
    while os.path.exists(path):
        n += 1
        path = os.path.join(base, "%s-%d" % (stamp, n))
    os.makedirs(path)
    return path


def execute(chosen, why, notes, advice, o):
    run = new_run_folder()
    rel = os.path.relpath(run, ROOT)
    with open(os.path.join(run, "plan.txt"), "w") as f:
        print_plan(chosen, why, notes, advice, o, out=f)
    total = sum(BY_NAME[n]["secs"] for n in chosen)
    print("%d groups, about %s; logs in %s/" % (len(chosen), duration(total), rel))
    for line in live_notes(o, chosen):
        print(line)
    rows, warnings, broken = [], {}, set()
    t_all = time.time()
    try:
        if o.build:
            for preset in presets_needed(chosen):
                ok, line = build(preset, run, o)
                print(line, flush=True)
                if not ok:
                    broken.add(preset)
        for n in chosen:
            g = BY_NAME[n]
            needs = {x.split(":")[0] for x in g["needs"]}
            if needs & broken:
                status, line, secs = "fail", "not run: the build it needs failed", 0
            else:
                status, line, secs = run_group(g, o, run, "gate" in chosen, warnings)
            rows.append((n, status, line, secs))
            log = os.path.join(rel, n + ".log")
            print("%-4s  %-12s %5d s  %s%s" % (status.upper(), n, secs, line,
                                               "   (%s)" % log if status == "fail" else ""), flush=True)
    except KeyboardInterrupt:
        print("interrupted")
        rows.append(("(interrupted)", "fail", "", 0))
    summary = summary_lines(rows, warnings, advice, o, rel, time.time() - t_all)
    with open(os.path.join(run, "summary.txt"), "w") as f:
        f.write("\n".join(summary) + "\n")
    print("\n".join(summary))
    return 0 if rows and all(s in ("pass", "skip", "ran") for _, s, _, _ in rows) else 1


def summary_lines(rows, warnings, advice, o, rel, secs):
    n = {s: sum(1 for r in rows if r[1] == s) for s in ("pass", "fail", "skip", "ran")}
    head = "%d passed, %d failed" % (n["pass"], n["fail"])
    if n["skip"]:
        head += ", %d skipped" % n["skip"]
    if n["ran"]:
        head += ", %d live runs judged by the gate" % n["ran"]
    out = ["", "summary: %s in %s" % (head, duration(secs))]
    for name, status, line, s in rows:
        out.append("  %-12s %-4s %5d s  %s" % (name, status, s, line))
    for note, names in warnings.items():
        out.append("warning: %s; ran %s with it (--build rebuilds)" % (note, ", ".join(names)))
    out += advice_lines(advice, o)
    out.append("run folder: %s" % rel)
    return out


# ---------------------------------------------------------------- listing
def print_list():
    print("groups (in run order):")
    for g in GROUPS:
        needs = ", ".join(g["needs"] + ["(%s if built)" % u for u in g.get("uses", [])]) or "-"
        print("  %-12s ~%-6s needs %s\n      %s" % (g["name"], duration(g["secs"]), needs, g["about"]))
    print("profiles:")
    for name, (about, groups) in PROFILES.items():
        print("  %-8s %s%s" % (name, about, (": " + ", ".join(groups)) if groups else ""))
    print("a change to these files asks for (first match wins):")
    for pattern, groups, why in PATH_RULES:
        print("  %-60s %s" % (pattern, ", ".join(groups) or "nothing"))


# ---------------------------------------------------------------- options
def parse(argv):
    ap = argparse.ArgumentParser(description="Run Zero Bot's tests by group, profile or change.",
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("--profile", choices=sorted(PROFILES), help="a set of groups (see --list)")
    ap.add_argument("--groups", default="", help="groups by name, comma-separated (profiles may be named too)")
    ap.add_argument("--base", default="main", help="for --profile change: compare with this ref (default main)")
    ap.add_argument("--list", action="store_true", help="show the groups, profiles and path rules")
    ap.add_argument("--plan", action="store_true", help="say what would run and why, then stop")
    ap.add_argument("--build", action="store_true", help="build what the chosen groups need first")
    ap.add_argument("--live", action="store_true", help="for --profile change: also run the live suite it advises")
    ap.add_argument("--baseline", default="", help="results file(s) to gate the live runs against (A.json[,B.json])")
    ap.add_argument("--no-search", action="store_true", help="leave out what needs web search (key over quota)")
    ap.add_argument("--tags", default="", help="live suite: cases with any of these tags (a+b means both)")
    ap.add_argument("--skip-tags", default="", help="live suite and gate: leave out cases with these tags")
    ap.add_argument("--sample", type=int, default=0, help="live suite: a stratified sample of this many cases")
    ap.add_argument("--seed", type=int, default=1, help="live suite: seed for --sample")
    ap.add_argument("--only", default="", help="live sets: cases whose name contains this (comma-separated)")
    ap.add_argument("--repeat", type=int, default=1, help="live sets: run each case this many times")
    ap.add_argument("--fast", default=None, help="live sets: force fast mode on (1) or off (0)")
    ap.add_argument("--self-test", action="store_true", help="check this runner and exit")
    o = ap.parse_args(argv)
    if o.baseline:
        o.baseline = ",".join(os.path.abspath(p) for p in o.baseline.split(",") if p)
    return o


def main(argv=None):
    o = parse(sys.argv[1:] if argv is None else argv)
    if o.self_test:
        return self_test()
    if o.list:
        print_list()
        return 0
    if not o.profile and not o.groups:
        raise SystemExit("choose --profile or --groups (tests/run.py --list shows them)")
    chosen, why, notes, advice = choose(o)
    if o.plan:
        print_plan(chosen, why, notes, advice, o)
        return 0
    if not chosen:
        for line in notes + advice_lines(advice, o):
            print(line)
        print("nothing to run")
        return 0
    return execute(chosen, why, notes, advice, o)


# ---------------------------------------------------------------- the runner's own test
def self_test():
    failures, counted = [], [0]

    def check(what, ok, got=None):
        counted[0] += 1
        if not ok:
            failures.append(what + ("" if got is None else ": got %r" % (got,)))

    def groups(*paths):
        return groups_for_paths(list(paths))[0]

    # The path map.
    check("hue driver", groups("src/smarthome/drivers/hue.cpp") == ["unit", "hue"], groups("src/smarthome/drivers/hue.cpp"))
    check("tapo transport", groups("src/smarthome/tapo.cpp") == ["unit", "tapo"])
    shared = groups("src/smarthome/control.cpp")
    check("shared smart-home layer", set(shared) == {"unit", "loop"} | set(FAMILIES), shared)
    check("smart-home tool", "firetv" in groups("src/smarthome/tool.cpp"))
    check("ui", groups("src/ui/app.cpp") == ["unit", "screens"], groups("src/ui/app.cpp"))
    check("agent", groups("src/agent/agent.cpp") == ["unit", "tools", "loop"], groups("src/agent/agent.cpp"))
    check("main.cpp", groups("src/main.cpp") == ["unit", "tools", "loop"])
    check("suite templates", groups("tests/suite/generate.py") == ["cases"])
    check("docs only", groups("docs/agent-architecture.html", "README.md", "AGENTS.md") == [],
          groups("docs/agent-architecture.html", "README.md", "AGENTS.md"))
    check("a mock", groups("tests/mocks/kasa/mock_kasa.py") == ["kasa"])
    check("the tapo mock", groups("tests/mocks/mock_tapo.py") == ["tapo"])
    check("a driver's e2e", groups("tests/wiz_e2e.py") == ["wiz"])
    check("a driver's unit test", groups("tests/unit/shelly_test.cpp") == ["unit", "shelly"])
    check("golden shots", groups("tests/golden/shots/01-chat.png") == ["screens"])
    check("this runner", groups("tests/run.py") == ["runner"])
    check("the scorer", groups("tests/agent_eval.py") == ["cases", "scorer"], groups("tests/agent_eval.py"))
    check("the build", groups("CMakeLists.txt") == PROFILES["quick"][1])
    check("an unknown path", groups("somewhere/new.txt") == PROFILES["quick"][1])
    check("several paths add up", groups("src/ui/app.cpp", "tests/suite/x.py") == ["unit", "cases", "screens"])
    for _, gs, _ in PATH_RULES:
        for g in gs:
            check("rule names group %s" % g, g in BY_NAME or g in ("{family}", QUICK))

    # Model-facing changes: advice, never a live run by itself.
    tags, hit = model_facing_tags(["src/agent/prompt.cpp", "README.md"])
    check("prompt.cpp asks for a sample of everything", tags == "all" and hit == ["src/agent/prompt.cpp"], (tags, hit))
    tags, _ = model_facing_tags(["src/agent/tools.cpp"], lambda p: "@@ static std::string t_timer(...)\n+  remind")
    check("tools.cpp: the tools its changed lines touch", tags == ["reminder", "timer"], tags)
    tags, _ = model_facing_tags(["src/smarthome/tool.cpp"], categories={"home", "timer"})
    check("tags only from the suite's own", tags == ["home"], tags)
    check("ui is not model-facing", model_facing_tags(["src/ui/app.cpp"])[1] == [])

    # The profiles.
    quick = PROFILES["quick"][1]
    check("quick is offline", not set(quick) & set(LIVE) and "gate" not in quick, quick)
    check("quick leaves out firetv", "firetv" not in quick and "unit" in quick and "runner" in quick, quick)
    check("offline has every offline group", set(PROFILES["offline"][1]) == set(OFFLINE) and "firetv" in OFFLINE)
    rel = PROFILES["release"][1]
    check("release: offline, then the live sets", rel[:len(OFFLINE)] == OFFLINE and rel[len(OFFLINE):] == LIVE, rel)
    o = parse(["--profile", "release", "--baseline", "b.json"])
    chosen = choose(o)[0]
    check("release with a baseline ends with the gate", chosen[-1] == "gate" and chosen[-3:-1] == LIVE, chosen)
    chosen = choose(parse(["--profile", "release"]))[0]
    check("no gate without a baseline", "gate" not in chosen)
    chosen = choose(parse(["--groups", "hue,unit,quick"]))[0]
    check("groups run in table order, once", chosen == [g for g in OFFLINE if g in set(quick)], chosen)
    check("every command's script is in the tree",
          all(os.path.exists(os.path.join(ROOT, w)) for g in GROUPS if not g.get("file")
              for c in g["cmds"] if not callable(c) for w in c if w.startswith("tests/")))

    # The live commands: --jobs 1 always, the pass-through options, --no-search.
    o = parse(["--groups", "live-suite", "--tags", "timer", "--repeat", "5", "--no-search", "--sample", "9"])
    cmd = suite_command(o, "/r")
    check("suite command", cmd[cmd.index("--jobs") + 1] == "1" and cmd[cmd.index("--tags") + 1] == "timer" and
          cmd[cmd.index("--skip-tags") + 1] == "search" and cmd[cmd.index("--repeat") + 1] == "5" and
          cmd[cmd.index("--sample") + 1] == "9", cmd)
    os.environ["ZB_EVAL_WRAPPER"], saved = "lockit --x", os.environ.get("ZB_EVAL_WRAPPER")
    try:
        check("ZB_EVAL_WRAPPER prefixes live runs", suite_command(o, "/r")[:2] == ["lockit", "--x"])
    finally:
        if saved is None:
            del os.environ["ZB_EVAL_WRAPPER"]
        else:
            os.environ["ZB_EVAL_WRAPPER"] = saved
    gate = gate_command(parse(["--baseline", "b.json", "--no-search", "--skip-tags", "camera"]), "<run>")
    check("gate skips search too", gate[gate.index("--skip-tags") + 1] == "camera,search", gate)
    free = search_free_eval_cases("")
    if free is not None:
        keep, left = free
        check("no-search leaves out the scored set's search cases", "search" in left and "search_news" in left and
              "search" not in keep, left)
        check("no-search keeps the case that runs without a key", "search_no_key" in keep)

    # The summaries.
    check("one count", key_line("ok a\nhue e2e: 193 checks, 0 failed\n") == "hue e2e: 193 checks, 0 failed")
    check("counts add up", key_line("x: 3 checks, 0 failed\ny: 2 passed, 1 failed\n") == "6 checks, 1 failed (2 parts)",
          key_line("x: 3 checks, 0 failed\ny: 2 passed, 1 failed\n"))
    check("screens", key_line("56 screens, 0 differ\n") == "56 screens, 0 differ")
    check("live", key_line("PASS x\n\n2/2 passed (100%) | per request: total median 3.1s\n") == "2/2 passed (100%)")
    check("gate", key_line("## Gate\n\nFAIL  timer -12\ngate: fails\n") == "gate: fails")
    check("cases", key_line("601 cases; tags: a 1\ncases.json is up to date (601 cases)\n")
          == "cases.json is up to date (601 cases)")

    # --plan runs nothing and writes nothing.
    calls = []
    global RUN_COMMAND
    saved_run, RUN_COMMAND = RUN_COMMAND, (lambda cmd, *a, **k: calls.append(cmd) or 0)
    runs = os.path.join(ROOT, "build", "test-runs")
    before = sorted(os.listdir(runs)) if os.path.isdir(runs) else []
    out = io.StringIO()
    try:
        with contextlib.redirect_stdout(out):
            rc = main(["--profile", "offline", "--plan", "--build", "--baseline", "b.json"])
            rc2 = main(["--groups", "live-suite,unit", "--plan", "--only", "timer-001"])
    finally:
        RUN_COMMAND = saved_run
    after = sorted(os.listdir(runs)) if os.path.isdir(runs) else []
    check("--plan runs nothing", rc == 0 and rc2 == 0 and not calls, calls)
    check("--plan writes no run folder", before == after)
    text = out.getvalue()
    check("--plan names every group", all(("  %s " % n) in text for n in OFFLINE), text[:300])
    check("--plan shows live commands", "--only timer-001" in text and "--jobs 1" in text, text[-400:])

    # A run, with every command recorded instead of started: lines, summary, exit status.
    calls = []

    def record(cmd, log, *args):
        calls.append(cmd)
        log.write("3 checks, 0 failed\n")
        return 0
    RUN_COMMAND = record
    folder = None
    try:
        with contextlib.redirect_stdout(io.StringIO()) as out:
            rc = main(["--groups", "runner,cases"])
        m = re.search(r"run folder: (\S+)", out.getvalue())
        folder = m and os.path.join(ROOT, m.group(1))
        check("a run starts each command", len(calls) == 3 and rc == 0, (calls, rc))
        check("a run writes logs and a summary", folder and all(os.path.exists(os.path.join(folder, f))
                                                                 for f in ("plan.txt", "summary.txt", "runner.log")))
        check("a run prints one line a group", re.search(r"^PASS  runner .*3 checks, 0 failed", out.getvalue(), re.M),
              out.getvalue())
    finally:
        RUN_COMMAND = saved_run
        if folder and os.path.isdir(folder):
            shutil.rmtree(folder)

    for f in failures:
        print("FAIL " + f)
    print("runner self-test: %d checks, %d failed" % (counted[0], len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
