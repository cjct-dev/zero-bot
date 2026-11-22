#!/usr/bin/env python3
"""One entry point for Zero Bot's tests: choose groups by name, by profile, or by what changed, and get
one line per group and a short summary instead of each script's own output.

    tests/run.py --list                          the groups, the profiles and what each needs
    tests/run.py --profile quick                 every offline group but the slow ones (a few minutes)
    tests/run.py --profile change [--base REF]   the groups that cover the files changed since REF
    tests/run.py --profile offline               every offline group
    tests/run.py --profile release --baseline EVAL.json,SUITE.json [--no-build]
                                                 build, offline, the scored set and the suite once, the gate
    tests/run.py --groups unit,hue               these groups only
    tests/run.py --groups live-suite --tags timer --repeat 5
    tests/run.py --groups live-scored --scored-only radio_ --repeat 5
    tests/run.py --groups live-suite --exp ZB_EXP_OFF=second_look --baseline B.json    a variant, gated
    tests/run.py ... --plan                      say what would run and why; run nothing

Each group's whole output goes to build/test-runs/<date-time>/<group>.log, with plan.txt and
summary.txt beside it. The exit status is 0 only when every group passed (a group whose test file is not
in this tree is skipped, and does not count). With --baseline, the live runs are judged by the
no-regression gate (tests/exp_compare.py) rather than by every case passing: the gate must compare every
set this run ran, and a live run with failed cases passes only when the gate's verdict (gate.json in the
run folder) is "passes" and covers its set. Live runs are always traced (--trace), so their baseline
must be traced too.

A release (--profile release, or --groups release: the same thing) runs every case of both sets as they
are defined: --tags, --sample, --skip-tags, --only, --scored-only, --fast and --exp are refused, and
--no-search is accepted and changes nothing: every live case's web reads go to the stand-in web
(tests/mocks/fake_web.py; the runners refuse a case that could reach a real web service), so no run spends
a search allowance and none needs a search key. It builds first unless
--no-build, and then a build older than any of its inputs (src, third_party, cmake, CMakeLists.txt,
CMakePresets.json) stops it. It also builds the device binary (cp0), which no group runs, and fails when that
binary's version is not the one in CMakeLists.txt or it holds a cli-only string or a path of the build machine. It starts only with a baseline measured against the roster of cases defined
in this tree (tests/exp_compare.py load_roster): valid records of both sets, a scorable, traced run of the
current version of at least 90% of each set's cases and of every critical case, by the current scorer. The
live sets run only on a cli build that is an experiments build whose hardware guard holds and which writes
the evaluator's records (probe_cli). The gate then runs with --release. Record a baseline with
tests/run.py --groups live-scored,live-suite (eval.json and suite.json in its run folder). Last, the release
runs the held-out group: both sets once on the stand-in web's held-out seed (never used day to day), judged on
its own by tests/exp_compare.py --held-out (pass-rate floors, critical cases), its verdict in heldout.json
beside gate.json. The web-contract group (tests/web_contract.py: the stand-in's shapes against the real
weather, place, rate and price services; never the search) is in no profile: run it by name before a release.

Groups never inherit switches that change what they test (ZB_UPDATE_SHOTS, ZB_UPDATE_SNAPSHOTS,
ZB_UNIT_BUILD_ONLY, ZB_UNIT_SKIP_SCRIPTS, HUE_E2E_ONLY, ZB_FAST_MODE, ZB_EVAL_TRACE, any ZB_EXP_*); a
variant's live runs get their experiment switches from --exp. A change to code the device build shares
adds the sim-build group, which compiles the sim build (no experiments, the screen code) incrementally.

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
sys.path.insert(0, HERE)
import netguard   # noqa: E402  the network boundary of every child (tests/netguard.py)
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


def _heldout_eval_cmd(o, run):
    return heldout_command(eval_command(o, run), os.path.join(run, "eval-heldout.json"), o)


def _heldout_suite_cmd(o, run):
    return heldout_command(suite_command(o, run), os.path.join(run, "suite-heldout.json"), o)


def _heldout_judge_cmd(o, run):
    files = ",".join(os.path.join(run, n) for n in ("eval-heldout.json", "suite-heldout.json"))
    n = max(1, getattr(o, "calibrate", 0))
    return py("tests/exp_compare.py", "--held-out", files, "--verdict", os.path.join(run, "heldout.json"), "--repeats", str(n)) + \
        (["--calibrate"] if getattr(o, "calibrate", 0) else [])


def _validate(script):
    """SCRIPT --validate (every case compiled offline), when SCRIPT has it; None (nothing to run) when not."""
    def cmd(o, run):
        try:
            with open(os.path.join(HERE, os.path.basename(script)), encoding="utf-8") as f:
                has = '"--validate"' in f.read()
        except OSError:
            has = False
        return pyi(script, "--validate") if has else None
    return cmd


def _sim_build_cmd(o, run):
    return shlex.split(os.environ.get("ZB_BUILD_WRAPPER", "")) + ["cmake", "--build", "--preset", "sim", "-j2"]


GROUPS = [
    dict(name="runner", secs=4, needs=[], cmds=[py("tests/run.py", "--self-test"), py("tests/exp_compare.py", "--self-test"),
                                              py("tests/web_contract.py", "--self-test"), py("tests/netguard.py", "--self-test")],
         about="this runner (the path map, the profiles, --plan, the summaries) and the gate's verdicts"),
    dict(name="unit", secs=60, needs=[], uses=["cli"], timeout=2400,
         cmds=[["sh", "tests/run_unit.sh"]], env={"ZB_UNIT_SKIP_SCRIPTS": "1"},
         about="tests/run_unit.sh: the C++ unit tests (the first build takes about 7 min), and the timer "
               "and radio tools through build/cli"),
    dict(name="cases", secs=3, needs=[],
         cmds=[pyi("tests/agent_suite.py", "--list"), pyi("tests/suite/generate.py", "--check"),
               _validate("tests/agent_suite.py"), _validate("tests/agent_eval.py")],
         about="the generated suite loads, tests/suite/cases.json matches generate.py, and every case of both "
               "live sets compiles (--validate)"),
    dict(name="scorer", secs=5, needs=[], file="tests/scorer_test.py",
         cmds=[pyi("tests/scorer_test.py")],
         about="the live sets' scoring, checked offline on saved answers"),
    dict(name="packaging", secs=2, needs=[], cmds=[pyi("tests/packaging_test.py")],
         about="scripts/package-deb.sh's package, built from a stand-in binary and read back, and its maintainer "
               "scripts (the wake screen's diversion) against a stand-in dpkg-divert"),
    dict(name="tools", secs=2, needs=["cli"], cmds=[pyi("tests/tools_cli.py", "build/cli/zero_bot")],
         about="the offline tools through the cli build: good, wrong and hostile arguments"),
    dict(name="loop", secs=30, needs=["cli"],
         cmds=[py("tests/home_chat_e2e.py"), py("tests/selfheal_chat_e2e.py"),
               pyi("tests/agent_loop_test.py", "-q", "build/cli/zero_bot")],
         about="the request loop against scripted and fake services: home_chat_e2e, selfheal_chat_e2e, "
               "agent_loop_test"),
    dict(name="tapo", secs=2, needs=["cli"], cmds=[py("tests/tapo_e2e.py")],
         about="Tapo end to end against two simulated plugs"),
    dict(name="wiz", secs=12, needs=["unit:wiz_cli", "cli"], cmds=[py("tests/wiz_e2e.py")],
         about="WiZ end to end against simulated lights and plugs"),
    dict(name="kasa", secs=12, needs=["unit:kasa_cli", "cli"], cmds=[py("tests/kasa_e2e.py")],
         about="Kasa end to end against simulated plugs, strip, dimmer, bulbs"),
    dict(name="shelly", secs=15, needs=["unit:shelly_cli", "cli"], cmds=[py("tests/shelly_e2e.py")],
         about="Shelly end to end against simulated Gen1 and Gen2+ devices"),
    dict(name="hue", secs=36, needs=["unit:hue_cli", "cli"], cmds=[py("tests/hue_e2e.py")],
         about="Hue end to end against a simulated bridge"),
    dict(name="homekit", secs=51, needs=["unit:homekit_cli"], cmds=[py("tests/homekit_e2e.py")],
         about="HomeKit end to end against a simulated bridge"),
    dict(name="sim-build", secs=60, needs=["sim-tree"], build=True, timeout=3600, cmds=[_sim_build_cmd],
         about="compiles the sim build, which has no experiments and has the screen code, as the device build: "
               "a change to shared code must compile there too (not in the profiles; the path map adds it)"),
    dict(name="screens", secs=10, needs=["sim"], cmds=[pyi("tests/shots_compare.py")],
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
               "(added when --baseline is given with a live group, and always in the release profile)"),
    dict(name="held-out", secs=2700, needs=["cli", "live"], heldout=True, judged=True, timeout=MAX_LIVE_SECONDS,
         cmds=[_heldout_eval_cmd, _heldout_suite_cmd, _heldout_judge_cmd],
         about="the release's held-out run: both sets once on the stand-in web's held-out seed and variants, which no "
               "day-to-day run uses, judged on its own (tests/exp_compare.py --held-out: a pass-rate floor per set, every "
               "critical case passing); reported beside the gate. Only in the release profile: never tune on it"),
    dict(name="web-contract", secs=15, needs=[], ondemand=True, internet=True, cmds=[py("tests/web_contract.py")],
         about="the stand-in web's shapes against the real weather, place, rate and price services (a handful of direct "
               "requests, never the search; needs the internet): on demand, before a release, in no profile"),
]
BY_NAME = {g["name"]: g for g in GROUPS}
OFFLINE = [g["name"] for g in GROUPS if not g.get("live") and not g.get("auto") and not g.get("build") and not g.get("heldout")
           and not g.get("ondemand")]
LIVE = [g["name"] for g in GROUPS if g.get("live")]
LIVE_SET = {"live-scored": "scored", "live-suite": "suite"}            # tests/exp_compare.py's names for them
LIVE_FILE = {"live-scored": "eval.json", "live-suite": "suite.json"}   # their results in the run folder
# Inherited switches that change what a group tests: never passed on (the runner sets its own).
SCRUB = ("ZB_UNIT_BUILD_ONLY", "ZB_UNIT_SKIP_SCRIPTS", "ZB_UPDATE_SHOTS", "ZB_UPDATE_SNAPSHOTS", "HUE_E2E_ONLY",
         "ZB_FAST_MODE", "ZB_EVAL_TRACE")   # ZB_EVAL_TRACE: the live runs are traced by --trace, which records it
# What a release refuses: it runs every case of both sets as they are defined.
RELEASE_REFUSES = ("tags", "sample", "skip_tags", "only", "scored_only", "fast", "exp")
RUNNER_EXP = ("ZB_EXP_TRACE", "ZB_EXP_WEB", "ZB_EXP_NO_HARDWARE", "ZB_EXP_NET")   # set by the live sets' runners per case

PROFILES = {
    "quick": ("every offline group but the slow ones; leaves out firetv (about 5 min) and the live sets",
              [n for n in OFFLINE if not BY_NAME[n].get("slow")]),
    "offline": ("every offline group, firetv included (about 10 min)", OFFLINE),
    "change": ("the groups that cover the files changed against --base (default main), the working tree "
               "included; for a change to what the model is told, which live cases to run five times "
               "(suite tags, scored-set names; run only with --live)", None),
    "release": ("every offline group on fresh builds, then the scored set and the suite once, then the gate "
                "against --baseline (results of both sets, recorded by the current runners), then the held-out run, "
                "judged on its own", OFFLINE + LIVE + ["held-out"]),
}

# ---------------------------------------------------------------- what a changed file needs
# The first pattern that matches a path decides. "{family}" stands for the device family the pattern
# caught. A path that matches nothing gets the quick profile.
_F = "|".join(FAMILIES)
DRIVERS = list(FAMILIES)
QUICK = "<quick>"
PATH_RULES = [
    (r"^(docs/|licenses/|LICENSE$|[^/]*\.md$|.*/README[^/]*$|\.gitignore$)", [], "documentation"),
    (r"^(.*/)?__pycache__/|.*\.pyc$", [], "Python's bytecode cache"),
    (r"^(scripts/package-deb\.sh$|packaging/|tests/packaging_test\.py$)", ["packaging"],
     "the package and its maintainer scripts"),
    (r"^(scripts/|extras/|\.github/)", [], "deployment: no offline test covers it"),
    (r"^tests/run\.py$", ["runner"], "this runner"),
    (r"^tests/netguard\.py$", ["runner", "tools", "loop", "scorer"], "the network boundary of every test process"),
    (r"^tests/(exp_compare|exp_report|web_contract)\.py$", ["runner"], "the gate and the reports (their self-test is in runner)"),
    (r"^tests/scorer_test\.py$", ["scorer"], "the scorer's own test"),
    (r"^tests/agent_eval\.py$", ["scorer", "cases", "loop"],
     "the scored set and the scoring both live sets use (agent_loop_test.py reads its parser)"),
    (r"^tests/agent_suite\.py$", ["scorer", "cases"], "the suite's runner"),
    (r"^tests/suite/from_conversation\.py$", ["cases", "scorer", "loop"],
     "drafts a case from a saved conversation (agent_loop_test.py runs it)"),
    (r"^tests/suite/", ["cases", "scorer"], "the generated suite (scorer_test.py reads its cases)"),
    (r"^tests/mocks/(fake_web\.py|web/)", ["cases", "scorer", "tools"],
     "the stand-in web (its fixtures: the validators and scorer_test.py; tools_cli.py starts it)"),
    (r"^tests/(golden/|shots/|shots_compare\.py$)", ["screens"], "the golden screenshots"),
    (r"^tests/mocks/mock_tapo\.py$", ["tapo", "loop"],
     "the simulated Tapo plugs (home_chat_e2e.py and selfheal_chat_e2e.py start them too)"),
    (r"^tests/mocks/(?P<family>hue|firetv)/", ["unit", "{family}"],
     "a simulated device whose files the driver's unit test reads too"),
    (r"^tests/mocks/(?P<family>%s)/" % _F, ["{family}"], "a simulated device"),
    (r"^tests/(?P<family>%s)_e2e\.py$" % _F, ["{family}"], "a device family's end-to-end test"),
    (r"^tests/(mocks/(mock_chat|fake_model)\.py|home_chat_e2e\.py|selfheal_chat_e2e\.py|agent_loop_test\.py)$",
     ["loop"], "the request loop's tests"),
    (r"^tests/tools_cli\.py$", ["tools"], "the offline tools' test"),
    (r"^tests/unit/(?P<family>%s)_(test|cli)\.cpp$" % _F, ["unit", "{family}"], "a driver's unit test"),
    (r"^tests/(unit/|snapshots/|run_unit\.sh$)", ["unit"], "the unit tests"),
    # Source shared with the device build also compiles the sim build (sim-build): the no-experiments
    # side of experiment.h, `#if ZB_EXPERIMENTS` and `#if !ZB_CLI_ONLY` are compiled nowhere else.
    (r"^src/smarthome/drivers/(?P<family>%s)\.(h|cpp)$" % _F, ["unit", "{family}", "sim-build"], "one device driver"),
    (r"^src/smarthome/tapo\.(h|cpp)$", ["unit", "tapo", "sim-build"], "the Tapo transport"),
    (r"^src/smarthome/", ["unit"] + DRIVERS + ["loop", "sim-build"], "the shared smart-home layer"),
    (r"^src/net/", ["unit", "tools", "loop"] + DRIVERS + ["sim-build"], "the HTTP layer (service and devices)"),
    (r"^src/lv_conf\.h$", ["sim-build", "screens"], "LVGL's settings (not in the cli build)"),
    (r"^src/ui/", ["unit", "sim-build", "screens"], "the screens and keys"),
    (r"^src/platform/(display|input)", ["sim-build", "screens"], "the screen and keyboard (sim build)"),
    (r"^src/platform/", ["unit", "tools", "loop", "sim-build"], "audio, camera, helper programs"),
    (r"^src/(agent/|main\.cpp$)", ["unit", "loop", "tools", "sim-build"], "the request loop and the tools"),
    (r"^src/core/", ["unit", "tools", "loop", "sim-build"], "settings, stores, timers, radio, utilities"),
    (r"^(CMakeLists\.txt|CMakePresets\.json|cmake/|third_party/)", [QUICK, "sim-build"], "the build itself"),
]

# Changes to what the model is told: which live cases to run five times. A rule gives suite tags, or
# {"scored": [...]} for scored-set names (--scored-only); "scan" reads the path, the changed lines and the
# function each change is in for the tools they touch (a tool is one file, src/agent/tools/<name>.cpp); "all"
# means a sample of every suite category.
MODEL_FACING = [
    (r"^src/agent/prompt\.cpp$", "all"),
    (r"^src/agent/(agent|llm|guards|controls)\.cpp$", "all"),
    (r"^src/agent/tools/", "scan"),
    (r"^src/smarthome/(tool|control)\.cpp$", ["home", "tv"]),   # the schema; the sentences results carry
    (r"^src/core/timers\.cpp$", ["timer", "countdown"]),
    (r"^src/core/radio\.cpp$", {"scored": ["radio_"]}),        # the suite has no radio case; the scored set's radio_*
]
# Words in a changed line (or the function it is in) that point at a suite category.
TAG_WORDS = [
    ("timer", r"\btimers?\b|t_timer"), ("reminder", r"remind"), ("calendar", r"calendar"),
    ("weather", r"weather"), ("calc", r"\bcalc"), ("currency", r"currenc|exchange rate"),
    ("stock", r"\bstock"), ("route", r"\broute|directions"), ("search", r"web_search|search"),
    ("music", r"music"), ("memory", r"memor|remember"), ("date", r"date_info"),
    ("world_time", r"world_time|time ?zone"), ("timezone", r"time ?zone"), ("breathe", r"breath"),
    ("look", r"\blook\b|camera|photo"), ("tv", r"\btv\b|infrared|\bir_"), ("home", r"smart_home"),
    ("device", r"battery|brightness|volume|wi-?fi"),
]
# The same for the scored set's cases, by name.
SCORED_WORDS = [("radio_", r"radio")]


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
    """What to run five times for the model-facing paths among PATHS: (suite tags or "all", [paths],
    [scored-set name parts])."""
    tags, hit, scored, everything = set(), [], set(), False
    for p in paths:
        for pattern, rule in MODEL_FACING:
            if not re.match(pattern, p):
                continue
            hit.append(p)
            if rule == "all":
                everything = True
            elif rule == "scan":
                text = (p + "\n" + (diff_text_of(p) if diff_text_of else "")).lower()
                found = [t for t, words in TAG_WORDS if re.search(words, text)]
                names = [n for n, words in SCORED_WORDS if re.search(words, text)]
                tags.update(found)
                scored.update(names)
                if not found and not names:
                    everything = True
            elif isinstance(rule, dict):
                scored.update(rule.get("scored", []))
                tags.update(rule.get("tags", []))
            else:
                tags.update(rule)
            break
    if categories:
        tags = {t for t in tags if t in categories}
    return ("all" if everything else sorted(tags)), hit, sorted(scored)


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
PER_CASE = 3.6   # seconds a live case takes at --jobs 1 (whole runs: 480 s for 123, 2100 s for 601)


def live_count(name, o):
    """How many cases a live group will run with these options, or None when that cannot be told."""
    sys.path.insert(0, HERE)
    try:
        if name == "live-suite":
            return len(suite_selected(o, o.skip_tags))
        only = scored_only(o)
        import agent_eval as ev
        words = [w for w in only.split(",") if w]
        return sum(1 for c in ev.CASES if not words or any(w in c["name"] for w in words))
    except Exception:  # a set that cannot be read here: the whole run's time stands
        return None
    finally:
        if sys.path and sys.path[0] == HERE:
            sys.path.pop(0)


def group_secs(name, o):
    g = BY_NAME[name]
    n = live_count(name, o) if g.get("live") else None
    return g["secs"] if n is None else max(5, n * PER_CASE * max(1, o.repeat))


def live_prefix():
    w = os.environ.get("ZB_EVAL_WRAPPER", "").strip()
    return shlex.split(w) if w else []


def scored_only(o):
    """--only for the scored set: --scored-only when given, else --only (both sets share it)."""
    return o.scored_only or o.only


def heldout_command(cmd, out, o=None):
    """A live set's command for the held-out run: the held-out seed, its own results file, and with --calibrate N
    each case N times."""
    cmd = list(cmd)
    cmd[cmd.index("--json") + 1] = out
    if "--repeat" in cmd:
        k = cmd.index("--repeat")
        del cmd[k:k + 2]
    n = getattr(o, "calibrate", 0) if o is not None else 0
    return cmd + ["--held-out"] + (["--repeat", str(n)] if n > 1 else [])


def eval_command(o, run):
    # --trace always: a traced run sends stream_options.include_usage, so a baseline and a variant must
    # both be traced (tests/exp_compare.py leaves out traced-against-untraced pairs).
    cmd = live_prefix() + py("tests/agent_eval.py", "--jobs", "1", "--trace", "--json", os.path.join(run, "eval.json"))
    only = scored_only(o)
    if only:
        cmd += ["--only", only]
    if o.repeat > 1:
        cmd += ["--repeat", str(o.repeat)]
    if o.fast is not None:
        cmd += ["--fast", o.fast]
    return cmd


def suite_command(o, run):
    cmd = live_prefix() + py("tests/agent_suite.py", "--jobs", "1", "--trace", "--json", os.path.join(run, "suite.json"))
    for flag, value in (("--tags", o.tags), ("--skip-tags", o.skip_tags), ("--only", o.only)):
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
    """tests/exp_compare.py on this run's live results. It must compare every set this run ran (--require),
    and in a release both sets with their manifest fields (--release); it writes its verdict to gate.json."""
    if run == "<run>":
        ran = [g for g in LIVE if g in getattr(o, "chosen", LIVE)]
    else:
        ran = [g for g in LIVE if g in getattr(o, "live_ran", ())]
    variant = [os.path.join(run, LIVE_FILE[g]) for g in ran if run == "<run>" or os.path.exists(os.path.join(run, LIVE_FILE[g]))]
    cmd = py("tests/exp_compare.py", "--base", o.baseline or "<baseline>", "--variant", ",".join(variant) or "<none>",
             "--tags", "--verdict", os.path.join(run, "gate.json"))
    if release_mode(o):
        cmd += ["--release", "--require", "scored,suite"]
    elif ran:
        cmd += ["--require", ",".join(LIVE_SET[g] for g in ran)]
    if o.skip_tags:
        cmd += ["--skip-tags", o.skip_tags]
    return cmd


def exp_env(o):
    """The experiment switches given with --exp, for the live groups."""
    return dict(e.split("=", 1) for e in o.exp)


def live_notes(o, chosen):
    notes = []
    if o.no_search and any(n in chosen for n in LIVE):
        notes.append("--no-search changes nothing: every live case's web reads go to the stand-in web, search included")
    if any(n in chosen for n in LIVE):
        exp = sorted(k for k in os.environ if k.startswith("ZB_EXP_"))
        if exp:
            notes.append("not passed on from the environment: %s (use --exp NAME=VALUE for a variant)" % ", ".join(exp))
        if o.exp:
            notes.append("experiment switches for the live sets: %s; the results are of that variant" % ", ".join(o.exp))
        if os.environ.get("ZB_EVAL_MUSIC_DIR") is not None:
            notes.append("ZB_EVAL_MUSIC_DIR is set: the suite's music library is not the stand-in one")
    return notes


# ---------------------------------------------------------------- what a group needs
def tool_path():
    tools = os.path.join(ROOT, ".tools", "bin")
    path = os.environ.get("PATH", "")
    return tools + os.pathsep + path if os.path.isdir(tools) else path


def need_path(need):
    if need in ("cli", "sim", "cp0"):
        return "build/%s/zero_bot" % need
    if need.startswith("unit:"):
        return "build/unit/" + need[5:]
    return None


# What the driver test programs are built from (tests/run_unit.sh), besides every header in src/.
SIM_ONLY = ("src/ui/", "src/platform/display.", "src/platform/input.", "src/lv_conf.h")    # not in the cli build
# What the cli and sim builds are made from: the sources, the bundled libraries and the build's own files.
BUILD_INPUTS = ("src", "third_party", "cmake", "CMakeLists.txt", "CMakePresets.json")
UNIT_SOURCES = ("src/smarthome/", "src/net/", "src/core/util.", "src/platform/proc.", "src/platform/sys.",
                "src/platform/net_status.", "tests/unit/")


def stale_note(need):
    """Why a built program may be older than its sources, or ''. By modification times: a build relinks
    whenever a source it uses changed. (ninja -n cannot tell: the build's globs always look out of date.)"""
    if need in ("cli", "sim", "cp0"):
        made = os.path.join(ROOT, need_path(need))
        tops, wanted = BUILD_INPUTS, (lambda rel: need != "cli" or not rel.startswith(SIM_ONLY))
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
        if not os.path.exists(os.path.join(ROOT, top)):
            continue
        walk = [(ROOT, [], [top])] if os.path.isfile(os.path.join(ROOT, top)) else os.walk(os.path.join(ROOT, top))
        for d, _, files in walk:
            for f in files:
                rel = os.path.relpath(os.path.join(d, f), ROOT).replace(os.sep, "/")
                if wanted(rel) and os.path.getmtime(os.path.join(d, f)) > t:
                    return "%s is older than %s" % (os.path.dirname(need_path(need)), rel)
    return ""


# The device binary a release packages (cp0): no group runs it, so a release builds it and reads it. Strings
# only the cli build has (its experiments and the evaluator's records and lines), and a build machine's path
# (a worktree's build reaches LVGL outside the prefix map: AGENTS.md section 8).
DEVICE_FORBIDDEN = ("ZB_EXP_", "ZB_EVAL_RECORDS", "tool_done", "[call]", "[state]", "/home/")


def project_version():
    """The version in CMakeLists.txt's project(... VERSION x.y.z ...), or ''."""
    try:
        with open(os.path.join(ROOT, "CMakeLists.txt"), encoding="utf-8") as f:
            m = re.search(r"project\s*\([^)]*?\bVERSION\s+(\d+(?:\.\d+)+)", f.read())
    except OSError:
        return ""
    return m.group(1) if m else ""


def device_problems(path, version):
    """What is wrong with the device binary at PATH, as `strings` would read it: missing, its --version
    ("zero_bot X") not VERSION, a cli-only string, a build machine's path. The text found is never quoted."""
    rel = os.path.relpath(path, ROOT)
    try:
        with open(path, "rb") as f:
            found = [s.decode("ascii") for s in re.findall(rb"[\x20-\x7e]{4,}", f.read())]
    except OSError:
        return ["%s is missing" % rel]
    out = []
    versions = sorted({v for s in found for v in re.findall(r"\bzero_bot (\d+(?:\.\d+)+)", s)})
    if not version or version not in versions:
        out.append("%s is version %s, CMakeLists.txt says %s" % (rel, "/".join(versions) or "unknown", version or "nothing"))
    for word in DEVICE_FORBIDDEN:
        if any(word in s for s in found):
            out.append("%s holds %s" % (rel, "a path of the build machine (/home/)" if word == "/home/" else
                                         "the cli-only string %s" % word))
    return out


def device_check(broken):
    """A release's row for the device build: (status, line)."""
    if "cp0" in broken:
        return "fail", "not checked: the device build failed"
    problems = device_problems(os.path.join(ROOT, need_path("cp0")), project_version())
    if not problems and stale_note("cp0"):
        problems = ["stale build: %s" % stale_note("cp0")]
    if problems:
        return "fail", "; ".join(problems)
    return "pass", "%s is %s, with no cli-only strings or build paths" % (need_path("cp0"), project_version())


def release_mode(o):
    """A release, however it was asked for: --profile release or --groups release."""
    return o.profile == "release" or "release" in [g.strip() for g in o.groups.split(",")]


def probe_cli(path):
    """Why the cli build at PATH cannot run live cases, or '': it must be an experiments build (an unknown
    ZB_EXP_OFF name stops it), its hardware guard must hold (under ZB_EXP_NO_HARDWARE a stand-in camera
    program that would work is never started), and it must write the evaluator's records (ZB_EVAL_RECORDS:
    a start and an end even for a run of no turns). Nothing here reaches a service or a device."""
    if not os.access(path, os.X_OK):
        return "%s is missing" % os.path.relpath(path, ROOT)
    tmp = tempfile.mkdtemp(prefix="zb-probe-")
    try:
        base = netguard.child_env(netguard.mock_env(), HOME=tmp, TZ="UTC", ZB_DRY_RUN="1", ZB_NO_AUDIO="1", XDG_CONFIG_HOME=tmp + "/config",
                                  XDG_DATA_HOME=tmp + "/data", XDG_CACHE_HOME=tmp + "/cache")   # the stand-in network, pinned

        def run(args, stdin="", **extra):
            return subprocess.run([path] + args, env=dict(base, **extra), input=stdin, capture_output=True, text=True,
                                  timeout=30, cwd=tmp)
        r = run(["--prompt"], ZB_EXP_OFF="zb-probe")
        if r.returncode != 2 or "is not a control this build knows" not in r.stderr:
            return "it is not an experiments build (ZB_EXPERIMENTS): no hardware guard, no records"
        stand_ins, marker = tmp + "/bin", tmp + "/camera-used"
        os.makedirs(stand_ins)
        with open(stand_ins + "/rpicam-jpeg", "w") as f:
            f.write('#!/bin/sh\ntouch "%s"\nfor a; do out="$a"; done\nhead -c 4000 /dev/zero > "$out"\n' % marker)
        os.chmod(stand_ins + "/rpicam-jpeg", 0o755)
        r = run(["--tool", "look", "{}"], ZB_EXP_NO_HARDWARE="1", PATH=stand_ins + os.pathsep + base["PATH"])
        if "switched off for this test run" not in r.stdout or os.path.exists(marker):
            return "its hardware guard does not hold (ZB_EXP_NO_HARDWARE did not keep the camera out)"
        records = tmp + "/records.jsonl"
        run(["--chat"], ZB_EVAL_RECORDS=records)
        lines = [json.loads(l) for l in open(records, encoding="utf-8")] if os.path.exists(records) else []
        if not lines or lines[0].get("ev") != "start" or lines[-1].get("ev") != "end":
            return "it writes no evaluator's records (ZB_EVAL_RECORDS)"
        lacking = [c for c in netguard.REQUIRED_CAPS if c not in (lines[0].get("caps") or [])]
        if lacking:
            return "it does not keep test runs off the real web and network (it lacks %s)" % ", ".join(lacking)
        return ""
    except (OSError, ValueError, subprocess.TimeoutExpired) as e:
        return "it could not be probed (%s)" % e.__class__.__name__
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def cli_problem(o):
    """probe_cli for build/cli/zero_bot, once per binary in a run."""
    path = os.path.join(ROOT, need_path("cli"))
    key = "%s@%s" % (path, os.path.getmtime(path) if os.path.exists(path) else "-")
    cache = o.__dict__.setdefault("probed", {})
    if key not in cache:
        cache[key] = probe_cli(path)
    return cache[key]


def missing(group, o, run):
    """What stops GROUP from running now, or ''."""
    for need in group["needs"]:
        if need == "live":
            if not live_prefix() and not (os.environ.get("ZB_API_BASE") and os.environ.get("ZB_API_KEY")):
                return "needs ZB_API_BASE and ZB_API_KEY, or ZB_EVAL_WRAPPER (a command that sets them)"
            if "cli" in group["needs"] and os.path.exists(os.path.join(ROOT, need_path("cli"))) and cli_problem(o):
                return "build/cli/zero_bot cannot run live cases: " + cli_problem(o)
        elif need == "baseline":
            if not o.baseline:
                return "needs --baseline"
            if not any(os.path.exists(os.path.join(run, n)) for n in LIVE_FILE.values()):
                return "no live results to compare (the live runs wrote none)"
        elif need == "sim-tree":
            if not os.path.exists(os.path.join(ROOT, "build", "sim", "CMakeCache.txt")):
                return "build/sim is not configured (cmake --preset sim, or --build)"
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
        tags, hit, scored = model_facing_tags(paths, diff_reader(fork), suite_categories())
        if hit:
            advice = (tags, hit, scored)
            if o.live:
                if (tags == "all" or tags) and "live-suite" not in names:
                    names.append("live-suite")
                    why["live-suite"] = "--live: the model-facing change"
                    if not (o.tags or o.sample or o.only):
                        if tags == "all":
                            o.sample = 60
                        else:
                            o.tags = ",".join(tags)
                if scored and "live-scored" not in names:
                    names.append("live-scored")
                    why["live-scored"] = "--live: the model-facing change"
                    if not (o.scored_only or o.only):
                        o.scored_only = ",".join(scored)
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
    if "held-out" in names and not (release_mode(o) or o.calibrate):
        raise SystemExit("the held-out run is the release profile's alone (or --groups held-out --calibrate N, to set its "
                         "floors): never tune on it")
    if (o.baseline or release_mode(o)) and any(n in names for n in LIVE):
        names.append("gate")
        why["gate"] = "a release" if release_mode(o) else "--baseline with a live run"
    chosen = [g["name"] for g in GROUPS if g["name"] in set(names)]
    o.chosen = chosen
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
    tags, hit, scored = advice
    ns = ""
    out = ["what the model is told changed (%s): run the live cases it touches five times," % ", ".join(hit)]
    if tags == "all":
        out.append("  tests/run.py --groups live-suite --sample 60 --repeat 5%s   (a few from every category)" % ns)
    elif tags:
        out.append("  tests/run.py --groups live-suite --tags %s --repeat 5%s" % (",".join(tags), ns))
    if scored:
        out.append("  tests/run.py --groups live-scored --scored-only %s --repeat 5%s   (the scored set's cases)"
                   % (",".join(scored), ns))
    out.append("  then both sets once: tests/run.py --profile release --baseline <earlier eval.json>,<earlier suite.json>%s" % ns)
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
    """The group's commands now; a command function that gives None has nothing to run here."""
    return [c for c in (c(o, run) if callable(c) else c for c in group["cmds"]) if c]


def presets_needed(chosen):
    needs = set()
    for n in chosen:
        g = BY_NAME[n]
        needs.update(x for x in g["needs"] + g.get("uses", []) if x in ("cli", "sim"))
        if "sim-tree" in g["needs"]:
            needs.add("sim")
        if any(x.startswith("unit:") for x in g["needs"]) and "unit" not in chosen:
            needs.add("unit")
    return [p for p in ("cli", "sim", "unit") if p in needs]


def build_presets(chosen, o):
    """What --build builds first: what the groups need and, in a release, the device build (cp0)."""
    return presets_needed(chosen) + (["cp0"] if release_mode(o) else [])


def print_plan(chosen, why, notes, advice, o, out=None):
    out = out or sys.stdout
    for line in notes:
        print(line, file=out)
    if o.build:
        builds = build_presets(chosen, o)
        print("build first: %s" % (", ".join(builds) or "nothing"), file=out)
    total = sum(group_secs(n, o) for n in chosen)
    print("would run %s, about %s:" % (plural(len(chosen), "group"), duration(total)), file=out)
    for n in chosen:
        g = BY_NAME[n]
        flags = []
        if g.get("file") and not os.path.exists(os.path.join(ROOT, g["file"])):
            flags.append("skipped: %s is not in this tree" % g["file"])
        count = live_count(n, o) if g.get("live") else None
        if count == 0:
            flags.append("skipped: no case of this set is selected")
        elif count:
            flags.append(plural(count, "case") + (" x%d" % o.repeat if o.repeat > 1 else ""))
        print("  %-12s ~%-6s %s%s" % (n, duration(group_secs(n, o)), why.get(n, ""),
                                     ("; " + "; ".join(flags)) if flags else ""), file=out)
        for c in commands_of(g, o, "<run>"):
            print("      " + command_text(c), file=out)
    for line in live_notes(o, chosen) + advice_lines(advice, o):
        print(line, file=out)
    for line in release_problems(o):
        print("release cannot start: " + line, file=out)


def release_problems(o):
    """Why a release would stop before running anything: its baseline (readable, valid, both sets, and the
    cases as defined now, measured against the roster of cases in this tree: at least 90% of each set and
    every critical case, scored by the current scorer and traced), a cli build that cannot run live cases,
    and, with --no-build, builds older than any of their inputs or a device build that fails its check."""
    if not release_mode(o):
        return []
    out = []
    if not o.baseline:
        out.append("--baseline is needed: the scored set's and the suite's results to compare with "
                   "(eval.json and suite.json of an earlier run)")
    else:
        sys.path.insert(0, HERE)
        try:
            import exp_compare as ec
            records, problems = ec.load(o.baseline)
            out += ["the baseline: " + p for p in problems]
            have = {ec.set_of(r) for r in records}
            for s in ec.SETS:
                if s not in have:
                    out.append("the baseline has no %s results: a release is judged on both sets" % s)
            bad = [r for r in records if ec.record_problems(r)]
            if bad:
                out.append("%d baseline records are not valid results (%s: %s)" % (len(bad), bad[0].get("case"),
                                                                                    ", ".join(ec.record_problems(bad[0]))))
            old = sum(1 for r in records if any(f not in r for f in ec.MANIFEST))
            if old:
                out.append("%d baseline runs lack %s (recorded before 2026-11-19's runners): record a new baseline "
                           "(tests/run.py --groups live-scored,live-suite)" % (old, ", ".join(ec.MANIFEST)))
            else:
                try:
                    roster = ec.load_roster()
                except Exception as e:
                    out.append("the roster of cases could not be read: %s" % e)
                else:
                    out += ec.roster_gaps(records, roster, "baseline", ec.MIN_COVERAGE, trace=True)
        finally:
            if sys.path and sys.path[0] == HERE:
                sys.path.pop(0)
    if o.no_build:
        for need in ("cli", "sim", "cp0"):
            if not os.path.exists(os.path.join(ROOT, need_path(need))):
                out.append("%s is missing (a release builds it without --no-build)" % need_path(need))
            elif stale_note(need):
                out.append("a release needs fresh builds: %s (leave out --no-build to rebuild)" % stale_note(need))
            elif need == "cp0":
                out += ["the device build: " + p for p in device_problems(os.path.join(ROOT, need_path(need)),
                                                                          project_version())]
    if (o.no_build or not o.build) and os.path.exists(os.path.join(ROOT, need_path("cli"))) and cli_problem(o):
        out.append("build/cli/zero_bot cannot run live cases: " + cli_problem(o))
    return out


def plural(n, word):
    return "%d %s%s" % (n, word, "" if n == 1 else "s")


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
    for pattern in (r"^gate: .*", r"^held-out: .*", r"^\d+/\d+ passed[^|]*", r".*\d+ checks, \d+ failed.*", r".*\d+ screens, \d+ differ.*",
                    r".*(up to date|OUT OF DATE).*", r"^no cases selected$", r"^ninja: no work to do\.?$",
                    r"^\[\d+/\d+\] Linking .*"):
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


def group_env(tmp, extra, group=None):
    """The environment a group runs in: fresh state folders, and none of the inherited switches that would
    change what it tests (SCRUB, ZB_EXP_*); EXTRA is what the runner sets itself. No group gets the search key; only
    the live groups get the AI service's address and key; every group but the one that must reach the internet
    (web-contract) has its proxies at the trap, only 127.0.0.1 and localhost exempt (tests/netguard.py)."""
    group = group or {}
    env = {k: v for k, v in os.environ.items() if k not in SCRUB and not k.startswith("ZB_EXP_")}
    live = group.get("live") or group.get("heldout")
    if group.get("internet"):
        for k in netguard.KEYS:
            env.pop(k, None)
    else:
        env = netguard.child_env(env, keep=("ZB_API_BASE", "ZB_API_KEY") if live else (),
                                 allow_origin=env.get("ZB_API_BASE", "") if live else "")
    for k in ("CONFIG", "DATA", "CACHE"):
        d = os.path.join(tmp, k.lower())
        os.makedirs(d, exist_ok=True)
        env["XDG_%s_HOME" % k] = d
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PATH"] = tool_path()
    env.update(extra or {})
    return env


def build(preset, run, o):
    """Build one preset (cli, sim, cp0) or the unit test programs; (ok, line)."""
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
    if group.get("live") and live_count(name, o) == 0:
        return "skip", "no case of this set is selected", 0
    problem = missing(group, o, run)
    logpath = os.path.join(run, name + ".log")
    if problem:
        with open(logpath, "w") as log:
            log.write(problem + "\n")
        return "fail", problem, 0
    for need in group["needs"] + group.get("uses", []):
        note = stale_note(need) if need_path(need) and os.path.exists(os.path.join(ROOT, need_path(need))) else ""
        if note and release_mode(o):
            with open(logpath, "w") as log:
                log.write("stale build: %s\n" % note)
            return "fail", "stale build: %s (--build rebuilds)" % note, 0
        if note:
            warnings.setdefault(note, []).append(name)
    tmp = tempfile.mkdtemp(prefix="zb-run-%s-" % name)
    t0 = time.time()
    rc_all = 0
    try:
        extra = dict(group.get("env") or {}, **(exp_env(o) if group.get("live") else {}))
        if group.get("heldout"):
            extra["ZB_HELD_OUT_RUN"] = "calibrate" if o.calibrate else "release"   # the scorers take --held-out only then
        env = group_env(tmp, extra, group)
        with open(logpath, "w") as log:
            log.write("# %s: %s\n" % (name, group["about"]))
            cmds = commands_of(group, o, run)
            for k, cmd in enumerate(cmds):
                rc = RUN_COMMAND(cmd, log, env, group.get("timeout", 900))
                log.write("(exit %d)\n" % rc)
                if group.get("judged") and k < len(cmds) - 1 and rc == 1:
                    rc = 0   # cases that failed: the last command (the judge) decides
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
    total = sum(group_secs(n, o) for n in chosen)
    print("%s, about %s; logs in %s/" % (plural(len(chosen), "group"), duration(total), rel))
    for line in live_notes(o, chosen):
        print(line)
    rows, warnings, broken = [], {}, set()
    t_all = time.time()
    try:
        if o.build:
            for preset in build_presets(chosen, o):
                ok, line = build(preset, run, o)
                print(line, flush=True)
                if not ok:
                    broken.add(preset)
        if release_mode(o):   # the binary the package is made from: a problem fails the release
            status, line = device_check(broken)
            rows.append(("device", status, line, 0))
            print("%-4s  %-12s %5d s  %s" % (status.upper(), "device", 0, line), flush=True)
        for n in chosen:
            g = BY_NAME[n]
            needs = {x.split(":")[0] for x in g["needs"]}
            if needs & broken:
                status, line, secs = "fail", "not run: the build it needs failed", 0
            else:
                status, line, secs = run_group(g, o, run, "gate" in chosen, warnings)
            rows.append((n, status, line, secs))
            if n in LIVE and status != "skip":
                o.live_ran = getattr(o, "live_ran", []) + [n]
            log = os.path.join(rel, n + ".log")
            print("%-4s  %-12s %5d s  %s%s" % (status.upper(), n, secs, line,
                                               "   (%s)" % log if status == "fail" else ""), flush=True)
    except KeyboardInterrupt:
        print("interrupted")
        rows.append(("(interrupted)", "fail", "", 0))
    rows = judged(rows, run)
    summary = summary_lines(rows, warnings, advice, o, rel, time.time() - t_all)
    with open(os.path.join(run, "summary.txt"), "w") as f:
        f.write("\n".join(summary) + "\n")
    print("\n".join(summary))
    return 0 if rows and all(s in ("pass", "skip", "ran") for _, s, _, _ in rows) else 1


def judged(rows, run):
    """A live run with failed cases ("ran") passes only when the gate's verdict covers its set: the gate
    compared cases of it and passed. Otherwise it fails, and a line says why."""
    try:
        with open(os.path.join(run, "gate.json"), encoding="utf-8") as f:
            verdict = json.load(f)
    except (OSError, ValueError):
        verdict = None
    out = []
    for n, status, line, secs in rows:
        if status == "ran":
            compared = ((verdict or {}).get("sets") or {}).get(LIVE_SET[n], {}).get("compared", 0)
            why = ("no gate verdict" if verdict is None else
                   "the gate compared none of its cases" if not compared else
                   "the gate's verdict: %s" % verdict.get("verdict") if verdict.get("verdict") != "passes" else "")
            if why:
                status, line = "fail", "%s; failed cases, and %s" % (line, why)
                print("FAIL  %-12s        %s" % (n, line), flush=True)
        out.append((n, status, line, secs))
    return out


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
    ap.add_argument("--build", action="store_true", help="build what the chosen groups need first (a release always does)")
    ap.add_argument("--no-build", action="store_true", help="a release: do not build; any build older than an input stops it")
    ap.add_argument("--live", action="store_true", help="for --profile change: also run the live suite it advises")
    ap.add_argument("--baseline", default="", help="results file(s) to gate the live runs against (A.json[,B.json])")
    ap.add_argument("--no-search", action="store_true", help="changes nothing (kept for old command lines): every live case "
                    "uses the stand-in web, search included")
    ap.add_argument("--tags", default="", help="live suite: cases with any of these tags (a+b means both)")
    ap.add_argument("--skip-tags", default="", help="live suite and gate: leave out cases with these tags")
    ap.add_argument("--sample", type=int, default=0, help="live suite: a stratified sample of this many cases")
    ap.add_argument("--seed", type=int, default=1, help="live suite: seed for --sample")
    ap.add_argument("--only", default="", help="live sets: cases whose name contains this (comma-separated)")
    ap.add_argument("--scored-only", default="", help="the scored set only: like --only (which then applies to the suite)")
    ap.add_argument("--exp", action="append", default=[], metavar="ZB_EXP_X=V",
                    help="live sets: an experiment switch for a variant's run (inherited ZB_EXP_* are not passed on)")
    ap.add_argument("--repeat", type=int, default=1, help="live sets: run each case this many times")
    ap.add_argument("--fast", default=None, help="live sets: force fast mode on (1) or off (0)")
    ap.add_argument("--calibrate", type=int, default=0, metavar="N",
                    help="with --groups held-out only: run the held-out set N (3 or more) times to set its floors (HELD_OUT_FLOOR); "
                         "never to tune on: read only the per-group rates it prints")
    ap.add_argument("--self-test", action="store_true", help="check this runner and exit")
    o = ap.parse_args(argv)
    if o.baseline:
        o.baseline = ",".join(os.path.abspath(p) for p in o.baseline.split(",") if p)
    for e in o.exp:
        k = e.split("=", 1)[0]
        if "=" not in e or not k.startswith("ZB_EXP_") or k in RUNNER_EXP:
            ap.error("--exp %s: give ZB_EXP_NAME=VALUE; %s are set by the runners" % (e, ", ".join(RUNNER_EXP)))
    if o.calibrate and ([g.strip() for g in o.groups.split(",") if g.strip()] != ["held-out"] or o.profile or
                        any(getattr(o, k) for k in RELEASE_REFUSES)):
        ap.error("--calibrate N goes with --groups held-out alone, and no filter or variant")
    if o.calibrate and o.calibrate < 3:
        ap.error("--calibrate needs at least 3 runs: it sets the held-out floors from their spread, and is not for tuning")
    if release_mode(o):
        given = ["--" + k.replace("_", "-") for k in RELEASE_REFUSES if getattr(o, k)]
        if given:
            ap.error("a release runs every case of both sets as defined, with the build as it is: %s cannot be "
                     "given" % ", ".join(given))
        if not o.no_build:
            o.build = True   # a release always builds, unless --no-build: then a stale build stops it
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
    stop = release_problems(o)
    if stop:   # before an hour of live runs that could not be judged
        for line in stop:
            print("release cannot start: " + line)
        return 1
    return execute(chosen, why, notes, advice, o)


# ---------------------------------------------------------------- the runner's own test
def LIVE_ORDER(names):
    """NAMES with the gate, in run order (GROUPS' order: the gate after the live sets, the held-out run last)."""
    return [g["name"] for g in GROUPS if g["name"] in set(names) | {"gate"}]


def self_test():
    global RUN_COMMAND, ROOT, changed_paths, diff_reader, stale_note, need_path
    failures, counted = [], [0]

    def check(what, ok, got=None):
        counted[0] += 1
        if not ok:
            failures.append(what + ("" if got is None else ": got %r" % (got,)))

    def groups(*paths):
        return groups_for_paths(list(paths))[0]

    # The path map.
    check("hue driver", groups("src/smarthome/drivers/hue.cpp") == ["unit", "hue", "sim-build"], groups("src/smarthome/drivers/hue.cpp"))
    check("tapo transport", groups("src/smarthome/tapo.cpp") == ["unit", "tapo", "sim-build"])
    shared = groups("src/smarthome/control.cpp")
    check("shared smart-home layer", set(shared) == {"unit", "loop", "sim-build"} | set(FAMILIES), shared)
    check("smart-home tool", "firetv" in groups("src/smarthome/tool.cpp"))
    check("ui: compiled, then the screens", groups("src/ui/app.cpp") == ["unit", "sim-build", "screens"], groups("src/ui/app.cpp"))
    check("LVGL's settings: the sim build and the screens", groups("src/lv_conf.h") == ["sim-build", "screens"], groups("src/lv_conf.h"))
    check("agent", groups("src/agent/agent.cpp") == ["unit", "tools", "loop", "sim-build"], groups("src/agent/agent.cpp"))
    check("main.cpp", groups("src/main.cpp") == ["unit", "tools", "loop", "sim-build"], groups("src/main.cpp"))
    for path in ("src/agent/experiment.h", "src/core/radio.cpp", "src/net/http.cpp", "src/platform/camera.cpp",
                 "src/smarthome/home.h", "src/platform/display.cpp", "CMakeLists.txt"):
        check("%s compiles the no-experiments build too" % path, "sim-build" in groups(path), groups(path))
    check("suite templates: the cases and the scorer that reads them", groups("tests/suite/generate.py") == ["cases", "scorer"],
          groups("tests/suite/generate.py"))
    check("from_conversation.py: the loop test runs it", groups("tests/suite/from_conversation.py") == ["cases", "scorer", "loop"],
          groups("tests/suite/from_conversation.py"))
    check("docs only", groups("docs/agent-architecture.html", "README.md", "AGENTS.md") == [],
          groups("docs/agent-architecture.html", "README.md", "AGENTS.md"))
    check("a mock", groups("tests/mocks/kasa/mock_kasa.py") == ["kasa"])
    check("the tapo mock: tapo, and the loop tests that start it", groups("tests/mocks/mock_tapo.py") == ["loop", "tapo"],
          groups("tests/mocks/mock_tapo.py"))
    check("hue's mock files: its unit test reads them", groups("tests/mocks/hue/certs/bridge.pem") == ["unit", "hue"])
    check("firetv's mock files: its unit test reads them", groups("tests/mocks/firetv/mock_firetv.py") == ["unit", "firetv"])
    check("the stand-in web: validators, scorer and tools", groups("tests/mocks/fake_web.py") == ["cases", "scorer", "tools"],
          groups("tests/mocks/fake_web.py"))
    check("a driver's e2e", groups("tests/wiz_e2e.py") == ["wiz"])
    check("a driver's unit test", groups("tests/unit/shelly_test.cpp") == ["unit", "shelly"])
    check("golden shots", groups("tests/golden/shots/01-chat.png") == ["screens"])
    check("this runner", groups("tests/run.py") == ["runner"])
    check("the package", groups("scripts/package-deb.sh", "packaging/postrm", "tests/packaging_test.py") ==
          ["packaging"], groups("scripts/package-deb.sh", "packaging/postrm", "tests/packaging_test.py"))
    check("deployment: no offline test", groups("scripts/deploy.sh") == [], groups("scripts/deploy.sh"))
    check("the gate and the reports", groups("tests/exp_compare.py") == ["runner"] and groups("tests/exp_report.py") == ["runner"])
    check("the scorer, and the loop test that reads its parser", groups("tests/agent_eval.py") == ["cases", "scorer", "loop"],
          groups("tests/agent_eval.py"))
    check("the suite's runner", groups("tests/agent_suite.py") == ["cases", "scorer"])
    want = [g["name"] for g in GROUPS if g["name"] in set(PROFILES["quick"][1]) | {"sim-build"}]
    check("the build", groups("CMakeLists.txt") == want, groups("CMakeLists.txt"))
    check("an unknown path", groups("somewhere/new.txt") == PROFILES["quick"][1])
    check("bytecode", groups("tests/__pycache__/run.cpython-314.pyc") == [])
    check("several paths add up", groups("src/ui/app.cpp", "tests/suite/x.py") == ["unit", "cases", "scorer", "sim-build", "screens"],
          groups("src/ui/app.cpp", "tests/suite/x.py"))
    for _, gs, _ in PATH_RULES:
        for g in gs:
            check("rule names group %s" % g, g in BY_NAME or g in ("{family}", QUICK))

    # Model-facing changes: advice, never a live run by itself.
    tags, hit, scored = model_facing_tags(["src/agent/prompt.cpp", "README.md"])
    check("prompt.cpp asks for a sample of everything", tags == "all" and hit == ["src/agent/prompt.cpp"], (tags, hit))
    tags, hit, _ = model_facing_tags(["src/agent/guards.cpp", "src/agent/controls.cpp"])
    check("the loop's guards and controls ask for a sample of everything", tags == "all" and len(hit) == 2, (tags, hit))
    tags, _, _ = model_facing_tags(["src/agent/tools/registry.cpp"], lambda p: "@@ static std::string t_timer(...)\n+  remind")
    check("the tools' registry: the tools its changed lines touch", tags == ["reminder", "timer"], tags)
    tags, hit, _ = model_facing_tags(["src/agent/tools/weather.cpp"], lambda p: "+ a line")
    check("a tool's own file: the tool it is named after", tags == ["weather"] and hit == ["src/agent/tools/weather.cpp"], (tags, hit))
    tags, _, _ = model_facing_tags(["src/agent/tools/common.cpp"], lambda p: "+ a line")
    check("the tools' shared code, naming no tool: a sample of everything", tags == "all", tags)
    check("the tools' interface (tools.h) is not model-facing", model_facing_tags(["src/agent/tools.h"])[1] == [])
    tags, _, scored = model_facing_tags(["src/core/radio.cpp"])
    check("radio.cpp: the scored set's radio cases, no suite tag", tags == [] and scored == ["radio_"], (tags, scored))
    tags, _, scored = model_facing_tags(["src/agent/tools/registry.cpp"], lambda p: "+ the radio station list")
    check("the tools' registry: a radio change asks for the scored set's radio cases", tags == [] and scored == ["radio_"], (tags, scored))
    tags, _, scored = model_facing_tags(["src/agent/tools/radio.cpp"], lambda p: "+ a line")
    check("the radio tool's file asks for the scored set's radio cases", tags == [] and scored == ["radio_"], (tags, scored))
    tags, _, _ = model_facing_tags(["src/smarthome/tool.cpp"], categories={"home", "timer"})
    check("tags only from the suite's own", tags == ["home"], tags)
    check("ui is not model-facing", model_facing_tags(["src/ui/app.cpp"])[1] == [])
    lines = advice_lines((["timer"], ["src/core/timers.cpp", "src/core/radio.cpp"], ["radio_"]), parse([]))
    check("advice names the scored set's radio cases", any("--groups live-scored --scored-only radio_ --repeat 5" in l for l in lines), lines)
    n = live_count("live-scored", parse(["--scored-only", "radio_"]))
    if n is not None:
        check("--scored-only radio_ picks the radio cases", n >= 5, n)
        check("--scored-only goes to the scored set only", "radio_" in eval_command(parse(["--scored-only", "radio_", "--only", "timer-001"]), "/r")
              and "radio_" not in " ".join(suite_command(parse(["--scored-only", "radio_", "--only", "timer-001"]), "/r")))
    saved_git = changed_paths, diff_reader
    changed_paths, diff_reader = (lambda base: ("f" * 40, ["src/core/radio.cpp"])), (lambda fork: (lambda p: ""))
    try:
        o = parse(["--profile", "change", "--live"])
        chosen = choose(o)[0]
        check("change --live on radio.cpp runs the scored set's radio cases five times",
              "live-scored" in chosen and "live-suite" not in chosen and o.scored_only == "radio_" and o.repeat == 5,
              (chosen, o.scored_only, o.repeat))
    finally:
        changed_paths, diff_reader = saved_git

    # The profiles.
    quick = PROFILES["quick"][1]
    check("quick is offline", not set(quick) & set(LIVE) and "gate" not in quick, quick)
    check("quick leaves out firetv", "firetv" not in quick and "unit" in quick and "runner" in quick, quick)
    check("offline has every offline group", set(PROFILES["offline"][1]) == set(OFFLINE) and "firetv" in OFFLINE)
    check("sim-build only from the path map", all("sim-build" not in g for _, g in PROFILES.values() if g))
    rel = PROFILES["release"][1]
    check("release: offline, then the live sets, then the held-out run", rel[:len(OFFLINE)] == OFFLINE and
          rel[len(OFFLINE):] == LIVE + ["held-out"], rel)
    check("the held-out run and the web contract are in no other profile", all("held-out" not in g and "web-contract" not in g
                                                                                for n, (_, g) in PROFILES.items() if g and n != "release"))
    check("the web contract is in no profile at all (it reaches the internet)", all("web-contract" not in g for _, g in PROFILES.values() if g))
    o = parse(["--profile", "release", "--baseline", "b.json"])
    chosen = choose(o)[0]
    check("release with a baseline: the live sets, the gate, then the held-out run", chosen[-4:] == LIVE + ["gate", "held-out"], chosen)
    chosen = choose(parse(["--profile", "release"]))[0]
    check("release always has the gate", chosen[-2:] == ["gate", "held-out"], chosen)
    # The held-out run: both runners with --held-out into files of their own, then the judge on its own.
    o = parse(["--profile", "release", "--baseline", "b.json"])
    choose(o)
    cmds = commands_of(BY_NAME["held-out"], o, "/r")
    check("held-out: both sets on the held-out seed, then the judge", len(cmds) == 3 and all("--held-out" in c for c in cmds) and
          cmds[0][cmds[0].index("--json") + 1] == "/r/eval-heldout.json" and cmds[1][cmds[1].index("--json") + 1] == "/r/suite-heldout.json" and
          "tests/exp_compare.py" in cmds[2] and "/r/heldout.json" in cmds[2] and "--base" not in cmds[2], cmds)
    for argv in (["--groups", "held-out"], ["--groups", "held-out,live-suite"]):
        try:
            with contextlib.redirect_stderr(io.StringIO()):
                choose(parse(argv))
            check("held-out outside the release is refused: %s" % " ".join(argv), False)
        except SystemExit:
            check("held-out outside the release is refused: %s" % " ".join(argv), True)
    o = parse(["--groups", "held-out", "--calibrate", "3"])
    chosen = choose(o)[0]
    cmds = commands_of(BY_NAME["held-out"], o, "/r")
    check("--calibrate 3: the held-out set three times, judged with three repeats and a calibration",
          chosen == ["held-out"] and all(c[c.index("--repeat") + 1] == "3" for c in cmds[:2]) and
          cmds[2][cmds[2].index("--repeats") + 1] == "3" and "--calibrate" in cmds[2], cmds)
    for argv in (["--groups", "live-suite", "--calibrate", "3"], ["--groups", "held-out", "--calibrate", "3", "--tags", "timer"],
                 ["--groups", "held-out", "--calibrate", "1"], ["--groups", "held-out", "--calibrate", "2"]):
        try:
            with contextlib.redirect_stderr(io.StringIO()):
                parse(argv)
            check("--calibrate refused: %s" % " ".join(argv), False)
        except SystemExit:
            check("--calibrate refused: %s" % " ".join(argv), True)
    check("held-out: the key line is the judge's", key_line("held-out scored 9/10 passed\nheld-out: passes\n") == "held-out: passes")
    check("no gate for live runs without a baseline", "gate" not in choose(parse(["--groups", "live-suite"]))[0])
    chosen = choose(parse(["--groups", "hue,unit,quick"]))[0]
    check("groups run in table order, once", chosen == [g for g in OFFLINE if g in set(quick)], chosen)
    check("every command's script is in the tree",
          all(os.path.exists(os.path.join(ROOT, w)) for g in GROUPS if not g.get("file")
              for c in g["cmds"] if not callable(c) for w in c if w.startswith("tests/")))
    for script in ("tests/agent_suite.py", "tests/agent_eval.py"):
        has = '"--validate"' in open(os.path.join(ROOT, script), encoding="utf-8").read()
        cmd = _validate(script)(parse([]), "<run>")
        check("cases group: %s --validate %s" % (script, "runs" if has else "is not in this tree yet"),
              (cmd is not None and cmd[-1] == "--validate") == has, cmd)
    check("a command that has nothing to run is left out", None not in commands_of(BY_NAME["cases"], parse([]), "<run>"))

    # The live commands: --jobs 1 and --trace always, the pass-through options.
    o = parse(["--groups", "live-suite", "--tags", "timer", "--repeat", "5", "--sample", "9"])
    cmd = suite_command(o, "/r")
    check("suite command", cmd[cmd.index("--jobs") + 1] == "1" and "--trace" in cmd and cmd[cmd.index("--tags") + 1] == "timer" and
          cmd[cmd.index("--repeat") + 1] == "5" and cmd[cmd.index("--sample") + 1] == "9", cmd)
    check("eval command traced", "--trace" in eval_command(o, "/r") and "--jobs" in eval_command(o, "/r"))
    os.environ["ZB_EVAL_WRAPPER"], saved = "lockit --x", os.environ.get("ZB_EVAL_WRAPPER")
    try:
        check("ZB_EVAL_WRAPPER prefixes live runs", suite_command(o, "/r")[:2] == ["lockit", "--x"])
    finally:
        if saved is None:
            del os.environ["ZB_EVAL_WRAPPER"]
        else:
            os.environ["ZB_EVAL_WRAPPER"] = saved
    o = parse(["--baseline", "b.json", "--skip-tags", "camera", "--groups", "live-suite"])
    choose(o)
    gate = gate_command(o, "<run>")
    check("gate: only the asked-for skips", gate[gate.index("--skip-tags") + 1] == "camera" and "--not-run" not in gate, gate)
    check("gate: a verdict file, and the sets this run ran required", "--verdict" in gate and
          gate[gate.index("--require") + 1] == "suite" and "--release" not in gate, gate)
    o = parse(["--profile", "release", "--baseline", "b.json"])
    choose(o)
    gate = gate_command(o, "<run>")
    check("gate in a release: both sets and the manifest", "--release" in gate and gate[gate.index("--require") + 1] == "scored,suite", gate)
    # --no-search is kept for old command lines and changes nothing: every live case uses the stand-in web.
    for argv in (["--groups", "live-suite,live-scored", "--tags", "search"], ["--profile", "release", "--baseline", "b.json"]):
        a, b = parse(argv), parse(argv + ["--no-search"])
        choose(a)
        choose(b)
        check("--no-search changes no command (%s)" % " ".join(argv),
              (suite_command(a, "/r"), eval_command(a, "/r"), gate_command(a, "<run>")) ==
              (suite_command(b, "/r"), eval_command(b, "/r"), gate_command(b, "<run>")))
    check("--no-search says it changes nothing", any("changes nothing" in n for n in live_notes(b, LIVE)))

    n = live_count("live-suite", parse(["--only", "timer-001,calc-001"]))
    check("a live subset's size", n in (None, 2), n)
    if n is not None:
        check("a live subset's time", group_secs("live-suite", parse(["--only", "timer-001", "--repeat", "3"])) < 60)

    # What a group inherits: no switch that changes what it tests; the runner's own; --exp for live runs.
    keys = ("ZB_UPDATE_SHOTS", "ZB_UPDATE_SNAPSHOTS", "ZB_UNIT_BUILD_ONLY", "ZB_UNIT_SKIP_SCRIPTS", "ZB_EXP_OFF", "HUE_E2E_ONLY",
            "ZB_FAST_MODE", "ZB_EVAL_TRACE")
    saved_env = {k: os.environ.get(k) for k in keys}
    tmp = tempfile.mkdtemp(prefix="zb-run-selftest-")
    try:
        os.environ.update({k: "1" for k in keys})
        env = group_env(tmp, {"ZB_UNIT_SKIP_SCRIPTS": "1"})
        check("inherited switches are not passed on", not any(k in env for k in keys if k != "ZB_UNIT_SKIP_SCRIPTS"),
              sorted(k for k in keys if k in env))
        check("the runner's own switch is", env.get("ZB_UNIT_SKIP_SCRIPTS") == "1")
        check("fresh state folders", env["XDG_DATA_HOME"].startswith(tmp))
        # W4: no group gets the search key; only live groups get the AI service; proxies at the trap.
        before = {k: os.environ.get(k) for k in ("ZB_SEARCH_KEY", "ZB_API_KEY", "ZB_API_BASE", "NO_PROXY", "https_proxy")}
        os.environ.update(ZB_SEARCH_KEY="sentinel-search", ZB_API_KEY="sentinel-api", ZB_API_BASE="https://ai.example/v1",
                          NO_PROXY="*", https_proxy="http://corp.example:3128")
        offline, live = group_env(tmp, {}, BY_NAME["unit"]), group_env(tmp, {}, BY_NAME["live-suite"])
        contract = group_env(tmp, {}, BY_NAME["web-contract"])
        check("an offline group gets no key, no AI service, and its proxies at the trap",
              not any(k in offline for k in netguard.KEYS) and offline["https_proxy"] == netguard.trap().url and
              offline["NO_PROXY"] == "127.0.0.1,localhost", {k: offline.get(k) for k in netguard.KEYS + ("NO_PROXY",)})
        check("a live group gets the AI service but not the search key, and only its host exempt",
              "ZB_SEARCH_KEY" not in live and live.get("ZB_API_KEY") == "sentinel-api" and live["NO_PROXY"] == "127.0.0.1,localhost,ai.example",
              {k: live.get(k) for k in netguard.KEYS + ("NO_PROXY",)})
        check("the contract group gets no key", not any(k in contract for k in netguard.KEYS))
        for k, v in before.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        for k, v in saved_env.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
    check("--exp", exp_env(parse(["--exp", "ZB_EXP_OFF=say"])) == {"ZB_EXP_OFF": "say"})
    for bad in (["--exp", "ZB_EXP_WEB=http://127.0.0.1:1"], ["--exp", "OTHER=1"], ["--exp", "ZB_EXP_OFF"],
                ["--profile", "release", "--exp", "ZB_EXP_OFF=say"]):
        try:
            with contextlib.redirect_stderr(io.StringIO()):
                parse(bad)
            check("refused: %s" % " ".join(bad), False)
        except SystemExit:
            check("refused: %s" % " ".join(bad), True)

    # The release: refused before it starts without a baseline of both sets recorded by the current runners.
    tmp = tempfile.mkdtemp(prefix="zb-run-selftest-")
    try:
        old = os.path.join(tmp, "old.json")
        with open(old, "w") as f:
            json.dump([{"case": "timer-001", "tags": ["timer"], "problems": [], "turns": []}], f)
        sys.path.insert(0, HERE)
        import exp_compare as ec
        roster = ec.load_roster()

        def baseline(drop=(), **change):
            return [dict({"case": n, "set": s, "problems": [], "turns": [{"answer": "a", "trace": []}], "case_hash": x["hash"],
                          "scorer": roster["scorer"], "trace": True, "outcome": "pass", "unscorable": False,
                          **({"tags": x["tags"]} if s == "suite" else {})}, **change)
                    for s, cases in roster["sets"].items() for n, x in cases.items() if n not in drop]
        good, partial, untraced = (os.path.join(tmp, n + ".json") for n in ("good", "partial", "untraced"))
        for path, recs in ((good, baseline()), (untraced, baseline(trace=False, turns=[{"answer": "a"}])),
                           (partial, baseline(drop=[n for n in roster["sets"]["suite"] if not n.startswith("timer-")]))):
            with open(path, "w") as f:
                json.dump(recs, f)
        p = release_problems(parse(["--profile", "release"]))
        check("a release needs a baseline", any("--baseline is needed" in x for x in p), p)
        p = release_problems(parse(["--profile", "release", "--baseline", old, "--build"]))
        check("a release needs both sets in the baseline", any("no scored results" in x for x in p), p)
        check("a release needs the manifest fields", any("lack case_hash" in x for x in p), p)
        p = release_problems(parse(["--profile", "release", "--baseline", os.path.join(tmp, "none.json"), "--build"]))
        check("a release's baseline must be readable", any("cannot be read" in x for x in p), p)
        check("a full baseline of the cases as defined now", release_problems(parse(["--profile", "release", "--baseline", good])) == [],
              release_problems(parse(["--profile", "release", "--baseline", good])))
        p = release_problems(parse(["--profile", "release", "--baseline", partial]))
        check("a baseline of the timer cases alone cannot start a release", any("suite cases" in x for x in p) and
              any("critical" in x for x in p), p)
        p = release_problems(parse(["--profile", "release", "--baseline", untraced]))
        check("an untraced baseline cannot start a release (its runs are traced)", any("untraced" in x for x in p), p)
        p = release_problems(parse(["--groups", "release", "--baseline", partial]))
        check("--groups release is the same release", any("suite cases" in x for x in p), p)

        # N1: a release runs every case; the filters that would make it partial are refused, both spellings.
        for argv in (["--profile", "release", "--tags", "timer", "--sample", "1", "--skip-tags", "confirm"],
                     ["--groups", "release", "--tags", "timer", "--sample", "1", "--skip-tags", "confirm"],
                     ["--profile", "release", "--only", "timer-001"], ["--groups", "release", "--scored-only", "radio_"],
                     ["--profile", "release", "--fast", "0"]):
            try:
                with contextlib.redirect_stderr(io.StringIO()):
                    parse(argv)
                check("refused: " + " ".join(argv), False)
            except SystemExit:
                check("refused: " + " ".join(argv), True)
        o = parse(["--groups", "release", "--baseline", good])
        chosen = choose(o)[0]
        gate = gate_command(o, "<run>")
        check("--groups release: the release's groups, its gate and its build", chosen == LIVE_ORDER(PROFILES["release"][1]) and
              "--release" in gate and o.build, (chosen, gate))
        check("a release builds unless --no-build", parse(["--profile", "release"]).build and
              not parse(["--profile", "release", "--no-build"]).build)

        # N5: the cli build must be able to run live cases: an experiments build, its guard, its records.
        fake = os.path.join(tmp, "zero_bot")
        with open(fake, "w") as f:
            f.write("#!/bin/sh\necho 'a prompt'\nexit 0\n")
        os.chmod(fake, 0o755)
        check("a build without experiments cannot run live cases", "not an experiments build" in probe_cli(fake), probe_cli(fake))
        saved_root, ROOT = ROOT, os.path.join(tmp, "probe-tree")
        saved_wrap = os.environ.get("ZB_EVAL_WRAPPER")
        try:
            os.makedirs(os.path.join(ROOT, "build", "cli"))
            shutil.copy(fake, os.path.join(ROOT, "build", "cli", "zero_bot"))
            os.environ["ZB_EVAL_WRAPPER"] = "true"
            why = missing(BY_NAME["live-suite"], parse([]), tmp)
            check("live cases refuse a cli build that cannot run them", "cannot run live cases: it is not an experiments build" in why, why)
        finally:
            ROOT = saved_root
            if saved_wrap is None:
                os.environ.pop("ZB_EVAL_WRAPPER", None)
            else:
                os.environ["ZB_EVAL_WRAPPER"] = saved_wrap
        for built, ok in (("cli", True), ("sim", False)):
            path = os.path.join(ROOT, need_path(built))
            if os.path.exists(path):
                check("probe of build/%s" % built, (probe_cli(path) == "") == ok, probe_cli(path))

        # N9: a build is stale when any of its inputs is newer, the bundled libraries and presets included.
        saved_root, ROOT = ROOT, os.path.join(tmp, "tree")
        try:
            for rel in ("build/cli/zero_bot", "src/main.cpp", "third_party/json.hpp", "CMakePresets.json", "src/lv_conf.h"):
                os.makedirs(os.path.dirname(os.path.join(ROOT, rel)), exist_ok=True)
                open(os.path.join(ROOT, rel), "w").close()
            for rel, t in (("src/main.cpp", 100), ("CMakePresets.json", 100), ("src/lv_conf.h", 100), ("build/cli/zero_bot", 200),
                           ("third_party/json.hpp", 300)):
                os.utime(os.path.join(ROOT, rel), (t, t))
            check("a newer bundled library makes the build stale", "third_party/json.hpp" in stale_note("cli"), stale_note("cli"))
            os.utime(os.path.join(ROOT, "third_party/json.hpp"), (100, 100))
            os.utime(os.path.join(ROOT, "CMakePresets.json"), (300, 300))
            check("so do newer presets", "CMakePresets.json" in stale_note("cli"), stale_note("cli"))
            os.utime(os.path.join(ROOT, "CMakePresets.json"), (100, 100))
            check("and nothing newer, nothing stale", stale_note("cli") == "")
            os.utime(os.path.join(ROOT, "src/lv_conf.h"), (300, 300))
            check("LVGL's settings are not the cli build's", stale_note("cli") == "", stale_note("cli"))
            os.makedirs(os.path.join(ROOT, "build", "sim"))
            open(os.path.join(ROOT, "build", "sim", "zero_bot"), "w").close()
            os.utime(os.path.join(ROOT, "build", "sim", "zero_bot"), (200, 200))
            check("but the sim build's", "src/lv_conf.h" in stale_note("sim"), stale_note("sim"))
        finally:
            ROOT = saved_root

        # ZB-084: a release also builds the device binary (cp0) and reads it: its version, cli-only strings, build paths.
        o = parse(["--profile", "release"])
        check("a release builds cp0 too", build_presets(choose(o)[0], o)[-1:] == ["cp0"], build_presets(choose(o)[0], o))
        o = parse(["--profile", "offline", "--build"])
        check("only a release builds cp0", "cp0" not in build_presets(choose(o)[0], o))
        check("the version in CMakeLists.txt", re.fullmatch(r"\d+\.\d+\.\d+", project_version()), project_version())
        saved_root, ROOT = ROOT, os.path.join(tmp, "device-tree")
        saved_run = RUN_COMMAND
        try:
            os.makedirs(os.path.join(ROOT, "build", "cp0"))
            cmakelists, binary = os.path.join(ROOT, "CMakeLists.txt"), os.path.join(ROOT, "build", "cp0", "zero_bot")
            with open(cmakelists, "w") as f:
                f.write("cmake_minimum_required(VERSION 3.20)\nproject(zero_bot VERSION 9.8.7 LANGUAGES C CXX)\n")
            os.utime(cmakelists, (100, 100))

            def stand_in(*words):   # a binary as `strings` reads it: printable runs between other bytes
                with open(binary, "wb") as f:
                    f.write(b"\x7fELF\x02\x01" + b"".join(b"\0" + w.encode() + b"\0\x01\x02" for w in words))
                os.utime(binary, (200, 200))
            stand_in("zero_bot [options]", "zero_bot 9.8.7", "usage")
            check("cp0: the right version and nothing else", device_problems(binary, project_version()) == [] and
                  device_check(set())[0] == "pass", (device_problems(binary, project_version()), device_check(set())))
            for words, want in ((("zero_bot 9.8.6",), "is version 9.8.6, CMakeLists.txt says 9.8.7"),
                                (("zero_bot 9.8.70",), "is version 9.8.70,"), (("hello world",), "is version unknown"),
                                (("zero_bot 9.8.7", "/home/someone/lvgl/src/core.c"), "a path of the build machine")):
                stand_in(*words)
                p = device_problems(binary, project_version())
                check("cp0: %s" % want, len(p) == 1 and want in p[0] and "someone" not in p[0] and device_check(set())[0] == "fail", p)
            for word in DEVICE_FORBIDDEN[:-1]:
                stand_in("zero_bot 9.8.7", "set %s here" % word)
                p = device_problems(binary, project_version())
                check("cp0 holds the cli-only %s" % word, p == ["build/cp0/zero_bot holds the cli-only string %s" % word], p)
            check("cp0: a failed build", device_check({"cp0"}) == ("fail", "not checked: the device build failed"))
            stand_in("zero_bot 9.8.7")
            os.utime(cmakelists, (300, 300))
            check("cp0: a stale build fails", device_check(set())[0] == "fail" and "older than CMakeLists.txt" in device_check(set())[1],
                  device_check(set()))
            nb = ["--profile", "release", "--baseline", good, "--no-build"]
            check("--no-build: a stale cp0 stops a release", any("build/cp0 is older than" in x for x in release_problems(parse(nb))),
                  release_problems(parse(nb)))
            os.utime(cmakelists, (100, 100))
            stand_in("zero_bot 9.8.6", "ZB_EVAL_RECORDS")
            p = release_problems(parse(nb))
            check("--no-build: a cp0 of another version or with cli strings stops a release",
                  "the device build: build/cp0/zero_bot is version 9.8.6, CMakeLists.txt says 9.8.7" in p and
                  "the device build: build/cp0/zero_bot holds the cli-only string ZB_EVAL_RECORDS" in p, p)
            os.remove(binary)
            p = release_problems(parse(nb))
            check("--no-build: a missing cp0 stops a release", "build/cp0/zero_bot is missing (a release builds it without --no-build)" in p, p)
            # The run itself: cp0 is built (commands recorded, nothing started) and checked; a bad binary fails the release.
            calls = []

            def build_bad(cmd, log, *args):
                calls.append(cmd)
                if cmd[-3:] == ["--preset", "cp0", "-j2"]:
                    stand_in("zero_bot 9.8.6")
                return 0
            RUN_COMMAND = build_bad
            with contextlib.redirect_stdout(io.StringIO()) as out:
                rc = main(["--profile", "release", "--baseline", good])
            check("a release builds cp0", any(c[-3:] == ["--preset", "cp0", "-j2"] for c in calls), calls)
            check("a cp0 of another version fails the release", rc == 1 and
                  re.search(r"^FAIL  device .*is version 9\.8\.6", out.getvalue(), re.M), out.getvalue()[-600:])
        finally:
            ROOT, RUN_COMMAND = saved_root, saved_run

        # N8: the report's entry point, on a saved result with traced turns.
        call = {"ev": "call", "purpose": "main", "role": "main", "ttfb_ms": 800, "done_ms": 1200, "body_bytes": 9000, "attempts": 1,
                "outcome": "ok", "usage": {"prompt_tokens": 3000, "completion_tokens": 20, "prompt_tokens_details": {"cached_tokens": 2800}},
                "usage_asked": True}
        req = {"ev": "request", "first_sentence_ms": 1300, "tools": [{"name": "timer", "outcome": "ok", "ms": 5, "result_bytes": 40}],
               "guards": [{"guard": "claims_action"}], "second_look": "ok"}
        saved = os.path.join(tmp, "saved.json")
        with open(saved, "w") as f:
            json.dump([{"case": "timer-001", "tags": ["timer"], "set": "suite", "outcome": "pass", "unscorable": False, "problems": [],
                        "case_hash": "h", "scorer": "s", "trace": True,
                        "turns": [{"answer": "Timer set.", "first": 1.1, "total": 1.4, "error": "", "tools": [["timer", "{}"]],
                                   "results": ["ok"], "trace": [call, req]}]},
                       {"case": "calc-001", "tags": ["calc"], "set": "suite", "outcome": "fail", "unscorable": False, "problems": ["wrong"],
                        "case_hash": "h2", "scorer": "s", "trace": True,
                        "turns": [{"answer": "", "first": None, "total": None, "error": "timeout", "timed_out": True, "tools": [],
                                   "results": [], "trace": []}]}], f)
        r = subprocess.run([PY, "-B", os.path.join(HERE, "exp_report.py"), saved, "--tags", "--cases", "2"], capture_output=True, text=True,
                           timeout=60)
        check("exp_report.py runs on a saved result", r.returncode == 0 and "## Model calls" in r.stdout and "## Answers" in r.stdout
              and "1/2 (50%)" in r.stdout, (r.returncode, r.stderr[-300:]))

        # N5: CMake refuses experiments in a device or sim configuration.
        cmake = shutil.which("cmake", path=tool_path())
        if cmake:
            r = subprocess.run([cmake, "-S", ROOT, "-B", os.path.join(tmp, "cmake-sim"), "-DZB_CLI_ONLY=OFF", "-DZB_EXPERIMENTS=ON"],
                               capture_output=True, text=True, timeout=120)
            check("CMake: ZB_EXPERIMENTS=ON outside the cli build is an error", r.returncode != 0 and
                  "ZB_EXPERIMENTS=ON is for the cli build only" in r.stdout + r.stderr, (r.returncode, (r.stdout + r.stderr)[-300:]))
        check("not a release: no such check", release_problems(parse(["--groups", "live-suite"])) == [])

        # A live run with failed cases passes only under a gate verdict that covers its set.
        rows = [("live-scored", "ran", "1/2 passed", 1), ("live-suite", "ran", "5/6 passed", 1), ("unit", "pass", "x", 1)]
        with contextlib.redirect_stdout(io.StringIO()):
            r = judged(rows, tmp)
            check("no gate verdict: the failed live runs fail", [x[1] for x in r] == ["fail", "fail", "pass"], r)
            with open(os.path.join(tmp, "gate.json"), "w") as f:
                json.dump({"verdict": "passes", "sets": {"suite": {"compared": 6}, "scored": {"compared": 0}}}, f)
            r = judged(rows, tmp)
            check("a set the gate compared none of fails", [x[1] for x in r] == ["fail", "ran", "pass"], r)
            with open(os.path.join(tmp, "gate.json"), "w") as f:
                json.dump({"verdict": "inconclusive", "sets": {"suite": {"compared": 6}, "scored": {"compared": 2}}}, f)
            r = judged(rows, tmp)
            check("a gate that did not pass: the failed live runs fail", [x[1] for x in r] == ["fail", "fail", "pass"] and
                  "inconclusive" in r[0][2], r)

        # A release runs on fresh builds only: a stale one fails its group (elsewhere it is a warning).
        saved_stale = stale_note, need_path
        stale_note, need_path = (lambda need: "build/cli is older than src/a.cpp"), (lambda need: "tests/run.py")
        group = dict(name="x", needs=[], uses=["cli"], cmds=[], about="x")
        try:
            warned = {}
            r = run_group(group, parse(["--profile", "release"]), tmp, False, warned)
            check("a release fails on a stale build", r[0] == "fail" and "stale build" in r[1] and not warned, r)
            warned = {}
            r = run_group(group, parse(["--groups", "unit"]), tmp, False, warned)
            check("elsewhere a stale build warns", r[0] == "pass" and warned, (r, warned))
        finally:
            stale_note, need_path = saved_stale
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # The summaries.
    check("one count", key_line("ok a\nhue e2e: 193 checks, 0 failed\n") == "hue e2e: 193 checks, 0 failed")
    check("counts add up", key_line("x: 3 checks, 0 failed\ny: 2 passed, 1 failed\n") == "6 checks, 1 failed (2 parts)",
          key_line("x: 3 checks, 0 failed\ny: 2 passed, 1 failed\n"))
    check("screens", key_line("56 screens, 0 differ\n") == "56 screens, 0 differ")
    check("a build", key_line("[1/2] Building x\n[2/2] Linking CXX executable zero_bot\n(exit 0)\n") ==
          "[2/2] Linking CXX executable zero_bot" and key_line("ninja: no work to do.\n(exit 0)\n") == "ninja: no work to do.")
    check("live", key_line("PASS x\n\n2/2 passed (100%) | per request: total median 3.1s\n") == "2/2 passed (100%)")
    check("gate", key_line("## Gate\n\nFAIL  timer -12\ngate: fails\n") == "gate: fails")
    check("cases", key_line("601 cases; tags: a 1\ncases.json is up to date (601 cases)\n")
          == "cases.json is up to date (601 cases)")

    # --plan runs nothing and writes nothing.
    calls = []
    saved_run, RUN_COMMAND = RUN_COMMAND, (lambda cmd, *a, **k: calls.append(cmd) or 0)
    runs = os.path.join(ROOT, "build", "test-runs")
    before = sorted(os.listdir(runs)) if os.path.isdir(runs) else []
    out = io.StringIO()
    try:
        with contextlib.redirect_stdout(out):
            rc = main(["--profile", "offline", "--plan", "--build", "--baseline", "b.json"])
            rc2 = main(["--groups", "live-suite,unit", "--plan", "--only", "timer-001"])
            rc3 = main(["--profile", "release", "--plan"])
            rc4 = main(["--profile", "release"])
    finally:
        RUN_COMMAND = saved_run
    after = sorted(os.listdir(runs)) if os.path.isdir(runs) else []
    check("--plan runs nothing", rc == 0 and rc2 == 0 and rc3 == 0 and not calls, calls)
    check("a release without a baseline stops before running anything", rc4 == 1 and not calls, (rc4, calls))
    check("--plan says why a release cannot start", "release cannot start: --baseline is needed" in out.getvalue())
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
        want = len(commands_of(BY_NAME["runner"], parse([]), "<run>")) + len(commands_of(BY_NAME["cases"], parse([]), "<run>"))
        check("a run starts each command", len(calls) == want and rc == 0, (calls, rc))
        check("a run writes logs and a summary", folder and all(os.path.exists(os.path.join(folder, f))
                                                                 for f in ("plan.txt", "summary.txt", "runner.log")))
        check("a run prints one line a group", re.search(r"^PASS  runner .*\d+ checks, 0 failed", out.getvalue(), re.M),
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
