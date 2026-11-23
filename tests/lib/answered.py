"""Who answered at a smart-home device's saved address, as the app reads a driver's error: home::who_answered
(src/smarthome/control.cpp; src/smarthome/driver.h says what the words mean). The app looks for the device at a
new address only when nobody, or another device, answered there; an answer that could not be read, an error code
or a refusal is the device's (ZB-108). The words are read from that function, so the drivers' end-to-end scripts
check their errors exactly as the app reads them; a function this cannot read stops the script that asked. On the
stand-in network, where the app never scans, ZB_EXP_SCAN=FILE has each search it would make noted instead
(`searches`). Standard library only.
"""
import os
import re

SOURCE = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "src", "smarthome",
                      "control.cpp")


def _words():
    """{"Another": [...], "Nobody": [...]}: the words of each answer, in the function's order."""
    with open(SOURCE, encoding="utf-8") as f:
        text = f.read()
    start = text.find("Answered who_answered(")
    end = text.find("return Answered::Device;", start)
    if start < 0 or end < 0:
        raise RuntimeError("no home::who_answered in %s" % SOURCE)
    rest, words = text[start:end], {}
    for kind in ("Another", "Nobody"):
        head, sep, rest = rest.partition("return Answered::%s;" % kind)
        words[kind] = re.findall(r'"([^"]+)"', head)
        if not sep or not words[kind]:
            raise RuntimeError("home::who_answered in %s does not read as tests/lib/answered.py expects" % SOURCE)
    return words


WORDS = _words()


def who_answered(err):
    """"another", "nobody" or "device", as the app reads ERR."""
    e = err.lower()
    for kind in ("Another", "Nobody"):
        if any(w in e for w in WORDS[kind]):
            return kind.lower()
    return "device"


def looked_for(err):
    """Whether the app looks for the device at a new address after ERR (a 2.5 s scan of the network)."""
    return who_answered(err) != "device"


def searches(path):
    """How many searches for a device that may have moved the app noted in PATH (ZB_EXP_SCAN)."""
    try:
        with open(path) as f:
            return len(f.read().splitlines())
    except FileNotFoundError:
        return 0
