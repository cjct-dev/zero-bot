#!/usr/bin/env python3
"""A stand-in for the web services the assistant's tools read (web search, weather forecasts, place
names), for the test sets: canned answers, the same on every run, and search without a key. The cli
build sends those requests here when ZB_EXP_WEB is set (src/agent/experiment.h): https://HOST/PATH?Q
arrives as BASE/HOST/PATH?Q, and BASE may carry a fixture's name, http://127.0.0.1:PORT/FIXTURE, so
one stand-in serves every case of a run, each with the world it needs.

It binds 127.0.0.1 on a port the system picks, prints "WEB listening PORT", and exits when its
standard input closes. Standard library only.

usage: fake_web.py [FIXTURES.json]

FIXTURES.json adds to or replaces the fixtures below: {"NAME": FIXTURE, ...}. A fixture:

  "places":   {"york": {"name": "York", "country": "United Kingdom", "country_code": "GB",
                        "admin1": "England", "latitude": 53.96, "longitude": -1.08}, ...}
              looked up by the name asked for (any case; the first word that names a place will do)
  "forecast": {"days": [DAY, ...]} for today, tomorrow and on; DAY: {"code": 61, "min": 4, "max": 9,
              "rain": 60} ("rain": null leaves the day's rain chance out, as a forecast without one);
              "now": {"code": 3, "temp": 7, "feels": 5, "wind": 12, "humidity": 80}
  "search":   [{"match": "regex", "answer": "summary", "results": [{"title", "url", "content"}]}, ...]
              the first rule whose regex is found in the query (any case) answers; with none, a
              result that says nothing was found
"""
import datetime
import json
import os
import re
import sys
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PLACES = {
    "london": {"name": "London", "country": "United Kingdom", "country_code": "GB", "admin1": "England", "latitude": 51.51, "longitude": -0.13},
    "york": {"name": "York", "country": "United Kingdom", "country_code": "GB", "admin1": "England", "latitude": 53.96, "longitude": -1.08},
    "lisbon": {"name": "Lisbon", "country": "Portugal", "country_code": "PT", "admin1": "Lisbon", "latitude": 38.72, "longitude": -9.14},
    "porto": {"name": "Porto", "country": "Portugal", "country_code": "PT", "admin1": "Porto", "latitude": 41.15, "longitude": -8.61},
    "paris": {"name": "Paris", "country": "France", "country_code": "FR", "admin1": "Île-de-France", "latitude": 48.85, "longitude": 2.35},
    "rome": {"name": "Rome", "country": "Italy", "country_code": "IT", "admin1": "Lazio", "latitude": 41.89, "longitude": 12.48},
    "madrid": {"name": "Madrid", "country": "Spain", "country_code": "ES", "admin1": "Madrid", "latitude": 40.42, "longitude": -3.70},
}
WEEK = [{"code": 3, "min": 6, "max": 11, "rain": 20}, {"code": 61, "min": 5, "max": 10, "rain": 40},
        {"code": 2, "min": 4, "max": 12, "rain": 10}, {"code": 3, "min": 5, "max": 11, "rain": 30},
        {"code": 80, "min": 6, "max": 10, "rain": 70}, {"code": 1, "min": 3, "max": 9, "rain": 5},
        {"code": 3, "min": 4, "max": 10, "rain": 25}]
NOW = {"code": 3, "temp": 8, "feels": 6, "wind": 14, "humidity": 78}
SEARCH = [
    {"match": r"sun ?set", "answer": "Sunset in London today is at 4:05 pm.",
     "results": [{"title": "Sunrise and sunset times, London", "url": "https://example.org/sun/london",
                  "content": "Today in London the sun rises at 7:21 am and sets at 4:05 pm."}]},
]


def rain_days(today, tomorrow):
    days = [dict(d) for d in WEEK]
    days[0]["rain"], days[1]["rain"] = today, tomorrow
    return {"days": days, "now": NOW}


FIXTURES = {
    "default": {"places": PLACES, "forecast": {"days": WEEK, "now": NOW}, "search": SEARCH},
    # The forecast-threshold reminder ("if tomorrow's rain chance in York is at least 60%"): tomorrow
    # just under, at, and without a rain chance; today on the other side, so reading the wrong day shows.
    "rain59": {"places": PLACES, "forecast": rain_days(85, 59), "search": SEARCH},
    "rain60": {"places": PLACES, "forecast": rain_days(10, 60), "search": SEARCH},
    "rainmissing": {"places": PLACES, "forecast": rain_days(85, None), "search": SEARCH},
}


def place(fx, name):
    """The fixture's place for a name asked for ("york", "York, England", "york uk")."""
    places = fx.get("places", {})
    words = re.findall(r"[^\W\d_]+", name.lower())
    for n in range(len(words), 0, -1):
        key = " ".join(words[:n])
        if key in places: return places[key]
    return None


def forecast(fx, q):
    days = int((q.get("forecast_days") or ["3"])[0])
    plan = fx.get("forecast", {})
    week, now = plan.get("days", WEEK), plan.get("now", NOW)
    today = datetime.date.today()
    picked = [week[i % len(week)] for i in range(days)]
    return {"latitude": float((q.get("latitude") or ["0"])[0]), "longitude": float((q.get("longitude") or ["0"])[0]),
            "timezone": "Europe/London",
            "current": {"temperature_2m": now["temp"], "apparent_temperature": now["feels"], "weather_code": now["code"],
                        "wind_speed_10m": now["wind"], "relative_humidity_2m": now["humidity"]},
            "daily": {"time": [(today + datetime.timedelta(days=i)).isoformat() for i in range(days)],
                      "weather_code": [d["code"] for d in picked], "temperature_2m_max": [d["max"] for d in picked],
                      "temperature_2m_min": [d["min"] for d in picked],
                      "precipitation_probability_max": [d.get("rain") for d in picked],
                      "sunrise": ["%sT07:21" % (today + datetime.timedelta(days=i)).isoformat() for i in range(days)],
                      "sunset": ["%sT16:05" % (today + datetime.timedelta(days=i)).isoformat() for i in range(days)]}}


def search(fx, query):
    for rule in fx.get("search", []):
        if re.search(rule["match"], query, re.I):
            return {"query": query, "answer": rule.get("answer", ""), "results": rule.get("results", [])}
    return {"query": query, "answer": "", "results": [{"title": "No results", "url": "https://example.org/none",
                                                        "content": "Nothing was found for this search."}]}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def reply(self, status, body):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def route(self, body=None):
        url = urllib.parse.urlsplit(self.path)
        parts = url.path.strip("/").split("/")
        fx_name = "default"
        if parts and parts[0] in FIXTURES: fx_name, parts = parts[0], parts[1:]
        fx = FIXTURES[fx_name]
        host, path = (parts[0], "/" + "/".join(parts[1:])) if parts else ("", "/")
        q = urllib.parse.parse_qs(url.query)
        if host.startswith("geocoding-api.") and path == "/v1/search":
            p = place(fx, (q.get("name") or [""])[0])
            return self.reply(200, {"results": [p]} if p else {"generationtime_ms": 0.1})
        if host.startswith("api.") and path == "/v1/forecast":
            return self.reply(200, forecast(fx, q))
        if path == "/search" and body is not None:
            try: query = str(json.loads(body or b"{}").get("query", ""))
            except ValueError: return self.reply(400, {"error": "bad request"})
            return self.reply(200, search(fx, query))
        return self.reply(404, {"error": "the stand-in has nothing at " + url.path})

    def do_GET(self):
        self.route()

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        self.route(self.rfile.read(n) if n else b"")


def main():
    if len(sys.argv) > 1:
        with open(sys.argv[1], encoding="utf-8") as f: FIXTURES.update(json.load(f))
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    print("WEB listening %d" % server.server_address[1], flush=True)
    sys.stdin.read()   # until the test closes it
    server.shutdown()


if __name__ == "__main__":
    os.environ.setdefault("PYTHONDONTWRITEBYTECODE", "1")
    main()
