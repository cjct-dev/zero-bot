#!/usr/bin/env python3
"""The stand-in web's contract with the real services: a handful of direct requests (no model, no app) to the
real weather, place-name, exchange-rate and stock-price services, each set beside the stand-in's answer to the
same request (tests/mocks/fake_web.py), to see that the stand-in's shapes still match the real ones: every key
the stand-in serves is in the real answer with a type that agrees (a number is a number, null fits anything),
lists compared by their first item, and every field the app reads (READS) is in both. It also asks for what is
not there (a place that does not exist, a symbol that does not, a currency that does not), whose shapes the
app relies on too.

Run it on demand, before a release (tests/run.py --groups web-contract), never on a loop: it reaches the
internet. It never calls the web search: search costs the owner's allowance, and its host is not in HOSTS (a
request to anything else is refused before it is made). Standard library only.

usage: tests/web_contract.py [--self-test]       (--self-test: the comparison itself, offline)
"""
import json
import os
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True
import netguard   # noqa: E402  openers that follow no redirect, and a proxy-free one for 127.x
HOSTS = ("geocoding-api.open-meteo.com", "api.open-meteo.com", "open.er-api.com", "query1.finance.yahoo.com")
FORECAST = ("https://api.open-meteo.com/v1/forecast?latitude=51.5085&longitude=-0.1257&timezone=auto&forecast_days=3"
            "&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m,relative_humidity_2m"
            "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset")
# (what, the request as the app makes it (src/agent/tools.cpp), the fields the app reads)
REQUESTS = [
    ("place", "https://geocoding-api.open-meteo.com/v1/search?language=en&count=10&name=London",
     ["results.0.name", "results.0.country", "results.0.country_code", "results.0.admin1", "results.0.timezone",
      "results.0.latitude", "results.0.longitude"]),
    ("no such place", "https://geocoding-api.open-meteo.com/v1/search?language=en&count=1&name=Florbington", []),
    ("forecast", FORECAST,
     ["current.temperature_2m", "current.apparent_temperature", "current.weather_code", "current.wind_speed_10m",
      "current.relative_humidity_2m", "daily.time", "daily.weather_code", "daily.temperature_2m_max", "daily.temperature_2m_min",
      "daily.precipitation_probability_max", "daily.sunrise", "daily.sunset"]),
    ("forecast in fahrenheit", FORECAST + "&temperature_unit=fahrenheit&wind_speed_unit=mph", ["daily.temperature_2m_max"]),
    ("rates", "https://open.er-api.com/v6/latest/EUR", ["result", "rates.GBP", "time_last_update_utc"]),
    ("no such currency", "https://open.er-api.com/v6/latest/XQZ", ["result"]),
    ("stock by name", "https://query1.finance.yahoo.com/v1/finance/search?quotesCount=3&newsCount=0&q=rolls%20royce",
     ["quotes.0.symbol", "quotes.0.longname", "quotes.0.shortname"]),
    ("stock price", "https://query1.finance.yahoo.com/v8/finance/chart/AAPL?range=5d&interval=1d",
     ["chart.result.0.meta.regularMarketPrice", "chart.result.0.meta.chartPreviousClose", "chart.result.0.meta.currency",
      "chart.result.0.meta.symbol", "chart.result.0.meta.longName", "chart.result.0.meta.fullExchangeName",
      "chart.result.0.meta.regularMarketDayHigh", "chart.result.0.meta.regularMarketDayLow", "chart.result.0.meta.fiftyTwoWeekHigh",
      "chart.result.0.meta.fiftyTwoWeekLow", "chart.result.0.meta.regularMarketTime"]),
    ("no such symbol", "https://query1.finance.yahoo.com/v8/finance/chart/ZZQXQ.NOPE?range=5d&interval=1d", ["chart.error"]),
]


def kind(x):
    if x is None: return "null"
    if isinstance(x, bool): return "bool"
    if isinstance(x, (int, float)): return "number"
    if isinstance(x, str): return "text"
    if isinstance(x, list): return "list"
    return "object"


def differences(stand, real, path=""):
    """Where the stand-in's shape is not the real one's: a key the real answer lacks, or a type that disagrees.
    Keys only the real answer has are fine (the stand-in serves what the app needs)."""
    out = []
    ks, kr = kind(stand), kind(real)
    if "null" in (ks, kr): return out
    if ks != kr:
        return ["%s: the stand-in has %s, the real service %s" % (path or "(top)", ks, kr)]
    if ks == "object":
        for k, v in stand.items():
            if k not in real: out.append("%s.%s: the real service has no such key" % (path, k) if path else "%s: the real service has no such key" % k)
            else: out += differences(v, real[k], "%s.%s" % (path, k) if path else k)
    elif ks == "list" and stand and real:
        out += differences(stand[0], real[0], path + ".0")
    return out


def get(doc, dotted):
    for part in dotted.split("."):
        if isinstance(doc, list) and part.isdigit() and int(part) < len(doc): doc = doc[int(part)]
        elif isinstance(doc, dict) and part in doc: doc = doc[part]
        else: raise KeyError(dotted)
    return doc


def http_get(opener, url, headers=None):
    """(status, JSON or None, where a redirect pointed): one GET through OPENER, which follows no redirect; a 3xx
    comes back as its status and its Location, never followed."""
    req = urllib.request.Request(url, headers=dict({"Accept": "application/json", "User-Agent": "zero-bot-contract-check/1"}, **(headers or {})))
    try:
        with opener.open(req, timeout=20) as r:
            return r.status, json.loads(r.read().decode("utf-8")), None
    except urllib.error.HTTPError as e:
        where = e.headers.get("Location") if 300 <= e.code < 400 else None
        try: return e.code, json.loads(e.read().decode("utf-8")), where
        except ValueError: return e.code, None, where


def fetch(url):
    """(status, JSON) of a GET to a real service: only the hosts in HOSTS, never the search, and no redirect
    followed (a 3xx is a failure of the contract, reported with where it pointed)."""
    host = urllib.parse.urlsplit(url).hostname
    if host not in HOSTS or "search" in host.split(".")[0] or "tavily" in url:
        raise SystemExit("refused: %s is not a service this check may call" % host)
    status, doc, where = http_get(netguard.no_redirect_opener(), url)
    if where is not None:
        return status, {"_redirect": where}
    return status, doc


def stand_in():
    p = subprocess.Popen([sys.executable, "-I", os.path.join(HERE, "mocks", "fake_web.py")], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, text=True, env=netguard.mock_env())
    line = p.stdout.readline().split()
    if line[:2] != ["WEB", "listening"]: raise SystemExit("the stand-in web did not start")
    return p, "http://127.0.0.1:%s/default/" % line[2]


def stand_in_fetch(base, url):
    """The stand-in's answer to the request URL would make: straight to 127.0.0.1 (no proxy), no redirect followed."""
    u = urllib.parse.urlsplit(url)
    status, doc, _ = http_get(netguard.local_opener(), base + u.hostname + u.path + ("?" + u.query if u.query else ""))
    return status, doc


def main():
    if "--self-test" in sys.argv[1:]:
        return self_test()
    os.environ.setdefault("RES_OPTIONS", "single-request")
    proc, base = stand_in()
    bad = 0
    try:
        for what, url, reads in REQUESTS:
            s_status, s_doc = stand_in_fetch(base, url)
            r_status, r_doc = fetch(url)
            problems = []
            if isinstance(r_doc, dict) and "_redirect" in r_doc:
                problems.append("the real service redirected (%d) to %s: not followed" % (r_status, r_doc["_redirect"]))
                r_doc = None
            if (s_status == 200) != (r_status == 200):
                problems.append("status: the stand-in %d, the real service %d" % (s_status, r_status))
            if r_doc is None or s_doc is None:
                problems.append("not JSON: the stand-in %s, the real service %s" % (s_doc is not None, r_doc is not None))
            else:
                problems += differences(s_doc, r_doc)
                for field in reads:
                    for side, doc in (("stand-in", s_doc), ("real service", r_doc)):
                        try: get(doc, field)
                        except KeyError: problems.append("%s: the %s does not have it (the app reads it)" % (field, side))
            bad += bool(problems)
            print("%-4s %-22s %s" % ("ok" if not problems else "DIFF", what, "" if not problems else "; ".join(problems[:6])))
    finally:
        proc.stdin.close()
        proc.wait(timeout=10)
    print("web contract: %d checks, %d failed" % (len(REQUESTS), bad))
    return 1 if bad else 0


def self_test():
    """The comparison, offline: shapes that agree, keys the real answer lacks, types that disagree, nulls; and
    that the check refuses the search and every host it does not know."""
    n, failed = 0, []

    def check(what, ok):
        nonlocal n
        n += 1
        if not ok: failed.append(what)
    real = {"a": 1.5, "b": {"c": [{"d": "x", "e": 2}]}, "f": None, "g": "extra"}
    check("a subset agrees", differences({"a": 2, "b": {"c": [{"d": "y"}]}}, real) == [])
    check("a key the real one lacks", differences({"z": 1}, real) == ["z: the real service has no such key"])
    check("a type that disagrees", differences({"a": "1"}, real) == ["a: the stand-in has text, the real service number"])
    check("null fits anything", differences({"f": 3, "a": None}, real) == [])
    check("lists by their first item", differences({"b": {"c": [{"q": 1}]}}, real) == ["b.c.0.q: the real service has no such key"])
    check("a field read", get(real, "b.c.0.e") == 2)
    for url in ("https://api.tavily.com/search", "https://example.com/x", "https://geocoding-api.open-meteo.com.evil.example/v1/search"):
        try:
            fetch(url)
            check("refused: " + url, False)
        except SystemExit:
            check("refused: " + url, True)
    check("no request is for the search", all("search" not in urllib.parse.urlsplit(u).hostname.split(".")[0] for _, u, _ in REQUESTS))
    # A redirect is reported, never followed (W5): a local server that sends a 302 to a second path.
    import http.server
    import threading
    hits = []

    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            hits.append(self.path)
            if self.path == "/start":
                self.send_response(302)
                self.send_header("Location", "http://127.0.0.1:%d/landed" % self.server.server_address[1])
                self.send_header("Content-Length", "0")
                self.end_headers()
            else:
                body = b"{}"
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        status, doc, where = http_get(netguard.no_redirect_opener(), "http://127.0.0.1:%d/start" % srv.server_address[1])
        check("a redirect is reported with where it points, and not followed", status == 302 and where and where.endswith("/landed")
              and hits == ["/start"])
    finally:
        srv.shutdown()
        srv.server_close()
    # The stand-in answers every request the check makes, with every field the app reads, straight to 127.0.0.1
    # whatever proxy the environment names (W5): the trap sees none of it.
    trap = netguard.trap()
    saved = {k: os.environ.get(k) for k in ("http_proxy", "HTTP_PROXY", "no_proxy", "NO_PROXY")}
    os.environ.update(http_proxy=trap.url, HTTP_PROXY=trap.url)
    for k in ("no_proxy", "NO_PROXY"):
        os.environ.pop(k, None)
    before = trap.count()
    proc, base = stand_in()
    try:
        for what, url, reads in REQUESTS:
            status, doc = stand_in_fetch(base, url)
            check("the stand-in answers %s" % what, doc is not None)
            for field in reads:
                try: get(doc, field)
                except KeyError: check("the stand-in's %s has %s" % (what, field), False)
    finally:
        proc.stdin.close()
        proc.wait(timeout=10)
        for k, v in saved.items():
            if v is None: os.environ.pop(k, None)
            else: os.environ[k] = v
    check("the stand-in was reached without the environment's proxy", trap.since(before) == [])
    for f in failed: print("FAIL  " + f)
    print("web contract self-test: %d checks, %d failed" % (n, len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
