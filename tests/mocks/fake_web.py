#!/usr/bin/env python3
"""A stand-in for every web service the assistant's tools read (web search, weather forecasts, place
names, exchange rates, stock prices), for the test sets: made-up but realistic answers, the same for the
same seed, and search without a key. The cli build sends those requests here when ZB_EXP_WEB is set
(src/agent/experiment.h): https://HOST/PATH?Q arrives as BASE/HOST/PATH?Q, and BASE may carry a fixture's
name, http://127.0.0.1:PORT/FIXTURE, so one stand-in serves every case of a run, each with the world it
needs. The live sets' runners (tests/agent_eval.py run_case) send every case here: no test reaches a real
web service, and none spends the owner's search allowance.

It binds 127.0.0.1 on a port the system picks, prints "WEB listening PORT", and exits when its standard
input closes. Standard library only.

usage: fake_web.py [--seed N] [--date YYYY-MM-DD] [--log FILE] [FIXTURES.json]

--seed N   which variant of each answer is served (default 1). Every value, wording, number of results and
           unrelated result is drawn from the seed and what was asked (the place, the currency, the search
           rule), never from the order of requests, so a case sees the same answers on every run with that
           seed. Seeds from HELD_OUT_FROM on are held out: they draw the held-out values and wordings, which
           the everyday seed never shows (tests/run.py's release profile runs one; nothing is tuned on it).
--date D   the run's date, frozen: every date written (forecast days, "yesterday", events, rates) counts from it.
           No value depends on the date (forecasts are made for a fixed season, by days from today), so runs on other
           days see the same answers with their dates moved.
--log FILE one JSON line per request: {"host", "path", "fixture", "status", "key"} ("key" says only whether
           a search carried the stand-in's key, never what it carried).

The data lives beside this file, in web/ (places.json, markets.json, search.json), and is versioned with
VERSION; the runners hash this file and every file in web/ into each case's case_hash, so results made
against other fixtures are never compared.

Fixtures (the path's first part; "default" without one): "default", "events" (an event whose time a
search gives, and one whose time is not announced), "rain59", "rain60", "rainmissing" (tomorrow's rain
chance in York just under, at, and without the threshold), and the edge cases "search_empty",
"search_error", "search_irrelevant", "search_conflict", "search_stale", "search_long", "weather_error",
"weather_partial", "rates_missing". FIXTURES.json adds to or replaces them: {"NAME": FIXTURE}, FIXTURE as
in FIXTURES below.

Search rules (web/search.json): {"id", "match": regex found in the query (any case), "slots": {NAME:
[VALUE, ...]}, "held_out": {NAME: [VALUE, ...]}, "answer": [TEMPLATE, ...], "answer_held_out": [...],
"results": [{"title", "url", "content": TEXT or [TEXT, ...]}, ...], "no_answer": chance that the summary is
empty}. A VALUE is "shown|other|ways" (the text uses the first; a check accepts any); templates take
{SLOT}, {day:+N} (that day's date), {year}, {year:-1}, {date:EVENT} and {time:EVENT}. The first result
must hold the facts: the others are picked and ordered by the seed. facts(rule_id, seed) gives the
values a check expects (tests/agent_eval.py chk_served).
"""
import datetime
import hashlib
import json
import math
import os
import random
import re
import sys
import threading
import unicodedata
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

VERSION = "web-4"          # change it with anything that changes what is served
CLIMATE_DAY = 325          # the season every forecast is made for (late November), whatever the date: values never move with the day
HELD_OUT_FROM = 1000       # seeds from here on draw the held-out variants
DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")
SEARCH_KEY = "stand-in"    # the only search key the stand-in takes


def _load(name):
    with open(os.path.join(DATA, name), encoding="utf-8") as f:
        return json.load(f)


PLACES = _load("places.json")
MARKETS = _load("markets.json")
_SEARCH = _load("search.json")
RULES = _SEARCH["rules"]                       # the everyday search answers
NOISE = _SEARCH["noise"]                       # results about something else
RULE_SETS = _SEARCH["fixtures"]                # rules only some fixtures serve (events, conflict, stale, long)
ALL_RULES = {r["id"]: r for r in RULES + [r for rs in RULE_SETS.values() for r in rs]}


def fold(text):
    """Lower case, accents and punctuation gone, single spaces: "Zürich," is "zurich"."""
    text = unicodedata.normalize("NFKD", str(text))
    text = "".join(c for c in text if not unicodedata.combining(c)).lower()
    return " ".join(re.findall(r"[a-z0-9&]+", text))


def held_out(seed):
    return seed >= HELD_OUT_FROM


def rng(seed, *parts):
    """A random source fixed by the seed and what it is for."""
    h = hashlib.sha256(json.dumps([seed] + [str(p) for p in parts]).encode()).digest()
    return random.Random(int.from_bytes(h[:8], "big"))


TODAY = None   # the run's date, frozen (--date): every date the stand-in writes counts from it


def today():
    return TODAY or datetime.date.today()


# ---------------------------------------------------------------- events and dates in the text
# Events whose time a search gives, worked out from the day the test runs, so they always lie ahead: the
# first such weekday at least `ahead` days away, at that time in that zone. tests/agent_suite.py's
# event_time check imports event_time to know the answer.
EVENTS = {"product_launch": {"zone": "America/Los_Angeles", "weekday": 1, "ahead": 14, "time": "10:00", "zone_name": "Pacific Time (PT)"}}


def event_time(name, day=None):
    from zoneinfo import ZoneInfo
    e = EVENTS[name]
    d = (day or today()) + datetime.timedelta(days=e["ahead"])
    d += datetime.timedelta(days=(e["weekday"] - d.weekday()) % 7)
    return datetime.datetime.combine(d, datetime.time(int(e["time"][:2]), int(e["time"][3:])), ZoneInfo(e["zone"]))


def fill(text, values=None, day=None):
    """A template's placeholders filled: slots, dates relative to DAY (default today()), years, events."""
    values = values or {}
    day = day or today()

    def one(m):
        kind, arg = m.group(1), m.group(2)
        if arg is None:
            if kind == "year": return str(day.year)
            return shown(values[kind]) if kind in values else m.group(0)
        if kind == "year": return str(day.year + int(arg))
        if kind == "day": return (day + datetime.timedelta(days=int(arg))).strftime("%A %-d %B")
        when = event_time(arg, day)
        if kind == "date": return when.strftime("%A %-d %B %Y")
        if kind == "time":
            return when.strftime("%-I:%M %p").lower().replace("am", "a.m.").replace("pm", "p.m.") + " " + EVENTS[arg]["zone_name"]
        return m.group(0)
    for _ in range(2):   # a slot's value may hold a date
        text = re.sub(r"\{(\w+)(?::([^}]+))?\}", one, text)
    return text


def shown(value):
    """A slot value as the text shows it: "Carlos Alcaraz|Alcaraz" is "Carlos Alcaraz"."""
    return str(value).split("|")[0]


# ---------------------------------------------------------------- search
def pool(rule, slot, seed):
    """The values a slot draws from with this seed: the held-out ones for a held-out seed, never the everyday ones."""
    values = rule.get("held_out", {}).get(slot) if held_out(seed) else rule["slots"][slot]
    if not values:
        raise KeyError("search rule %s, slot %s: no %s values" % (rule["id"], slot, "held-out" if held_out(seed) else "everyday"))
    return values


def facts(rule_id, seed, day=None):
    """The slot values the rule serves with this seed, {NAME: "shown|other|ways"}, each filled ({day:+N}) from DAY
    (the run's date; default today())."""
    rule = ALL_RULES[rule_id]
    return {name: fill(rng(seed, "slot", rule_id, name).choice(pool(rule, name, seed)), day=day) for name in sorted(rule.get("slots", {}))}


def candidates(rule_id, slot, day=None):
    """Every value a slot can take, everyday and held out (filled): what an answer may not give instead of the served one."""
    rule = ALL_RULES[rule_id]
    return [fill(v, day=day) for v in rule["slots"][slot] + rule.get("held_out", {}).get(slot, [])]


def pool_problems():
    """What keeps the held-out set apart: every slot has held-out values that no everyday value equals, and every
    rule has held-out wordings."""
    out = []
    for r in ALL_RULES.values():
        for k, vals in r.get("slots", {}).items():
            held = r.get("held_out", {}).get(k) or []
            if not held: out.append("%s: slot %s has no held-out values" % (r["id"], k))
            elif set(held) & set(vals): out.append("%s: slot %s shares values with the held-out set" % (r["id"], k))
        if not r.get("answer_held_out"): out.append("%s: no held-out wording (answer_held_out)" % r["id"])
    return out


def _result(r, rnd, values, score):
    content = r["content"]
    if isinstance(content, list): content = rnd.choice(content)
    return {"title": fill(r["title"], values), "url": r["url"], "content": fill(content, values),
            "score": round(score, 5), "raw_content": None}


def search_rule(fx, query):
    for rule in fx.get("search", RULES):
        if re.search(rule["match"], query, re.I):
            return rule
    return None


def search(fx, query, seed=1):
    """The answer to a search, as the search service gives it."""
    rnd = rng(seed, "search", fx.get("name", ""), query.lower())
    base = {"query": query, "follow_up_questions": None, "answer": "", "images": [], "results": [],
            "response_time": round(0.6 + rnd.random(), 2), "request_id": "%032x" % rnd.getrandbits(128)}
    mode = fx.get("search_mode")
    if mode == "empty":
        return base
    if mode == "irrelevant":
        picked = rnd.sample(NOISE, 3)
        base["results"] = [_result(r, rnd, {}, 0.4 - i * 0.07) for i, r in enumerate(picked)]
        return base
    rule = search_rule(fx, query)
    if rule is None:
        return base
    values = facts(rule["id"], seed)
    rnd = rng(seed, "search", rule["id"])
    answers = rule.get("answer_held_out") if held_out(seed) else rule.get("answer")
    if not answers:
        raise KeyError("search rule %s: no %s wording" % (rule["id"], "held-out" if held_out(seed) else "everyday"))
    base["answer"] = "" if rnd.random() < rule.get("no_answer", 0.15) else fill(rnd.choice(answers), values)
    pool = rule["results"]
    others = rnd.sample(pool[1:], rnd.randint(min(len(pool) - 1, rule.get("min_results", 1) - 1), len(pool) - 1))
    chosen = [pool[0]] + others
    if NOISE and rnd.random() < rule.get("noise", 0.3): chosen.append(rnd.choice(NOISE))
    rnd.shuffle(chosen)
    if not base["answer"] and chosen[0] in NOISE:   # without a summary the facts come first, as a search ranks them
        chosen.remove(pool[0])
        chosen.insert(0, pool[0])
    base["results"] = [_result(r, rnd, values, 0.92 - i * 0.08 - rnd.random() * 0.05) for i, r in enumerate(chosen)]
    return base


# ---------------------------------------------------------------- places
def place_record(p):
    out = {"id": int(hashlib.sha1((p["name"] + p["country_code"] + p.get("admin1", "")).encode()).hexdigest()[:7], 16),
           "name": p["name"], "latitude": p["latitude"], "longitude": p["longitude"],
           "elevation": float(int(hashlib.sha1(p["name"].encode()).hexdigest()[:3], 16) % 300),
           "feature_code": "PPLC" if p["population"] > 1000000 else "PPL", "country_code": p["country_code"],
           "timezone": p["timezone"], "population": p["population"], "country": p["country"]}
    if p.get("admin1"): out["admin1"] = p["admin1"]
    return out


def places_named(fx, name, count):
    """Places the geocoder would list for a name: whole names (and other names) first, in the gazetteer's
    order, then names that begin with it, as the service matches; nothing for "paris france"."""
    q = fold(name)
    if not q: return []
    places = fx.get("places", PLACES)
    exact = [p for p in places if q == fold(p["name"]) or q in [fold(x) for x in p.get("other_names", [])]]
    prefix = [p for p in places if p not in exact and len(q) >= 3 and fold(p["name"]).startswith(q)]
    return [place_record(p) for p in (exact + prefix)[:max(1, count)]]


def zone_at(lat, lon):
    best = min(PLACES, key=lambda p: (p["latitude"] - lat) ** 2 + ((p["longitude"] - lon) * math.cos(math.radians(lat))) ** 2)
    return best["timezone"]


# ---------------------------------------------------------------- forecasts
WEEK = [{"code": 3, "min": 6, "max": 11, "rain": 20}, {"code": 61, "min": 5, "max": 10, "rain": 40},
        {"code": 2, "min": 4, "max": 12, "rain": 10}, {"code": 3, "min": 5, "max": 11, "rain": 30},
        {"code": 80, "min": 6, "max": 10, "rain": 70}, {"code": 1, "min": 3, "max": 9, "rain": 5},
        {"code": 3, "min": 4, "max": 10, "rain": 25}]
NOW = {"code": 3, "temp": 8, "feels": 6, "wind": 14, "humidity": 78}
# Weather codes by kind, with the rain chances that go with them.
SKIES = [((0, 1), (0, 10)), ((2, 3), (5, 35)), ((45,), (10, 30)), ((51, 53, 61), (40, 75)), ((63, 65, 80, 81), (60, 95)),
         ((71, 73), (50, 85)), ((95,), (60, 90))]


def made_up_days(seed, lat, lon, start, n):
    """A plausible forecast for a place and season, drawn from the seed: the same place and day give the same
    weather in every case of a run."""
    days, out = [], []
    doy = CLIMATE_DAY   # a fixed season: the values do not move with the date (paired runs on other days see the same)
    season = math.cos(2 * math.pi * (doy - 200) / 365.0) * (1 if lat >= 0 else -1)
    mean = 29 - 0.35 * abs(lat) + 0.14 * abs(lat) * season   # roughly a year's mean and swing by latitude
    for i in range(n):
        r = rng(seed, "wx", round(lat, 1), round(lon, 1), i)   # by days from today, not by the date
        mid = mean + r.uniform(-4, 4)
        spread = r.uniform(2.5, 6.5)
        codes, rain = r.choices(SKIES, weights=[22, 35, 6, 18, 14, 2, 3])[0]
        if codes[0] == 71 and mid - spread > 1: codes = (61, 63)        # snow only where it is cold enough
        elif codes[0] in (51, 63) and mid - spread < -1: codes = (71, 73)
        out.append({"code": r.choice(codes), "min": round(mid - spread, 1), "max": round(mid + spread, 1),
                    "rain": int(r.uniform(*rain))})
    return out


def made_up_now(seed, lat, lon, day):
    first = made_up_days(seed, lat, lon, day, 1)[0]
    r = rng(seed, "now", round(lat, 1), round(lon, 1))
    temp = round(r.uniform(first["min"], first["max"]), 1)
    wind = round(r.uniform(3, 32), 1)
    return {"code": first["code"], "temp": temp, "feels": round(temp - wind / 12.0 - r.uniform(0, 1.5), 1), "wind": wind,
            "humidity": int(r.uniform(35, 95))}


def forecast(fx, q, seed=1):
    from zoneinfo import ZoneInfo
    days = max(1, min(16, int((q.get("forecast_days") or ["3"])[0])))
    lat, lon = float((q.get("latitude") or ["0"])[0]), float((q.get("longitude") or ["0"])[0])
    zone = zone_at(lat, lon)
    real = datetime.datetime.now(ZoneInfo(zone))
    start = today()   # the run's date: the forecast starts on the user's today, frozen for the run
    now = datetime.datetime.combine(start, real.time().replace(microsecond=0), real.tzinfo)
    plan = fx.get("forecast")
    if plan:
        week, cur = plan.get("days", WEEK), plan.get("now", NOW)
        picked = [week[i % len(week)] for i in range(days)]
    else:
        picked, cur = made_up_days(seed, lat, lon, start, days), made_up_now(seed, lat, lon, start)
    f = (q.get("temperature_unit") or [""])[0] == "fahrenheit"
    mph = (q.get("wind_speed_unit") or [""])[0] == "mph"
    deg = (lambda c: None if c is None else round(c * 9 / 5 + 32, 1)) if f else (lambda c: c)
    speed = (lambda k: round(k / 1.609344, 1)) if mph else (lambda k: k)
    t_unit, w_unit = ("°F" if f else "°C"), ("mp/h" if mph else "km/h")
    r = rng(seed, "sun", round(lat, 1))
    rise, set_ = 7 * 60 + 21 + r.randint(-40, 40), 16 * 60 + 5 + r.randint(-60, 90)
    dates = [(start + datetime.timedelta(days=i)).isoformat() for i in range(days)]
    out = {"latitude": round(lat, 4), "longitude": round(lon, 4), "generationtime_ms": 0.08,
           "utc_offset_seconds": int(now.utcoffset().total_seconds()), "timezone": zone, "timezone_abbreviation": now.strftime("%Z"),
           "elevation": 40.0,
           "current_units": {"time": "iso8601", "interval": "seconds", "temperature_2m": t_unit, "apparent_temperature": t_unit,
                             "weather_code": "wmo code", "wind_speed_10m": w_unit, "relative_humidity_2m": "%"},
           "current": {"time": now.strftime("%Y-%m-%dT%H:") + "%02d" % (now.minute // 15 * 15), "interval": 900,
                       "temperature_2m": deg(cur["temp"]), "apparent_temperature": deg(cur["feels"]), "weather_code": cur["code"],
                       "wind_speed_10m": speed(cur["wind"]), "relative_humidity_2m": cur["humidity"]},
           "daily_units": {"time": "iso8601", "weather_code": "wmo code", "temperature_2m_max": t_unit, "temperature_2m_min": t_unit,
                           "precipitation_probability_max": "%", "sunrise": "iso8601", "sunset": "iso8601"},
           "daily": {"time": dates,
                     "weather_code": [d["code"] for d in picked], "temperature_2m_max": [deg(d["max"]) for d in picked],
                     "temperature_2m_min": [deg(d["min"]) for d in picked],
                     "precipitation_probability_max": [d.get("rain") for d in picked],
                     "sunrise": ["%sT%02d:%02d" % (d, rise // 60, rise % 60) for d in dates],
                     "sunset": ["%sT%02d:%02d" % (d, set_ // 60, set_ % 60) for d in dates]}}
    if fx.get("forecast_mode") == "partial":
        # A forecast with gaps, as a service sends one mid-update: no current conditions, and no figures for
        # the days after tomorrow.
        del out["current"], out["current_units"]
        for k in ("temperature_2m_max", "temperature_2m_min", "precipitation_probability_max", "weather_code"):
            out["daily"][k] = [v if i < 2 else None for i, v in enumerate(out["daily"][k])]
        if len(out["daily"]["temperature_2m_max"]) > 1:   # a one-day request has no tomorrow to blank
            out["daily"]["temperature_2m_max"][1] = None
    return out


# ---------------------------------------------------------------- exchange rates and prices
def usd_rates(seed):
    """Units of each currency per US dollar, each moved by a seeded few per cent."""
    return {c: (v if c == "USD" else v * (1 + rng(seed, "fx", c).uniform(-0.04, 0.04))) for c, v in MARKETS["usd_rates"].items()}


def sig(x, n=6):
    return float("%.*g" % (n, x))


def rates(fx, base, seed=1):
    usd = usd_rates(seed)
    if base not in usd:
        return 200, {"result": "error", "error-type": "unsupported-code"}   # as the real one: 200, and an error inside
    stamp = datetime.datetime.combine(today(), datetime.time(0, 2, 31), datetime.timezone.utc)
    nxt = stamp + datetime.timedelta(days=1)
    drop = set(fx.get("rates_drop", ()))
    return 200, {"result": "success", "time_last_update_unix": int(stamp.timestamp()),
                 "time_last_update_utc": stamp.strftime("%a, %d %b %Y %H:%M:%S +0000"), "time_next_update_unix": int(nxt.timestamp()),
                 "time_next_update_utc": nxt.strftime("%a, %d %b %Y %H:%M:%S +0000"), "time_eol_unix": 0, "base_code": base,
                 "rates": {c: (1 if c == base else sig(v / usd[base])) for c, v in sorted(usd.items()) if c not in drop or c == base}}


def instrument(symbol=None, query=None):
    for i in MARKETS["instruments"]:
        if symbol is not None and i["symbol"] == symbol: return i
        if query is not None and (fold(query) == fold(i["symbol"]) or
                                  any(re.search(r"\b%s\b" % re.escape(fold(n)), fold(query)) for n in i["names"])):
            return i
    return None


def quote_search(q):
    i = instrument(query=q)
    quotes = []
    if i:
        quotes.append({"exchange": i["exchange"], "shortname": i["shortName"], "quoteType": i["quoteType"], "symbol": i["symbol"],
                       "index": "quotes", "score": 32000.0, "typeDisp": i["quoteType"].title(), "longname": i["longName"],
                       "exchDisp": i["fullExchangeName"], "isYahooFinance": True})
    return {"explains": [], "count": len(quotes), "quotes": quotes, "news": [], "nav": [], "lists": [], "researchReports": [],
            "totalTime": 21, "timeTakenForQuotes": 410, "timeTakenForNews": 0, "timeTakenForAlgowatchlist": 400,
            "timeTakenForPredefinedScreener": 400, "timeTakenForCrunchbase": 0, "timeTakenForNav": 400,
            "timeTakenForResearchReports": 0}


def chart(symbol, seed=1):
    i = instrument(symbol=symbol)
    if i is None:
        return 404, {"chart": {"result": None, "error": {"code": "Not Found", "description": "No data found, symbol may be delisted"}}}
    r = rng(seed, "price", symbol)
    price = sig(i["price"] * (1 + r.uniform(-0.15, 0.15)), 6)
    closes = [price]
    for _ in range(5):
        closes.insert(0, sig(closes[0] / (1 + r.uniform(-0.03, 0.03)), 6))
    prev = closes[0]
    lo, hi = sorted((price * (1 - r.uniform(0.002, 0.02)), price * (1 + r.uniform(0.002, 0.02))))
    day0 = datetime.datetime.combine(today(), datetime.time(0, 0), datetime.timezone.utc)
    real = datetime.datetime.now(datetime.timezone.utc)
    when = int(datetime.datetime.combine(today(), real.time(), datetime.timezone.utc).timestamp()) - r.randint(60, 3 * 3600)
    stamps = [int((day0 - datetime.timedelta(days=4 - k)).timestamp()) + 14 * 3600 + 30 * 60 for k in range(5)]
    meta = {"currency": i["currency"], "symbol": i["symbol"], "exchangeName": i["exchange"], "fullExchangeName": i["fullExchangeName"],
            "instrumentType": i["quoteType"], "firstTradeDate": 345479400, "regularMarketTime": when, "hasPrePostMarketData": True,
            "gmtoffset": 0, "timezone": "GMT", "exchangeTimezoneName": "Europe/London", "regularMarketPrice": price,
            "fiftyTwoWeekHigh": sig(price * (1 + r.uniform(0.05, 0.4))), "fiftyTwoWeekLow": sig(price * (1 - r.uniform(0.05, 0.35))),
            "regularMarketDayHigh": sig(hi), "regularMarketDayLow": sig(lo), "regularMarketVolume": r.randint(10 ** 5, 10 ** 8),
            "longName": i["longName"], "shortName": i["shortName"], "chartPreviousClose": prev, "priceHint": 2,
            "dataGranularity": "1d", "range": "5d", "validRanges": ["1d", "5d", "1mo", "3mo", "6mo", "1y", "5y", "max"]}
    quote = {"open": closes[1:], "close": closes[1:], "high": [sig(c * 1.01) for c in closes[1:]], "low": [sig(c * 0.99) for c in closes[1:]],
             "volume": [r.randint(10 ** 5, 10 ** 8) for _ in range(5)]}
    return 200, {"chart": {"result": [{"meta": meta, "timestamp": stamps, "indicators": {"quote": [quote], "adjclose": [{"adjclose": closes[1:]}]}}],
                           "error": None}}


# ---------------------------------------------------------------- the fixtures
def rain_days(today_rain, tomorrow_rain):
    days = [dict(d) for d in WEEK]
    days[0]["rain"], days[1]["rain"] = today_rain, tomorrow_rain
    return {"days": days, "now": NOW}


FIXTURES = {
    "default": {},
    "events": {"search": RULE_SETS["events"] + RULES},
    # The forecast-threshold reminder ("if tomorrow's rain chance in York is at least 60%"): tomorrow just
    # under, at, and without a rain chance; today on the other side, so reading the wrong day shows.
    "rain59": {"forecast": rain_days(85, 59)},
    "rain60": {"forecast": rain_days(10, 60)},
    "rainmissing": {"forecast": rain_days(85, None)},
    # The edges: what a search, a forecast or a rate can be on a bad day.
    "search_empty": {"search_mode": "empty"},
    "search_error": {"fail": {"search": 503}},
    "search_irrelevant": {"search_mode": "irrelevant"},
    "search_conflict": {"search": RULE_SETS["conflict"] + RULES},
    "search_stale": {"search": RULE_SETS["stale"] + RULES},
    "search_long": {"search": RULE_SETS["long"] + RULES},
    "weather_error": {"fail": {"forecast": 503}},
    "weather_partial": {"forecast_mode": "partial"},
    "rates_missing": {"rates_drop": ["GBP"]},
}


class Handler(BaseHTTPRequestHandler):
    seed = 1
    log = None
    lock = threading.Lock()

    def log_message(self, *a):
        pass

    def reply(self, status, body, what):
        data = (body if isinstance(body, str) else json.dumps(body)).encode()
        self.send_response(status)
        self.send_header("Content-Type", "text/html" if isinstance(body, str) else "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        if self.log:
            with self.lock, open(self.log, "a", encoding="utf-8") as f:
                f.write(json.dumps(dict(what, status=status)) + "\n")

    def route(self, body=None):
        url = urllib.parse.urlsplit(self.path)
        parts = url.path.strip("/").split("/")
        fx_name = "default"
        if parts and parts[0] in FIXTURES: fx_name, parts = parts[0], parts[1:]
        fx = dict(FIXTURES[fx_name], name=fx_name)
        host, path = (parts[0], "/" + "/".join(parts[1:])) if parts else ("", "/")
        what = {"host": host, "path": path, "fixture": fx_name}
        q = urllib.parse.parse_qs(url.query)
        fail = fx.get("fail", {})
        if host.startswith("geocoding-api.") and path == "/v1/search":
            found = places_named(fx, (q.get("name") or [""])[0], int((q.get("count") or ["10"])[0]))
            return self.reply(200, {"results": found, "generationtime_ms": 0.6} if found else {"generationtime_ms": 0.3}, what)
        if host.startswith("api.") and path == "/v1/forecast":
            if "forecast" in fail:
                return self.reply(fail["forecast"], "<html><body><h1>%d Service Unavailable</h1></body></html>" % fail["forecast"], what)
            return self.reply(200, forecast(fx, q, self.seed), what)
        if host.startswith("open.er-api.") and path.startswith("/v6/latest/"):
            status, out = rates(fx, path.rsplit("/", 1)[1].upper(), self.seed)
            return self.reply(status, out, what)
        if host.startswith("query1.finance.") and path == "/v1/finance/search":
            return self.reply(200, quote_search((q.get("q") or [""])[0]), what)
        if host.startswith("query1.finance.") and path.startswith("/v8/finance/chart/"):
            status, out = chart(urllib.parse.unquote(path.rsplit("/", 1)[1]), self.seed)
            return self.reply(status, out, what)
        if path == "/search" and body is not None:
            # The stand-in takes only its own key: a real key sent here means a runner let one through.
            what["key"] = "stand-in" if self.headers.get("Authorization", "") == "Bearer " + SEARCH_KEY else "other"
            if what["key"] != "stand-in":
                return self.reply(401, {"detail": {"error": "Unauthorized: the stand-in web takes only the key 'stand-in'"}}, what)
            if "search" in fail:
                return self.reply(fail["search"], {"detail": {"error": "The service is temporarily unavailable."}}, what)
            try: query = str(json.loads(body or b"{}").get("query", ""))
            except ValueError: return self.reply(400, {"detail": {"error": "bad request"}}, what)
            return self.reply(200, search(fx, query, self.seed), what)
        return self.reply(404, {"error": "the stand-in has nothing at " + url.path}, what)

    def do_GET(self):
        self.route()

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        self.route(self.rfile.read(n) if n else b"")


def main(argv):
    args = list(argv)
    while args and args[0].startswith("--"):
        opt = args.pop(0)
        if opt == "--seed": Handler.seed = int(args.pop(0))
        elif opt == "--log": Handler.log = args.pop(0)
        elif opt == "--date":
            global TODAY
            TODAY = datetime.date.fromisoformat(args.pop(0))
        else: sys.exit("unknown option " + opt)
    if args:
        with open(args[0], encoding="utf-8") as f: FIXTURES.update(json.load(f))
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    print("WEB listening %d" % server.server_address[1], flush=True)
    sys.stdin.read()   # until the test closes it
    server.shutdown()


if __name__ == "__main__":
    os.environ.setdefault("PYTHONDONTWRITEBYTECODE", "1")
    main(sys.argv[1:])
