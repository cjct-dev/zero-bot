"""The web search services built into the app, read from its own sources: the list in
src/agent/search/providers/registry.cpp (ZB_SEARCH_PROVIDERS: X(<kind>, "<Name>")), in its order, and each service's
file, providers/<kind>.cpp. The tests that go through every service (sim/tools_cli.py's search checks,
tools/search_contract.py) read them here, so a service added to the app is a service they test. Standard library only.
"""
import os
import re

PROVIDERS = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "src", "agent",
                         "search", "providers")


def registered():
    """(kind, name) of every service built in, in the registry's order."""
    with open(os.path.join(PROVIDERS, "registry.cpp"), encoding="utf-8") as f:
        return re.findall(r'^\s*X\((\w+), "([^"]+)"\)', f.read(), re.M)


def source(kind):
    """The text of the service's own file, providers/<kind>.cpp."""
    with open(os.path.join(PROVIDERS, kind + ".cpp"), encoding="utf-8") as f:
        return f.read()
