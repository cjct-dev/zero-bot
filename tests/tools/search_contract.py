#!/usr/bin/env python3
"""Each web search service's contract: what its public documentation says it is asked and answers
(tests/tools/contracts/<kind>.json, a sample of each, with the pages it was read from and when), set beside the
stand-in web's shape of it (SEARCH_SHAPES in tests/mocks/fake_web.py) and the app's own provider
(src/agent/search/providers/<kind>.cpp), for every service in the app's registry (providers/registry.cpp).

By default it is offline and sends nothing. For each service it compares:
  - where it is asked: the address and the method, in the documentation, the stand-in and the app;
  - how the key travels: the header and what comes before the key; the stand-in takes its key that way only;
  - the query: where the documentation puts it, where the stand-in reads it, where the app sends it; and the
    other parameters the app sends, each documented and within its documented range;
  - how an answer is wrapped: every field of the stand-in's answer (one that found something and one that did
    not), and of the documentation's own example, is a documented field of a documented type, and every field
    the app reads is in both; the headers an answer carries, and those the app keeps;
  - how it says "key refused", "spent", "too many at once" and "down": the stand-in's status and body against the
    documented ones, and the service's own codes, where the app reads one (and whether the documentation says it);
  - every field name the app reads in an answer is one the contract says it reads, and the other way round.
A difference the contract already knows (its "known" list, each with why) is reported and passes; any other
fails, as does a known one that no longer happens (take it out). The stand-in's "outage" (a proxy's page) and
"hostile" answers are none of the service's, and are not compared.

--spend KIND --key-file PATH makes exactly one real search, of one fixed, non-personal question (QUERY), with the key
read from the file the owner names (mode 0600; never from the environment), after he types the service's kind at the
terminal. It prints the HTTP status, whether the answer's shape matches the contract, the time taken and the allowance
headers, and nothing else: never the key, never a word of the results. It spends the owner's allowance: only he runs
it, by hand. tests/run.py refuses it in every group and profile (run.py spends), and it refuses to run inside a
test's network boundary (every group's, netguard.child_env) or without a terminal.

Standard library only.

usage: tests/tools/search_contract.py                         the offline comparison (group search-contract)
       tests/tools/search_contract.py --self-test             the comparison itself and --spend's refusals, offline
       tests/tools/search_contract.py --spend KIND --key-file PATH    one real search: the owner only
"""
import email.message
import json
import os
import re
import stat
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(HERE)
sys.dont_write_bytecode = True
for _p in (TESTS, os.path.join(TESTS, "lib"), os.path.join(TESTS, "mocks"), HERE):
    sys.path.insert(0, _p)
import netguard          # noqa: E402  an opener with no proxy that follows no redirect
import search_services   # noqa: E402  the services built in, read from the app's registry and providers
from web_contract import kind as type_of   # noqa: E402  "text", "number", "bool", "object", "list" or "null"

CONTRACTS = os.path.join(HERE, "contracts")
ASKED = "louvre opening hours on sunday"   # a question the stand-in has an answer for (tests/mocks/web/search.json)
QUERY = "how tall is the eiffel tower"     # the one question --spend asks: fixed, about nothing personal
BAD_DAYS = ("refused", "spent", "busy", "down")
ALLOWANCE_HEADER = re.compile(r"^(x-ratelimit-[a-z-]+|retry-after)$", re.I)
FIGURES = re.compile(r"^[0-9;=w, ]{1,80}$")   # what an allowance header's value may be to be printed


# ---------------------------------------------------------------- shapes
def fields_of(doc):
    """{path: {types}} of every value in DOC (an object): keys joined with ".", every item of a list as "[]"."""
    out = {}

    def walk(x, path):
        out.setdefault(path, set()).add(type_of(x))
        if isinstance(x, dict):
            for k, v in x.items(): walk(v, path + "." + k)
        elif isinstance(x, list):
            for v in x: walk(v, path + "[]")
    for k, v in (doc.items() if isinstance(doc, dict) else []):
        walk(v, k)
    return out


def undocumented(doc, fields, what, open_fields=()):
    """Where DOC is not the documented FIELDS ({path: type, "?" when it may be null}): a field the documentation does
    not have, or a value of a type it does not say. What is inside an OPEN_FIELDS object is the service's own (Brave's
    error.meta, "non-standard meta-information"), and is not compared."""
    out = []
    for path, types in sorted(fields_of(doc).items()):
        if any(path.startswith(o + ".") or path.startswith(o + "[]") for o in open_fields):
            continue
        if path not in fields:
            out.append("%s: %s: the documentation has no such field" % (what, path))
            continue
        spec = fields[path]
        for t in sorted(types - ({"null"} if spec.endswith("?") else set()) - {spec.rstrip("?")}):
            out.append("%s: %s: it has %s, the documentation says %s" % (what, path, t, spec))
    return out


def values_at(doc, path):
    """Every value at PATH in DOC ("a.b[].c": each item of the list a.b), none where a part is missing."""
    found = [doc]
    for part in path.split("."):
        name, lists = part.rstrip("[]"), part.count("[]")
        found = [x[name] for x in found if isinstance(x, dict) and name in x]
        for _ in range(lists):
            found = [v for x in found if isinstance(x, list) for v in x]
    return found


def unread(doc, reads, what):
    """The fields the app reads ({path: type}) that DOC lacks, or holds as another type."""
    out = []
    for path, spec in sorted(reads.items()):
        values = values_at(doc, path)
        if not values and not spec.endswith("?") and not path.endswith("[]"):
            out.append("%s: %s: not there (the app reads it)" % (what, path))
        for t in sorted({type_of(v) for v in values} - ({"null"} if spec.endswith("?") else set()) - {spec.rstrip("?")}):
            out.append("%s: %s: it has %s, the app reads %s" % (what, path, t, spec))
    return out


# ---------------------------------------------------------------- the three sides
def contract_of(kind):
    with open(os.path.join(CONTRACTS, kind + ".json"), encoding="utf-8") as f:
        return json.load(f)


def endpoints(contract):
    """The contract's endpoints: the search, and the allowance's when the service has one."""
    return [contract[e] for e in ("search", "allowance") if e in contract]


def headers_with(name, value):
    """A request's headers as the stand-in reads them (by name, in any capitals)."""
    h = email.message.Message()
    h[name] = value
    return h


def stand_in_side(kind, shape, c):
    """Where the stand-in's shape of KIND differs from the documented one (C: the contract's search)."""
    import fake_web
    out = []
    u = urllib.parse.urlsplit(c["url"])
    if (shape["host"], shape["path"], shape["method"]) != (u.hostname, u.path, c["method"]):
        out.append("the stand-in's address: %s %s%s, the documentation %s %s%s" % (shape["method"], shape["host"], shape["path"],
                                                                                c["method"], u.hostname, u.path))
    key = c["key"]
    if not shape["key"](headers_with(key["header"], key["prefix"] + fake_web.SEARCH_KEY)):
        out.append("the stand-in's key: it does not take its key in %s, as the documentation sends it" % key["header"])
    if shape["key"](headers_with("X-Other-Header", key["prefix"] + fake_web.SEARCH_KEY)) or \
            shape["key"](headers_with(key["header"], key["prefix"] + "not-the-stand-in")):
        out.append("the stand-in's key: it takes a key in another header, or another key")
    q, sample = c["query"], c["sample_request"]
    params, body = ({k: [v] for k, v in sample.items()}, None) if q["in"] == "params" else ({}, json.dumps(sample).encode())
    if shape["query"](params, body) != sample[q["name"]]:
        out.append("the stand-in's query: it does not read the documented request's %s" % q["name"])
    answer = c["answer"]
    for label, fx in (("answer", fake_web.FIXTURES["default"]), ("empty answer", {"services": {kind: "empty"}})):
        status, doc, headers, _ = fake_web.search_reply(dict(fx, name="contract"), kind, shape, ASKED, 1)
        if status != answer["status"]:
            out.append("the stand-in's %s: status %d, the documentation %d" % (label, status, answer["status"]))
        out += undocumented(doc, answer["fields"], "the stand-in's " + label)
        if label == "answer":
            out += unread(doc, answer["reads"], "the stand-in's answer")
            out += header_differences(headers, answer.get("headers", {}), "the stand-in's answer")
    for mode in BAD_DAYS:
        status, doc, headers = shape["bad"](mode)
        out += bad_day_differences(mode, status, doc, c)
    return out


def header_differences(headers, documented, what):
    """Headers HEADERS has that the documentation does not, or whose figures are not in the documented form."""
    out, known = [], {k.lower(): v for k, v in documented.items()}
    form = lambda v: [part.strip().isdigit() for part in str(v).split(",")] if ";" not in str(v) else None
    for name, value in sorted(headers.items()):
        if name.lower() not in known:
            out.append("%s: header %s: the documentation has no such header" % (what, name))
        elif form(value) != form(known[name.lower()]):
            out.append("%s: header %s: %r is not in the documented form (%r)" % (what, name, value, known[name.lower()]))
    return out


def bad_day_differences(mode, status, doc, c):
    """Where the stand-in's answer on a bad day (MODE) is not what the documentation says the service sends."""
    out, said = [], c["errors"][mode]
    what = "the stand-in's " + mode
    if status not in said["status"]:
        out.append("%s: status %d, the documentation lists %s" % (what, status, " or ".join(map(str, said["status"])) or "none"))
    if isinstance(doc, dict):
        out += undocumented(doc, c["error_fields"], what, c.get("open_fields", ()))
        if "code" in said:
            got = (doc.get("error") or {}).get("code") if isinstance(doc.get("error"), dict) else None
            if got != said["code"]:
                out.append("%s: code %r, the contract %r" % (what, got, said["code"]))
    out += undocumented(said["body"], c["error_fields"], "the documentation's " + mode, c.get("open_fields", ()))
    return out


def literal(value):
    """VALUE as it is written in the app's C++ (5, true, "basic")."""
    return json.dumps(value)


def app_side(kind, contract, source):
    """Where the app's provider of KIND (SOURCE) asks otherwise than the documentation says."""
    out, c = [], contract["search"]
    documented = {e["url"] for e in endpoints(contract)}
    sent = set(re.findall(r'send\("(https://[^"?]+)', source))
    out += ["the app's address: it sends to %s, which the contract does not describe" % u for u in sorted(sent - documented)]
    out += ["the app's address: it never sends to the documented %s" % u for u in sorted(documented - sent)]
    keys = set(re.findall(r'"([\w-]+): ([^"]*)" \+ service\.key', source))
    for e in endpoints(contract):
        if not any(h.lower() == e["key"]["header"].lower() and p == e["key"]["prefix"] for h, p in keys):
            out.append("the app's key: not sent as %s: %s<key>" % (e["key"]["header"], e["key"]["prefix"]))
    q = c["query"]
    sends_query = ('{"%s", query}' % q["name"]) in source if q["in"] == "body" else \
        re.search(r'[?&]%s=" \+ http::url_encode\((\w+\()?query\)?\)' % re.escape(q["name"]), source)   # fitted(query) too
    if not sends_query:
        out.append("the app's query: not sent as the %s's %s" % (q["in"], q["name"]))
    if q["in"] == "body":
        body = re.search(r"post_body = json\{(.*)\}\.dump", source)
        names = re.findall(r'\{"(\w+)", ', body.group(1)) if body else []
    else:
        names = re.findall(r'[?&](\w+)=', " ".join(re.findall(r'send\("https://[^;]*', source)))
    out += ["the app's parameters: it sends %s, which the contract does not list" % n for n in names
            if n != q["name"] and n not in c["app"]]
    for name, value in c["app"].items():
        written = ('{"%s", %s}' % (name, literal(value))) if q["in"] == "body" else ("%s=%s" % (name, value))
        if written not in source:
            out.append("the app's parameters: %s is not sent as %s" % (name, written))
        out += param_problems(name, value, c["params"])
    out += read_differences(contract, source)
    out += kept_header_differences(c["answer"], source)
    for mode in BAD_DAYS:
        code = c["errors"][mode].get("code") if c["errors"][mode].get("app_reads_code", True) else None
        if code and '"%s"' % code not in source:
            out.append("the app's %s: it does not read the code %s" % (mode, code))
    return out


def kept_header_differences(answer, source):
    """The reply headers the app keeps (Fetch::keep_headers) against those the contract says it reads, and the documented."""
    block = re.search(r"keep_headers = \{([^}]*)\}", source)
    kept = {h.lower() for h in re.findall(r'"([\w-]+)"', block.group(1))} if block else set()
    out = []
    if kept != {h.lower() for h in answer.get("header_reads", [])}:
        out.append("the app's headers: it keeps %s, the contract says it reads %s" % (sorted(kept), answer.get("header_reads", [])))
    documented = {h.lower() for h in answer.get("headers", {})}
    return out + ["the app's headers: it keeps %s, which the documentation does not list" % h for h in sorted(kept - documented)]


def param_problems(name, value, params):
    """Whether the documentation lists the parameter NAME and allows VALUE."""
    if name not in params:
        return ["the app's parameters: %s is not a documented parameter" % name]
    allowed = params[name]
    ok = (value in allowed) if isinstance(allowed, list) else \
        (allowed["from"] <= value <= allowed["to"]) if isinstance(allowed, dict) else type_of(value) == allowed
    return [] if ok else ["the app's parameters: %s=%s is outside the documented %s" % (name, literal(value), json.dumps(allowed))]


def read_differences(contract, source):
    """The field names the app reads in an answer (jget, contains, an index) against those the contract says it reads."""
    reads = {}
    for e in endpoints(contract):
        reads.update(e["answer"]["reads"])
    reads.update(contract["search"].get("error_reads", {}))
    names = {part.rstrip("[]") for path in reads for part in path.split(".")}
    read = set(re.findall(r'jget\(\s*[^,()]+(?:\([^)]*\))?[^,]*,\s*"(\w+)"', source)) | \
        set(re.findall(r'contains\("(\w+)"\)', source)) | set(re.findall(r'\w\["(\w+)"\]', source))
    out = ["the app reads %s, which the contract does not say it reads" % n for n in sorted(read - names)]
    last = {path.split(".")[-1].rstrip("[]") for path in reads}
    out += ["the contract says the app reads %s, which it does not name" % n for n in sorted(last) if '"%s"' % n not in source]
    return out


def documentation_side(contract):
    """Where the documentation's own examples are not its own fields, or lack a field the app reads."""
    out = []
    for e in endpoints(contract):
        a = e["answer"]
        out += undocumented(a["sample"], a["fields"], "the documentation's own example")
        out += unread(a["sample"], a["reads"], "the documentation's own example")
    return out


def differences(kind, contract=None):
    """Every difference for the service KIND (against CONTRACT, else its file), and the contract (None when there is
    none)."""
    import fake_web
    try:
        contract = contract or contract_of(kind)
    except (OSError, ValueError) as e:
        return ["no contract that can be read: %s" % e], None
    out = []
    if contract.get("kind") != kind or not all(s.get("url", "").startswith("https://") for s in contract.get("sources", [])) \
            or not contract.get("sources") or not re.match(r"^\d{4}-\d\d-\d\d$", contract.get("read", "")):
        out.append("the contract does not say its kind, the pages it was read from and when")
    shape = fake_web.SEARCH_SHAPES.get(kind)
    if shape is None:
        out.append("the stand-in has no shape for it")
    else:
        out += stand_in_side(kind, shape, contract["search"])
    out += app_side(kind, contract, search_services.source(kind))
    out += documentation_side(contract)
    return out, contract


def compare(kind, out=print, contract=None):
    """One service: prints each difference (known or not) and the contract's notes; the count of failures."""
    found, contract = differences(kind, contract)
    known = {k["difference"]: k["why"] for k in (contract or {}).get("known", [])}
    failures = 0
    for d in found:
        if d in known:
            out("known %-7s %s (%s)" % (kind, d, known[d]))
        else:
            out("DIFF  %-7s %s" % (kind, d))
            failures += 1
    for d in sorted(set(known) - set(found)):
        out("STALE %-7s %s: no longer so; take it out of the contract's known list" % (kind, d))
        failures += 1
    for n in (contract or {}).get("notes", []):
        out("note  %-7s %s" % (kind, n))
    if not failures:
        out("ok    %-7s address, key, query, parameters, answers and bad days as documented%s" %
            (kind, " (%d known difference%s)" % (len(known), "s" if len(known) > 1 else "") if known else ""))
    return failures


def main_offline():
    services = search_services.registered()
    failures = sum(compare(kind) for kind, _ in services)
    extra = sorted(set(f[:-5] for f in os.listdir(CONTRACTS) if f.endswith(".json")) - {k for k, _ in services})
    for kind in extra:
        print("DIFF  %-7s a contract for a service the app does not have: take it out" % kind)
    failures += len(extra)
    print("search contract: %d services, %d failed" % (len(services), failures))
    return 1 if failures else 0


# ---------------------------------------------------------------- one real search (--spend)
def refusal(kind, key_file, env, is_tty):
    """Why --spend may not run, or ""."""
    if env.get("ZB_EXP_NET") or env.get("ZB_TEST_NETWORK"):   # netguard.child_env's marks: tests/run.py's groups, the tests
        return "refused: this is a test run (inside a test's network boundary); --spend is typed by the owner only"
    if not is_tty():
        return "refused: --spend needs a terminal: the owner types it, and confirms it there"
    if kind not in {k for k, _ in search_services.registered()} or not os.path.exists(os.path.join(CONTRACTS, kind + ".json")):
        return "refused: %r is not a service the app has with a contract" % kind
    if not key_file:
        return "refused: name the file that holds the key (--key-file PATH); the key is never read from the environment"
    try:
        st = os.stat(key_file)
    except OSError:
        return "refused: the key file cannot be read"
    if not stat.S_ISREG(st.st_mode) or st.st_mode & 0o077:
        return "refused: the key file must be a plain file only its owner can read (chmod 600)"
    return ""


def request_for(contract, key, base=None):
    """The one request --spend makes, as the app makes it: (method, url, headers, body). BASE replaces the scheme and
    host (the self-test's local peer)."""
    c = contract["search"]
    url, q = c["url"], c["query"]
    if base:
        url = base + urllib.parse.urlsplit(url).path
    headers = {"User-Agent": "zero-bot-contract-check/1", c["key"]["header"]: c["key"]["prefix"] + key}
    if q["in"] == "params":
        url += "?" + urllib.parse.urlencode(dict({q["name"]: QUERY}, **c["app"]))
        headers["Accept"] = "application/json"
        return c["method"], url, headers, None
    headers["Content-Type"] = "application/json"
    return c["method"], url, headers, json.dumps(dict({q["name"]: QUERY}, **c["app"])).encode()


def spend(kind, key_file, env=None, is_tty=None, ask=None, opener=None, base=None, out=print):
    """One real search of KIND with the key in KEY_FILE: prints the status, whether the shape matches the contract, the
    time and the allowance headers. 0 when the service answered with the documented shape."""
    env = os.environ if env is None else env
    why = refusal(kind, key_file, env, is_tty or sys.stdin.isatty)
    if why:
        out(why)
        return 2
    with open(key_file, encoding="utf-8") as f:
        key = f.read().strip()
    if not key or any(c.isspace() for c in key):
        out("refused: the key file must hold one key on one line")
        return 2
    name = dict(search_services.registered())[kind]
    if (ask or input)("This spends one search of your %s allowance. Type %s to go on: " % (name, kind)).strip() != kind:
        out("not sent")
        return 2
    contract = contract_of(kind)
    method, url, headers, body = request_for(contract, key, base)
    opener = opener or netguard.local_opener()   # no proxy, no redirect followed: the key goes to the service only
    t0 = time.monotonic()
    try:
        with opener.open(urllib.request.Request(url, data=body, headers=headers, method=method), timeout=20) as r:
            status, raw, got = r.status, r.read(512 * 1024), dict(r.headers.items())
    except urllib.error.HTTPError as e:
        status, raw, got = e.code, b"", dict(e.headers.items())
    except (urllib.error.URLError, OSError) as e:
        out("no answer: %s" % type(e).__name__)
        return 1
    ms = int((time.monotonic() - t0) * 1000)
    out("status %d%s" % (status, " (a redirect: not followed)" if 300 <= status < 400 else ""))
    matched = report_shape(contract, status, raw, out)
    out("time %d ms" % ms)
    for h, v in sorted(got.items()):
        if ALLOWANCE_HEADER.match(h):
            out("header %s: %s" % (h, v if FIGURES.match(v) else "(not figures: left out)"))
    return 0 if matched else 1


def report_shape(contract, status, raw, out):
    """Whether a real answer has the contract's shape; prints only field names, never a value."""
    import fake_web
    c = contract["search"]
    meaning = [m for m in BAD_DAYS if status in c["errors"][m]["status"]]
    if status != c["answer"]["status"]:
        out("shape: not an answer (%s)" % ("the documentation's " + " or ".join(meaning) if meaning else "a status the contract does not know"))
        return False
    try:
        doc = json.loads(raw.decode("utf-8"))
    except ValueError:
        out("shape: not JSON")
        return False
    problems = unread(doc, c["answer"]["reads"], "the answer")
    real = fields_of(doc)
    stand = fields_of(fake_web.search_reply(dict(fake_web.FIXTURES["default"], name="contract"), contract["kind"],
                                            fake_web.SEARCH_SHAPES[contract["kind"]], ASKED, 1)[1])
    problems += ["the stand-in's %s: the answer has no such field" % p for p in sorted(stand) if p not in real]
    out("shape: %s" % ("matches the contract" if not problems else "differs: " + "; ".join(problems[:8])))
    unlisted = [p for p in real if p not in c["answer"]["fields"]]
    if unlisted:
        out("shape: %d field%s the documentation does not list (not named here)" % (len(unlisted), "s" if len(unlisted) > 1 else ""))
    return not problems


def main(argv):
    if "--self-test" in argv:
        return self_test()
    if "--spend" in argv:
        i = argv.index("--spend")
        kind = argv[i + 1] if i + 1 < len(argv) else ""
        key_file = argv[argv.index("--key-file") + 1] if "--key-file" in argv and argv.index("--key-file") + 1 < len(argv) else ""
        os.environ.setdefault("RES_OPTIONS", "single-request")
        return spend(kind, key_file)
    if argv:
        print(__doc__.split("usage:")[1].strip())
        return 2
    return main_offline()


# ---------------------------------------------------------------- the self-test
def self_test():
    """The comparison and --spend, offline: shapes compared, each kind of difference found, a known difference that is
    no longer so reported; --spend refused inside a test's boundary, without a terminal, without a key file or
    with one others can read, for a service without a contract, and when the owner does not confirm, sending nothing; and
    once confirmed, exactly one request to a local peer, as documented, its report without the key or a result's words."""
    import copy
    import fake_web
    n, failed = 0, []

    def check(what, ok, got=None):
        nonlocal n
        n += 1
        if not ok: failed.append("%s (%s)" % (what, got) if got is not None else what)
    fields = {"a": "number", "b": "object?", "b.c": "list", "b.c[]": "object", "b.c[].d": "text"}
    check("a shape as documented", undocumented({"a": 1, "b": {"c": [{"d": "x"}]}}, fields, "x") == [])
    check("null where it may be", undocumented({"b": None}, fields, "x") == [])
    check("a field the documentation lacks", undocumented({"z": 1}, fields, "x") == ["x: z: the documentation has no such field"])
    check("a type it does not say", undocumented({"a": "1"}, fields, "x") == ["x: a: it has text, the documentation says number"])
    check("every item of a list", undocumented({"b": {"c": [{"d": "x"}, {"d": 2}]}}, fields, "x") ==
          ["x: b.c[].d: it has number, the documentation says text"])
    check("a field read, missing", unread({"a": 1}, {"b.c": "list"}, "x") == ["x: b.c: not there (the app reads it)"])
    check("a field read, of another type", unread({"b": {"c": [{"d": 3}]}}, {"b.c[].d": "text"}, "x") ==
          ["x: b.c[].d: it has number, the app reads text"])
    check("a field read that may be absent", unread({}, {"web": "object?"}, "x") == [])

    services = search_services.registered()
    check("every service built in has a contract", services and all(os.path.exists(os.path.join(CONTRACTS, k + ".json")) for k, _ in services),
          services)
    for kind, _ in services:
        contract = contract_of(kind)
        source = search_services.source(kind)
        shape = fake_web.SEARCH_SHAPES[kind]
        base = sorted(differences(kind, contract)[0])
        for what, change in (("another address", lambda c: c["search"].update(url=c["search"]["url"] + "x")),
                             ("another key header", lambda c: c["search"]["key"].update(header="X-Elsewhere")),
                             ("another prefix", lambda c: c["search"]["key"].update(prefix="Token ")),
                             ("another method", lambda c: c["search"].update(method="PUT")),
                             ("a parameter out of range", lambda c: c["search"]["app"].update({next(iter(c["search"]["app"])): 999})),
                             ("a field the app reads, not documented", lambda c: c["search"]["answer"]["fields"].pop(
                                 next(iter(c["search"]["answer"]["reads"])))),
                             ("another status for a refused key", lambda c: c["search"]["errors"]["refused"].update(status=[418]))):
            changed = copy.deepcopy(contract)
            change(changed)
            got = sorted(differences(kind, changed)[0])
            check("%s: %s is found" % (kind, what), [d for d in got if d not in base], got[:3])
        check("%s: the app reading a field the contract does not list is found" % kind,
              any("does not say it reads" in d for d in read_differences(contract, source + '\njget(r, "secret_field", 0)')))
        lines = []
        check("%s: as the contract has it, nothing fails" % kind, compare(kind, lines.append) == 0, lines)
        stale = copy.deepcopy(contract)
        stale.setdefault("known", []).append({"difference": "a difference that never happens", "why": "a test"})
        lines = []
        failures = compare(kind, lines.append, stale)
        check("%s: a known difference that no longer happens fails" % kind, failures == 1 and any(l.startswith("STALE") for l in lines), lines)
        if contract.get("known"):
            lines, unknown = [], dict(contract, known=contract["known"][1:])
            check("%s: a difference the contract does not know fails" % kind, compare(kind, lines.append, unknown) == 1 and
                  any(l.startswith("DIFF") for l in lines), lines)
    spend_checks(check)
    for f in failed: print("FAIL  " + f)
    print("search contract self-test: %d checks, %d failed" % (n, len(failed)))
    return 1 if failed else 0


def spend_checks(check):
    """--spend's refusals send nothing; once confirmed, one request to a local peer, reported without the key or a
    result's words."""
    import http.server
    import tempfile
    import threading
    import fake_web
    folder = tempfile.mkdtemp(prefix="zb-contract-")
    secret = "contract-test-key-" + "7" * 12
    good = os.path.join(folder, "key")
    with open(good, "w") as f: f.write(secret + "\n")
    os.chmod(good, 0o600)
    loose = os.path.join(folder, "loose")
    with open(loose, "w") as f: f.write(secret)
    os.chmod(loose, 0o644)

    class Recorder:
        sent = []

        def open(self, *a, **k):
            self.sent.append(a)
            raise AssertionError("a refused --spend sent a request")
    tty, clean = (lambda: True), {"PATH": "/usr/bin:/bin"}
    for what, args in (("inside a test's boundary", dict(env=netguard.child_env(clean))),
                       ("pinned to loopback", dict(env=dict(clean, ZB_EXP_NET="loopback"))),
                       ("on the stand-in network", dict(env=dict(clean, ZB_TEST_NETWORK="1"))),
                       ("without a terminal", dict(is_tty=lambda: False)),
                       ("without a key file", dict(key_file="")),
                       ("with a key file others can read", dict(key_file=loose)),
                       ("with no such key file", dict(key_file=os.path.join(folder, "none"))),
                       ("for a service without a contract", dict(kind="nosuch")),
                       ("when the owner types something else", dict(ask=lambda prompt: "no"))):
        lines, rec = [], Recorder()
        a = dict(dict(kind="tavily", key_file=good, env=clean, is_tty=tty, ask=lambda prompt: "tavily"), **args)
        rc = spend(a.pop("kind"), a.pop("key_file"), opener=rec, out=lines.append, **a)
        check("--spend refused %s, nothing sent" % what, rc == 2 and not rec.sent and lines and secret not in " ".join(lines), (rc, lines))
    # Confirmed: one request to a local peer that answers as the stand-in does; the report has no key and no result's words.
    for kind, _ in search_services.registered():
        shape, seen = fake_web.SEARCH_SHAPES[kind], []
        status, doc, headers, _ = fake_web.search_reply(dict(fake_web.FIXTURES["default"], name="contract"), kind, shape, ASKED, 1)

        class Peer(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def answer(self):
                n = int(self.headers.get("Content-Length") or 0)
                seen.append((self.command, self.path, dict(self.headers.items()), self.rfile.read(n) if n else b""))
                body = json.dumps(doc).encode()
                self.send_response(status)
                for h, v in dict(headers, **{"Content-Type": "application/json", "Content-Length": str(len(body))}).items():
                    self.send_header(h, v)
                self.end_headers()
                self.wfile.write(body)
            do_GET = do_POST = answer
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Peer)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        lines = []
        try:
            rc = spend(kind, good, env=clean, is_tty=tty, ask=lambda prompt: kind, out=lines.append,
                       base="http://127.0.0.1:%d" % srv.server_address[1])
        finally:
            srv.shutdown()
            srv.server_close()
        c = contract_of(kind)["search"]
        text = "\n".join(lines)
        words = [r.get("title") for r in values_at(doc, "results[]") + values_at(doc, "web.results[]") if isinstance(r, dict)]
        check("%s: --spend confirmed sends exactly one request, as documented" % kind, len(seen) == 1 and seen[0][0] == c["method"] and
              seen[0][1].split("?")[0] == urllib.parse.urlsplit(c["url"]).path and
              {k.lower(): v for k, v in seen[0][2].items()}.get(c["key"]["header"].lower()) == c["key"]["prefix"] + secret and
              QUERY in urllib.parse.unquote_plus(seen[0][1] + seen[0][3].decode()), seen)
        check("%s: --spend reports the status, the shape, the time and the allowance headers" % kind, rc == 0 and "status 200" in text and
              "shape: matches the contract" in text and re.search(r"time \d+ ms", text) and
              all(("header %s: %s" % (h, v)) in text for h, v in headers.items() if ALLOWANCE_HEADER.match(h)), lines)
        check("%s: --spend prints neither the key nor a result's words" % kind, secret not in text and words and
              not any(w and w in text for w in words) and ASKED not in text, lines)
    lines = []
    report_shape(contract_of("tavily"), 401, b"", lines.append)
    check("a refusal's status reads as the documented refusal, its body unread", lines == ["shape: not an answer (the documentation's refused)"], lines)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
