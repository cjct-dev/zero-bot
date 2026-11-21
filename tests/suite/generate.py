#!/usr/bin/env python3
"""Writes tests/suite/cases.json: several hundred spoken-style requests with the tools and checks
they need, for tests/agent_suite.py (which runs them through the scored set's machinery).

Requests are built from templates and lists of places, amounts, names and phrasings; a seeded
random choice picks slot values and the light speech-recogniser slips (fillers, a repeated word, a
dropped article), so the file is the same on every run. Change the templates, run this again, and
read the diff before committing it. `--check` exits non-zero when cases.json is out of date.

Each case: {"name", "tags", "turns", "must", "must_not", "checks", "env", "home", "steer"}, and
"long_ok" and "tried" when it has them.
  turns     what the user says, in order ("/new" starts a new conversation, memories kept)
  must      tools the last turn must call; "a|b" means either; a change must also have worked
  tried     tools the last turn must call whatever came of it: routing, and cases about a failure or
            about a question first (a change waiting for the user's yes has not worked yet)
  must_not  tools it must not call; "*" means none at all
  checks    declarative checks of the last turn, compiled by tests/agent_suite.py (see KINDS there);
            a check with "turn": N looks at turn N instead (0 is the first, -1 the last; "/new" is
            not a turn), so a middle turn can be held to something too
  long_ok   turns (numbered the same way) whose answer may run longer than the usual cap of 420
            characters: analytical and deeper follow-ups, where a long answer is wanted
  env       environment changes for the run (null removes one; "@web/FIXTURE" picks the stand-in web's fixture;
            every case's web reads go to the stand-in, tests/mocks/fake_web.py)
  home      "eval", "tv" or "sockets": a made-up smart home from tests/agent_eval.py; "tv_silent" and
            "tv_closed": its TV, not answering over the network (these cases set env ZB_DRY_RUN null,
            so that the TV on 127.0.0.1 is really asked)
  tags      the category first, then features: asr, lang, lang-xx, uk, us, followup, compound,
            notool, clarify, home, route, search (the stand-in web's search), camera
Dates and times in checks are relative ("tomorrow", "next:4" for the coming Friday, "date:12-03"
for the next 3 December) and are worked out when the case runs, so a confident wrong date fails.

usage: tests/suite/generate.py [--check]
"""
import json
import os
import random
import sys

SEED = 20261118
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "cases.json")
rng = random.Random(SEED)
CASES = []
COUNT = {}

MON, TUE, WED, THU, FRI, SAT, SUN = range(7)


# ------------------------------------------------------------------ building blocks
def case(cat, turns, tags=(), must=(), must_not=(), checks=(), env=None, home=None, steer=None, long_ok=(), tried=(), held_out=None):
    COUNT[cat] = COUNT.get(cat, 0) + 1
    turns = [turns] if isinstance(turns, str) else list(turns)
    tags = [cat] + [t for t in tags if t != cat]
    if any(t.startswith("lang-") for t in tags) and "lang" not in tags: tags.append("lang")
    if len([t for t in turns if t != "/new"]) > 1 and "followup" not in tags and "/new" not in turns: tags.append("followup")
    if home and "home" not in tags: tags.append("home")
    CASES.append({"name": "%s-%03d" % (cat, COUNT[cat]), "tags": tags, "turns": turns, "must": list(must), "must_not": list(must_not),
                  "checks": list(checks), "env": env or {}, "home": home, "steer": steer})
    # A forecast's figures are what the stand-in served: any temperature the answer gives must be one of them.
    if any("weather" in m.split("|") for m in must): CASES[-1]["checks"].append({"kind": "weather_served"})
    if long_ok: CASES[-1]["long_ok"] = list(long_ok)
    # The held-out question forms: asked instead of turns on a held-out seed (tests/agent_eval.py case_turns).
    if held_out: CASES[-1]["held_out_turns"] = [held_out] if isinstance(held_out, str) else list(held_out)
    if tried: CASES[-1]["tried"] = list(tried)


def ans(pattern, what=None): return {"kind": "answer", "re": pattern, "what": what}
def nans(pattern, what): return {"kind": "not_answer", "re": pattern, "what": what}
def arg_has(tool, key, *subs): return {"kind": "arg", "tool": tool, "key": key, "has": list(subs)}
def arg_num(tool, key, value): return {"kind": "arg", "tool": tool, "key": key, "num": value}
def arg_is(tool, key, value): return {"kind": "arg", "tool": tool, "key": key, "is": value}
def timer(seconds, label=None): return {"kind": "timer", "seconds": seconds, "label": label}
def countdown(seconds): return {"kind": "countdown", "seconds": seconds}
def timer_action(action): return {"kind": "timer_action", "action": action}
def at(day, *times, tools="reminder_set|calendar_add", weekly=False):
    return {"kind": "at", "day": day, "times": list(times), "tools": tools, "weekly": weekly}
def yearly(md, year=None): return {"kind": "yearly", "md": md, "year": year}
def number(*values, tol=0.01): return {"kind": "number", "values": list(values), "tol": tol}
def days_until(md, slack=0): return {"kind": "days_until", "md": md, "slack": slack}
def weekday_of(md=None, year=None, date=None): return {"kind": "weekday_of", "md": md, "year": year, "date": date}
def date_in(days): return {"kind": "date_in", "days": days}
def currency(amount=None, frm=None, to=None): return {"kind": "currency", "amount": amount, "from": frm, "to": to}
def home_call(action, targets=None, **values): return {"kind": "home_call", "action": action, "targets": targets, "values": values}
def simple(kind, **kw): return dict({"kind": kind}, **kw)
def max_calls(tool, n): return {"kind": "max_calls", "tool": tool, "n": n}
def max_words(n): return {"kind": "max_words", "n": n}
def on_turn(turn, check): return dict(check, turn=turn)
def every_turn(check): return dict(check, turn="all")       # in no turn may this go wrong
def timers_saved(*seconds): return {"kind": "timers_saved", "lengths": list(seconds)}
def moved(first_day, first_times, then_day, then_times, what, title=None):
    """The whole conversation: one saved at the first time, then exactly one, the same, at the new time;
    its name has every word of title (default what) that says what it is."""
    return {"kind": "moved", "first": [first_day, first_times], "then": [then_day, then_times], "what": what, "title": title or what}
def memory(has, lacks=()): return {"kind": "memory", "has": has, "lacks": list(lacks)}


QUESTION = ans(r"\?", "a question")
CANNOT = r"can'?t|cannot|not able|unable|don'?t have|no way|not something|isn'?t something|not set up|only|no access"
NO_CLAIM = nans(r"\b(i'?ve |i have )(sent|ordered|booked|called|posted|transferred|emailed|messaged|locked|bought)|\b(is|has been) (sent|ordered|booked|locked)\b",
                "claim it was done")


def slip(text):
    """A light speech-recogniser slip that keeps the meaning: a filler, a repeated word, or a lost article."""
    words = text.split()
    kind = rng.choice(["filler", "repeat", "drop"])
    if kind == "drop" and any(w in ("a", "the") for w in words[1:]):
        i = rng.choice([k for k, w in enumerate(words) if k > 0 and w in ("a", "the")])
        return " ".join(words[:i] + words[i + 1:])
    if kind == "repeat" and len(words) > 2:
        i = rng.randrange(0, min(3, len(words) - 1))
        return " ".join(words[:i + 1] + [words[i]] + words[i + 1:])
    i = rng.randrange(1, len(words))
    return " ".join(words[:i] + [rng.choice(["um", "uh", "er", "like"])] + words[i:])


def with_slips(cat, items, share, build):
    """build(text, item) -> case kwargs. Adds an 'asr' copy with a slip for a seeded share of items."""
    for item in items:
        text = item[0]
        kw = build(text, item)
        case(cat, text, **kw)
        if rng.random() < share:
            kw = build(text, item)
            kw["tags"] = list(kw.get("tags", ())) + ["asr"]
            case(cat, slip(text), **kw)


# ------------------------------------------------------------------ no tool needed
CAPITALS = [("portugal", "lisbon"), ("japan", "tokyo"), ("canada", "ottawa"), ("australia", "canberra"), ("kenya", "nairobi"),
            ("norway", "oslo"), ("peru", "lima"), ("egypt", "cairo"), ("new zealand", "wellington"), ("turkey", "ankara")]
CAP_FORMS = ["what is the capital of {c}", "whats the capital of {c}", "which city is the capital of {c}", "capital of {c}",
             "tell me the capital of {c}"]
for country, city in CAPITALS:
    case("notool", rng.choice(CAP_FORMS).format(c=country), tags=["notool"], must_not=["web_search", "look"], checks=[ans(city)])

for text, pattern in [("how many legs does a spider have", r"\b(8|eight)\b"), ("who wrote pride and prejudice", r"austen"),
                      ("whats the chemical symbol for gold", r"\bau\b"), ("what language do they speak in brazil", r"portuguese"),
                      ("who painted the mona lisa", r"leonardo|da vinci"), ("how many players are on a football team", r"\b(11|eleven)\b"),
                      ("what is the boiling point of water in fahrenheit", r"\b212\b"), ("what planet is closest to the sun", r"mercury"),
                      ("how many sides does a hexagon have", r"\b(6|six)\b"), ("what's the largest ocean", r"pacific"),
                      ("what does serendipity mean", r"chance|luck|accident|unexpected|fortunate"),
                      ("who was the first person to walk on the moon", r"armstrong")]:
    case("notool", text, tags=["notool"], must_not=["web_search", "look", "calculator"], checks=[ans(pattern)])

for text in ["thanks thats great", "cheers mate", "thank you so much that's awesome", "good morning", "hello there",
             "how are you doing today", "you're brilliant", "nice one", "ok thank you", "never mind", "that's all for now thanks",
             "tell me a joke", "say something nice", "goodnight"]:
    tags = ["notool"] + (["uk"] if text in ("cheers mate", "nice one") else []) + (["us"] if "awesome" in text else [])
    case("notool", text, tags=tags, must_not=["*"])

for text, pattern in [("how do you say thank you in japanese", r"arigat"), ("whats good night in spanish", r"buenas noches"),
                      ("how do i say where is the toilet in french", r"toilettes"),
                      ("translate i would like a coffee please into italian", r"caff[eè]"), ("how do you say cheers in german", r"prost"),
                      ("how do you say where is the train station in portuguese", r"esta[çc][ãa]o"),
                      ("what's hello in dutch", r"hallo|goedendag|dag")]:
    case("notool", text, tags=["notool", "translate"], must_not=["look", "web_search"], checks=[ans(pattern)])

for text in ["what time is it", "whats the time", "what time is it right now", "have you got the time"]:
    case("notool", text, tags=["notool", "clock"] + (["uk"] if "got" in text else []), must_not=["world_time", "web_search"],
         checks=[simple("local_time")])
for text in ["whats the date today", "what's today's date"]:
    case("notool", text, tags=["notool", "clock"], must_not=["web_search"], checks=[simple("today_date")])
for text in ["what day is it today", "what day of the week is it"]:
    case("notool", text, tags=["notool", "clock"], must_not=["web_search"], checks=[simple("today_weekday")])
case("notool", ["what's the capital of australia", "and of canada"], tags=["notool"], must_not=["web_search"], checks=[ans("ottawa")])
case("notool", ["tell me a joke", "another one"], tags=["notool"], must_not=["*"])
case("notool", ["whats the weather in lisbon tomorrow", "thanks"], tags=["notool"], must_not=["*"])
case("notool", ["how many days until christmas", "great thanks"], tags=["notool"], must_not=["*"])

# ------------------------------------------------------------------ sums (calculator, Python or in words: all right)
CALC = [("whats 15 percent of 80", [12], 0.01, []), ("what is 17 and a half percent of 2340", [409.5], 0.01, []),
        ("split 96 pounds between 4 people", [24], 0.01, ["uk"]), ("whats a 20 percent tip on 45 dollars", [9, 54], 0.01, ["us"]),
        ("how many kilometres is 26.2 miles", [42.16], 0.2, ["uk"]), ("convert 5 miles to kilometers", [8.05], 0.1, ["us"]),
        ("whats 100 fahrenheit in celsius", [37.78], 0.3, []), ("whats 72 fahrenheit in celsius", [22.22], 0.25, []),
        ("whats 20 celsius in fahrenheit", [68], 0.1, []), ("how many pounds is 70 kilos", [154.32], 0.4, []),
        ("how tall is 6 feet in centimetres", [182.88], 0.2, ["uk"]), ("whats 12 times 17", [204], 0.01, []),
        ("whats the square root of 144", [12], 0.01, []), ("whats 2 to the power of 10", [1024], 0.01, []),
        ("how many seconds are there in a day", [86400], 0.01, []), ("how many minutes are in a week", [10080], 0.01, []),
        ("if i drive 300 miles at 60 miles an hour how long will it take", [5], 0.01, []),
        ("whats a third of 250", [83.33], 0.35, []), ("whats 1200 divided by 7", [171.43], 0.05, []),
        ("a pizza costs 18 pounds and there are 4 of us how much each", [4.5], 0.01, ["uk"]),
        ("whats 8 percent sales tax on 250 dollars", [20, 270], 0.01, ["us"]), ("whats vat at 20 percent on 150 pounds", [30, 180], 0.01, ["uk"]),
        ("how many litres is 5 gallons", [18.93, 22.73], 0.15, ["uk", "us"]), ("whats 3/4 of 60", [45], 0.01, []),
        ("whats 250 plus 375 plus 120", [745], 0.01, []), ("i earn 32000 a year how much is that a month", [2666.67], 1.0, []),
        ("whats 45 minus 17 times 2", [11], 0.01, []), ("how much is 12 percent off 80 pounds", [70.4, 9.6], 0.01, ["uk"])]
with_slips("calc", CALC, 0.2, lambda text, item: {"tags": item[3], "checks": [number(*item[1], tol=item[2])]})
case("calc", ["whats 15 percent of 80", "and 20 percent"], tags=["followup"], checks=[number(16)])
case("calc", ["whats 12 times 17", "now divide that by 4"], tags=["followup"], checks=[number(51)])

# ------------------------------------------------------------------ dates (date_info, Python or in words)
case("date", "how many days until christmas", checks=[days_until("12-25")])
case("date", "how many days is it until new years day", checks=[days_until("01-01")])
case("date", "how many days until halloween", checks=[days_until("10-31")])
case("date", "how many days until valentines day", checks=[days_until("02-14")])
case("date", "how many days are left in the year", checks=[days_until("12-31", slack=1)])
case("date", "how many weeks until christmas", checks=[simple("weeks_until", md="12-25")])
case("date", "what day of the week is the 3rd of march next year", tags=["uk"], checks=[weekday_of("03-03", "next")])
case("date", "what day of the week is march 3rd next year", tags=["us"], checks=[weekday_of("03-03", "next")])
case("date", "what day does christmas fall on this year", checks=[weekday_of("12-25", "this")])
case("date", "what day of the week was the 4th of july 1976", checks=[weekday_of(date="1976-07-04")])
case("date", "what day of the week will the 9th of may be next year", checks=[weekday_of("05-09", "next")])
case("date", "whats the date 100 days from now", checks=[date_in(100)])
case("date", "what will the date be in two weeks", checks=[date_in(14)])
case("date", "whats the date in 30 days", checks=[date_in(30)])
case("date", "is 2028 a leap year", checks=[ans(r"\byes\b|2028 is a leap|it is a leap|it is\b", "yes"), nans(r"not a leap|isn'?t a leap", "say no")])
case("date", "how many days are there in february 2028", checks=[ans(r"\b(29|twenty[- ]nine)\b")])
case("date", "how many days until the 1st of april", tags=["uk"], checks=[days_until("04-01")])
case("date", "how many days until april 1st", tags=["us"], checks=[days_until("04-01")])
case("date", "how many days till christmas", tags=["asr"], checks=[days_until("12-25")])
case("date", "um how many days until uh christmas", tags=["asr"], checks=[days_until("12-25")])
case("date", ["how many days until christmas", "and until new year"], tags=["followup"], checks=[days_until("01-01")])
case("date", "whats 15 percent of 80 and how many days until christmas", tags=["compound"], checks=[number(12), days_until("12-25")])

# ------------------------------------------------------------------ timers
TIMERS = [("set a timer for 12 minutes", 720, None, []), ("set a timer for 90 seconds", 90, None, []),
          ("can you set a 20 minute timer", 1200, None, []), ("timer for 3 minutes please", 180, None, []),
          ("start a timer for half an hour", 1800, None, ["uk"]), ("set a timer for a half hour", 1800, None, ["us"]),
          ("set a timer for a quarter of an hour", 900, None, ["uk"]), ("set a timer for a quarter hour", 900, None, ["us"]),
          ("set a timer for an hour and a half", 5400, None, []), ("set a timer for two and a half minutes", 150, None, []),
          ("set a timer for 45 mins", 2700, None, []), ("start a countdown for 10 minutes", 600, None, []),
          ("put a timer on for 25 minutes", 1500, None, ["uk"]), ("set a pasta timer for 11 minutes", 660, "pasta", []),
          ("set a timer for the tea for 4 minutes", 240, "tea", ["uk"]), ("set a 7 minute timer for the eggs", 420, "egg", []),
          ("the pizza needs 12 minutes in the oven", 720, None, []), ("time me for 25 minutes", 1500, None, []),
          ("can you time 5 minutes for me", 300, None, []), ("i want to read for 40 minutes set a timer", 2400, "read", []),
          ("the cake has to bake for 35 minutes", 2100, None, []), ("set a timer for 2 hours", 7200, None, []),
          ("set a timer for 1 hour 15 minutes", 4500, None, []), ("set a timer for 30 seconds", 30, None, []),
          ("i want to study for an hour can you set a timer", 3600, "stud", []), ("the food needs to come out of the oven in 5 mins", 300, None, []),
          ("the eggs need six minutes", 360, None, []), ("give me 10 minutes on the clock", 600, None, ["uk"])]
with_slips("timer", TIMERS, 0.25, lambda text, item: {"tags": item[3], "must": ["timer"], "must_not": ["reminder_set", "calendar_add"],
                                                       "checks": [timer(item[1], item[2])]})
for text, seconds in [("ten minute timer", 600), ("timer 5 minutes", 300), ("set a time for 10 minutes", 600),
                      ("set a timer for too minutes", 120), ("set a timer for ate minutes", 480), ("set a timer for fifteen minute", 900),
                      ("set a timer four 20 minutes", 1200), ("set the timer for 1 5 minutes", 15 * 60)]:
    case("timer", text, tags=["asr"], must=["timer"], must_not=["reminder_set"], checks=[timer(seconds)])
for text, seconds in [("let me know when 10 minutes are up", 600), ("tell me when 5 minutes have passed", 300),
                      ("remind me in 20 minutes to check the oven", 1200), ("remind me in an hour to move the car", 3600),
                      ("remind me in 2 hours to call back", 7200), ("in 15 minutes remind me to take the washing out", 900),
                      ("ping me in 30 minutes", 1800), ("give me a shout in 10 minutes", 600), ("remind me in an hour and a half to take my tablets", 5400)]:
    case("timer", text, tags=["countdown"] + (["us"] if "ping" in text else []) + (["uk"] if "shout" in text or "washing" in text else []),
         must=["timer|reminder_set"], must_not=["calendar_add"], checks=[countdown(seconds)])
for second in ["how long is left on my timer", "how much time is left", "how long have i got left", "is my timer still running"]:
    case("timer", ["set a timer for 10 minutes", second], must=["timer"], checks=[on_turn(0, timer(600)), timer_action("list")]
         + ([simple("time_left")] if "still" not in second else []))
for second in ["cancel that", "stop the timer", "cancel the timer", "never mind cancel it"]:
    case("timer", ["set a timer for 10 minutes", second], must=["timer"], must_not=["reminder_cancel"],
         checks=[on_turn(0, timer(600)), timer_action("cancel"), timers_saved()])
case("timer", ["set a timer for 10 minutes", "add 5 minutes to it"], must=["timer"],
     checks=[on_turn(0, timer(600)), timer_action("add_time"), arg_num("timer", "minutes", 5), timers_saved(900)])
case("timer", ["set a timer for 10 minutes", "give it another 2 minutes"], must=["timer"],
     checks=[on_turn(0, timer(600)), simple("timer_becomes", seconds=720, added=120)])
case("timer", ["set a timer for 10 minutes", "actually make it 15 minutes"], must=["timer"],
     checks=[on_turn(0, timer(600)), simple("timer_becomes", seconds=900, added=300)])
case("timer", ["set a timer for 20 minutes for studying", "and a 5 minute one for the tea", "cancel the tea timer"], must=["timer"],
     checks=[on_turn(0, timer(1200)), on_turn(1, timer(300)), simple("cancelled", want="tea", keep="stud"), timers_saved(1200)])
case("timer", ["i want to study for 45 minutes set a timer", "and set a 10 minute timer for the pasta", "cancel the study timer"],
     must=["timer"], checks=[on_turn(0, timer(2700)), on_turn(1, timer(600)), simple("cancelled", want="stud", keep="pasta"), timers_saved(600)])
case("timer", ["set a 5 minute timer for the eggs", "and another for 12 minutes for the rice", "how long left on the rice"],
     must=["timer"], checks=[on_turn(0, timer(300)), on_turn(1, timer(720)), timer_action("list")])
case("timer", "set two timers one for 5 minutes and one for 20 minutes", tags=["compound"], must=["timer"], checks=[timer(300), timer(1200)])
for text in ["set a timer", "start a timer please", "can you set a timer for me", "i need a timer"]:
    case("timer", text, tags=["clarify"], must_not=["timer", "reminder_set", "calendar_add"],
         checks=[QUESTION, ans(r"how long|minutes|length|long", "a question about the length")])

# ------------------------------------------------------------------ reminders and alarms
REMINDERS = [("remind me at 7 tomorrow morning to call the hotel", "tomorrow", ["07:00"], []),
             ("remind me tomorrow at 9am to email jane", "tomorrow", ["09:00"], []),
             ("remind me on friday at 6pm to buy flowers", "next:%d" % FRI, ["18:00"], []),
             ("remind me on monday at half past eight to ring the garage", "next:%d" % MON, ["08:30", "20:30"], ["uk"]),
             ("wake me up at half six tomorrow", "tomorrow", ["06:30"], ["uk"]),
             ("set an alarm for 6:45 tomorrow morning", "tomorrow", ["06:45"], []),
             ("set an alarm for 7:15 a.m. tomorrow", "tomorrow", ["07:15"], ["us"]),
             ("remind me on the 3rd of december at 10 to renew my passport", "date:12-03", ["10:00", "22:00"], ["uk"]),
             ("remind me on december 3rd at 10 a.m. to renew my passport", "date:12-03", ["10:00"], ["us"]),
             ("remind me on saturday morning at 10 to water the plants", "next:%d" % SAT, ["10:00"], []),
             ("remind me tomorrow at noon to take my pills", "tomorrow", ["12:00"], []),
             ("remind me at quarter to eight tomorrow morning to leave for the train", "tomorrow", ["07:45"], ["uk"]),
             ("remind me sunday at 5pm to call grandma", "next:%d" % SUN, ["17:00"], ["us"]),
             ("set an alarm for 7", "next_clock", ["07:00", "19:00"], []),
             ("wake me at 6 tomorrow", "tomorrow", ["06:00"], []),
             ("remind me next monday at 9am to call the bank", "next+:%d" % MON, ["09:00"], []),
             ("remind me on wednesday at 7:30pm to put the bins out", "next:%d" % WED, ["19:30"], ["uk"]),
             ("can you remind me thursday at 2 in the afternoon to pick up the dry cleaning", "next:%d" % THU, ["14:00"], ["us"])]
with_slips("reminder", REMINDERS, 0.2, lambda text, item: {"tags": item[3], "must": ["reminder_set|calendar_add"], "must_not": ["timer"],
                                                            "checks": [at(item[1], *item[2])]})
for text, day, times in [("remind me at ate tomorrow morning to call the hotel", "tomorrow", ["08:00"]),
                         ("set an alarm for ate tomorrow morning", "tomorrow", ["08:00"]),
                         ("remind me tomorrow 9am dentist", "tomorrow", ["09:00"]),
                         ("alarm seven tomorrow morning", "tomorrow", ["07:00"]),
                         ("remind me friday at uh 6pm to pick up the kids", "next:%d" % FRI, ["18:00"]),
                         ("remind me tomorrow at ten too call the plumber", "tomorrow", ["10:00", "22:00"])]:
    case("reminder", text, tags=["asr"], must=["reminder_set|calendar_add"], checks=[at(day, *times)])
case("reminder", ["remind me tomorrow at 9 to call the bank", "actually make it 10"], must=["reminder_set|calendar_add|reminder_cancel"],
     checks=[moved("tomorrow", ["09:00", "21:00"], "tomorrow", ["10:00"], "reminder", "bank")])
case("reminder", ["remind me tomorrow at 9am to call the bank", "what reminders have i got"], must=["reminder_list"],
     checks=[on_turn(0, at("tomorrow", "09:00"))])
for text in ["set a reminder", "remind me to call mum", "set an alarm", "can you remind me about the dentist"]:
    case("reminder", text, tags=["clarify"], must_not=["reminder_set", "calendar_add", "timer"], checks=[QUESTION])

# ------------------------------------------------------------------ the calendar
EVENTS = [("put dinner with marta in my calendar for friday at 8pm", "next:%d" % FRI, "20:00", []),
          ("add lunch with sam on thursday at 1pm to my calendar", "next:%d" % THU, "13:00", []),
          ("add a dentist appointment on the 3rd of december at 10am", "date:12-03", "10:00", ["uk"]),
          ("schedule a dentist appointment for december 3rd at 10 a.m.", "date:12-03", "10:00", ["us"]),
          ("put a meeting with the bank in the diary for tuesday at 2pm", "next:%d" % TUE, "14:00", ["uk"]),
          ("add parents evening on wednesday at 6pm", "next:%d" % WED, "18:00", ["uk"]),
          ("add soccer practice on saturday at 10am", "next:%d" % SAT, "10:00", ["us"]),
          ("put the school play in my calendar on the 12th of december at 6:30pm", "date:12-12", "18:30", ["uk"]),
          ("add my flight to lisbon on the 3rd of december at 6 40 am", "date:12-03", "06:40", []),
          ("add a haircut on monday at 11am", "next:%d" % MON, "11:00", []),
          ("put a call with the landlord in my calendar for tomorrow at 4pm", "tomorrow", "16:00", []),
          ("add the gas engineer coming on the 2nd of december at 8am", "date:12-02", "08:00", ["uk"]),
          ("book in a team meeting for friday at 9:30am", "next:%d" % FRI, "09:30", []),
          ("add drinks with the team on january 15th at 6pm", "date:01-15", "18:00", ["us"])]
with_slips("calendar", EVENTS, 0.2, lambda text, item: {"tags": item[3], "must": ["calendar_add"],
                                                         "checks": [at(item[1], item[2], tools="calendar_add")]})
for text, order, md in [("put the dentist in my calendar on 3/4 at 10am", "dmy", "04-03"), ("put the dentist in my calendar on 3/4 at 10am", "mdy", "03-04"),
                        ("add the vet on 14/5 at 9am", "dmy", "05-14"), ("add the vet on 5/14 at 9am", "mdy", "05-14"),
                        ("put the garage in my calendar on 2/6 at 8am", "dmy", "06-02"), ("put the garage in my calendar on 2/6 at 8am", "mdy", "02-06")]:
    hhmm = "10:00" if "10am" in text else "09:00" if "9am" in text else "08:00"
    case("calendar", text, tags=["numeric-date", "us" if order == "mdy" else "uk"], must=["calendar_add"], env={"ZB_DATE_ORDER": order},
         checks=[at("date:" + md, hhmm, tools="calendar_add")])
for text, md, year, tags in [("my mums birthday is on the 14th of march can you add it to my calendar", "03-14", None, ["uk"]),
                             ("add my sisters birthday on the 2nd of august", "08-02", None, []),
                             ("add sarahs birthday shes born on 20/11/1990", "11-20", 1990, ["numeric-date"]),
                             ("add moms birthday july 4th", "07-04", None, ["us"]),
                             ("my wedding anniversary is on the 9th of june put it in the calendar", "06-09", None, []),
                             ("put dads birthday in the calendar he was born on the 2nd of june 1955", "06-02", 1955, []),
                             ("remember that my mums birthday is the 14th of march", "03-14", None, [])]:
    case("calendar", text, tags=tags + ["birthday"], must=["calendar_add"], checks=[yearly(md, year)])
for text, wd, hhmm in [("add bin collection to my calendar every tuesday at 7am", TUE, "07:00"),
                       ("add yoga every wednesday at 6pm to my calendar", WED, "18:00"),
                       ("put football training in every thursday at 7pm", THU, "19:00")]:
    case("calendar", text, tags=["repeat"], must=["calendar_add"], checks=[at("next:%d" % wd, hhmm, tools="calendar_add", weekly=True)])
for text in ["whats on my calendar this week", "do i have anything on tomorrow", "what have i got on on friday", "am i free on saturday",
             "when is my next dentist appointment", "whats in my diary for next week"]:
    case("calendar", text, tags=["lookup"] + (["uk"] if "got on" in text or "diary" in text else []), must=["calendar_list"])
case("calendar", ["put dinner with marta in my calendar for friday at 8pm", "actually make it half past eight"], must=["calendar_update|calendar_add"],
     checks=[moved("next:%d" % FRI, ["20:00"], "next:%d" % FRI, ["20:30"], "dinner")])
case("calendar", ["add lunch with sam on thursday at 1pm", "move it to 2"], must=["calendar_update|calendar_add"],
     checks=[moved("next:%d" % THU, ["13:00"], "next:%d" % THU, ["14:00"], "lunch")])
case("calendar", ["add a dentist appointment on the 3rd of december at 10am", "can you move it to the 4th"], must=["calendar_update|calendar_add"],
     checks=[moved("date:12-03", ["10:00"], "date:12-04", ["10:00"], "dentist appointment", "dentist")])
case("calendar", ["add yoga on wednesday at 6pm", "delete that"], must=["calendar_delete"],
     checks=[on_turn(0, at("next:%d" % WED, "18:00", tools="calendar_add")), simple("events_saved", n=0)])
case("calendar", ["add yoga on wednesday at 6pm", "whats on my calendar on wednesday"],
     checks=[on_turn(0, at("next:%d" % WED, "18:00", tools="calendar_add")), ans(r"yoga")])
case("calendar", ["my dads birthday is the 2nd of june 1955 put it in the calendar", "/new", "when is dads birthday and how old will he be"],
     tags=["followup"], must=["calendar_list"], checks=[on_turn(0, yearly("06-02", 1955)), ans(r"june"), simple("age_on", md="06-02", born=1955)])
for text in ["add a meeting with tom to my calendar", "put lunch with anna in my calendar"]:
    case("calendar", text, tags=["clarify"], must_not=["calendar_add"], checks=[QUESTION])
# A day but no time: asking is right, and so is keeping the day as an all-day entry while asking for the time.
case("calendar", "add the dentist on friday", tags=["clarify"],
     checks=[simple("either", of=[[simple("no_call", tool="calendar_add"), QUESTION],
                                  [simple("all_day_on", day="next:%d" % FRI), ans(r"time|when", "a request for the time")]])])

# ------------------------------------------------------------------ memory
for text, sub, tags in [("remember that im allergic to shellfish", ["shellfish"], []), ("please remember my wifes name is anna", ["anna"], []),
                        ("by the way im vegetarian", ["vegetarian"], []), ("dont forget that i take my coffee black", ["coffee", "black"], []),
                        ("remember my locker number is 42", ["42"], []), ("note that my car is a blue golf", ["golf"], []),
                        ("im allergic to peanuts", ["peanut"], []), ("my favourite football team is arsenal", ["arsenal"], ["uk"]),
                        ("my favorite color is green", ["green"], ["us"]), ("remember i parked on level 3", ["3"], []),
                        ("for future reference im gluten free", ["gluten"], [])]:
    case("memory", text, tags=tags, must=["memory_save"], checks=[arg_has("memory_save", "text", *sub)])
for first, question, pattern, saved in [("remember that im allergic to shellfish", "is the prawn curry ok for me to eat", r"shellfish|allerg", r"shellfish"),
                                        ("remember that my car is a blue golf", "what car do i drive", r"golf", r"golf"),
                                        ("remember my locker number is 42", "whats my locker number", r"\b42\b|forty[- ]two", r"\b42\b|forty[- ]two"),
                                        ("by the way im vegetarian", "can you suggest something for dinner", r"vegetarian|meat-free|meatless|veg",
                                         r"vegetarian")]:
    case("memory", [first, "/new", question], tags=["recall"], must_not=["memory_save"], checks=[on_turn(0, memory(saved)), ans(pattern)])
case("memory", ["remember that im vegetarian", "/new", "forget that im vegetarian"], tags=["forget"], must=["memory_delete"],
     checks=[on_turn(0, memory(r"vegetarian")), simple("memory_lacks", re=[r"vegetarian"])])

# ------------------------------------------------------------------ weather
CITIES = ["lisbon", "madrid", "rome", "berlin", "paris", "new york", "chicago", "seattle", "edinburgh", "manchester", "dublin",
          "tokyo", "sydney", "cape town", "toronto", "barcelona", "amsterdam", "vienna", "prague", "boston"]
WEATHER_FORMS = ["whats the weather like in {c}", "whats the weather in {c} tomorrow", "will it rain in {c} tomorrow",
                 "how hot is it in {c} right now", "whats the forecast for {c} this weekend", "is it going to be sunny in {c} on saturday",
                 "do i need an umbrella in {c} today", "how cold will it be in {c} tonight", "whats the temperature in {c}",
                 "whats the weather gonna be like in {c} tomorrow", "is it raining in {c} at the moment"]
for k, c in enumerate(CITIES):
    form = WEATHER_FORMS[k % len(WEATHER_FORMS)]
    tags = ["us"] if "gonna" in form else ["uk"] if "at the moment" in form else []
    case("weather", form.format(c=c), tags=tags, must=["weather"], checks=[arg_has("weather", "location", c)])
for text in ["do i need a coat today", "whats the weather like today", "will it rain tomorrow", "is it cold outside", "do i need an umbrella today"]:
    case("weather", text, tags=["home-city"], must=["weather"], checks=[simple("home_weather")])
for text, c in [("whats the whether in paris", "paris"), ("weather tomorrow madrid", "madrid"), ("whats the whether in paris france", "paris"),
                ("um whats the uh weather like in berlin", "berlin"), ("hows the weather looking in rome rome", "rome"),
                ("what the weather in dublin tomorrow", "dublin")]:
    case("weather", text, tags=["asr"], must=["weather"], checks=[arg_has("weather", "location", c)])
case("weather", ["whats the weather in lisbon tomorrow", "and what about porto"], must=["weather"], checks=[arg_has("weather", "location", "porto")])
case("weather", ["whats the weather like in paris", "and in rome"], must=["weather"], checks=[arg_has("weather", "location", "rom")])
case("weather", ["whats the weather in madrid today", "what about tomorrow"], must=["weather"], checks=[arg_has("weather", "location", "madrid")])
case("weather", "whats the weather like in florbington", tags=["unknown"],
     checks=[nans(r"\d+ ?(degrees|°)", "invent a temperature"), ans(r"find|couldn'?t|could not|not sure|which|where|\?")])

# ------------------------------------------------------------------ money and prices
AMOUNTS = [20, 35, 40, 50, 75, 85, 120, 150, 250, 500, 1000]
for form, frm, to, tags in [("how much is {a} euros in pounds", "EUR", "GBP", []), ("convert {a} dollars to euros", "USD", "EUR", ["us"]),
                            ("whats {a} pounds in dollars", "GBP", "USD", ["uk"]), ("how many yen is {a} pounds", "GBP", "JPY", []),
                            ("how much is {a} swiss francs in euros", "CHF", "EUR", []), ("how much is {a} quid in euros", "GBP", "EUR", ["uk"]),
                            ("how much is {a} bucks in pounds", "USD", "GBP", ["us"]), ("convert {a} canadian dollars to us dollars", "CAD", "USD", []),
                            ("whats {a} euros in australian dollars", "EUR", "AUD", []), ("how many dollars is {a} pounds", "GBP", "USD", []),
                            ("whats {a} euros in sterling", "EUR", "GBP", ["uk"]), ("how much is {a} us dollars in canadian", "USD", "CAD", [])]:
    a = rng.choice(AMOUNTS)
    case("currency", form.format(a=a), tags=tags, must=["convert_currency"], checks=[currency(a, frm, to)])
for text, a, frm, to in [("convert fifty euros two pounds", 50, "EUR", "GBP"), ("hundred dollars in euros", 100, "USD", "EUR"),
                         ("how much is fifty bucks in euros", 50, "USD", "EUR"), ("how much is 30 euro in pounds please", 30, "EUR", "GBP")]:
    case("currency", text, tags=["asr"], must=["convert_currency"], checks=[currency(a, frm, to)])
case("currency", "whats the exchange rate between the euro and the dollar", must=["convert_currency"], checks=[currency(None, "EUR", "USD")])
case("currency", ["how much is 40 euros in pounds", "and in dollars"], must=["convert_currency"], checks=[currency(40, None, "USD")])
case("currency", ["convert 100 dollars to euros", "what about 250"], must=["convert_currency"], checks=[currency(250, "USD", "EUR")])
case("currency", "how much is 200 dollars in euros and in yen", tags=["compound"], must=["convert_currency"],
     checks=[simple("converted", amount=200, targets=["EUR", "JPY"])])
case("currency", "how much is 85 euros in pounds and split that between three people", tags=["compound"], must=["convert_currency"],
     checks=[simple("converted_share", amount=85, factor=1 / 3.0)])
case("currency", "the bill is 64 euros add a 15 percent tip and tell me what that is in pounds", tags=["compound"], must=["convert_currency"],
     checks=[simple("converted_share", amount=64, factor=1.15)])
for text, subs in [("hows the apple share price doing", ["apple", "aapl"]), ("whats the price of bitcoin", ["btc", "bitcoin"]),
                   ("hows the ftse doing today", ["ftse"]), ("whats tesla stock at", ["tesla", "tsla"]),
                   ("whats microsoft trading at", ["msft", "microsoft"]), ("how is rolls royce doing on the stock market", ["rolls", "rr"]),
                   ("whats the nasdaq at", ["nasdaq", "ixic", "ndx"]), ("how much is one ethereum worth", ["eth"]),
                   ("check the nvidia share price", ["nvda", "nvidia"]), ("hows amazon stock doing today", ["amzn", "amazon"])]:
    case("stock", text, must=["stock_price"], checks=[arg_has("stock_price", "symbol", *subs), simple("stock_answer")])

# ------------------------------------------------------------------ time in other places, and the device's own time zone
for text, subs, zone in [("what time is it in tokyo right now", ["tokyo"], "Asia/Tokyo"), ("whats the time in new york", ["new york", "new_york"], "America/New_York"),
                         ("what time is it in sydney", ["sydney"], "Australia/Sydney"), ("what time is it in los angeles", ["los angeles", "los_angeles"], "America/Los_Angeles"),
                         ("whats the time in singapore", ["singapore"], "Asia/Singapore"), ("what time is it in dubai at the moment", ["dubai"], "Asia/Dubai"),
                         ("whats the time over in chicago", ["chicago"], "America/Chicago"), ("what time is it in hong kong", ["hong kong", "hong_kong"], "Asia/Hong_Kong")]:
    case("world_time", text, must=["world_time"], checks=[arg_has("world_time", "place", *subs), simple("time_in", zone=zone)])
case("world_time", "is it late in auckland right now", must=["world_time"], checks=[arg_has("world_time", "place", "auckland")])
case("world_time", "if its 3pm here what time is it in tokyo", tags=["convert"], must=["world_time"],
     checks=[arg_has("world_time", "place", "tokyo"), simple("time_conv", local="15:00", zone="Asia/Tokyo")])
case("world_time", ["what time is it in tokyo", "and in sydney"], must=["world_time"], checks=[arg_has("world_time", "place", "sydney")])
case("world_time", "what time is it in tokio", tags=["asr"], must=["world_time"], checks=[simple("time_in", zone="Asia/Tokyo")])
for text, subs in [("ive just landed in new york switch the clock over", ["new york", "new_york"]), ("im in tokyo now change the time zone", ["tokyo"]),
                   ("switch the clock to paris time", ["paris"]), ("weve arrived in sydney update the clock", ["sydney"]),
                   ("change your time zone to lisbon", ["lisbon"]), ("im back home in london set the clock back", ["london"])]:
    case("timezone", text, must=["set_timezone"], checks=[arg_has("set_timezone", "place", *subs)])

# ------------------------------------------------------------------ the web (the stand-in's search)
# Each answer is what the stand-in web served (tests/mocks/web/search.json, a rule per question, its values
# drawn from the run's seed): the check works the right answer out from the rule and the seed, and fails a
# run whose search matched no rule, so a fixture that stops matching shows.
def served(rule, *slots, how="name", any_of=False):
    return {"kind": "served", "rule": rule, "slots": list(slots), "as": how, "any": any_of}
for text, tags, check, held in [
        ("what time does the louvre open on sundays", [], [served("louvre-hours", "open", how="time")],
         "on a sunday when do the doors of the louvre open"),
        ("is the british museum open on mondays", ["uk"], [served("british-museum", "status", how="open_closed")],
         "can i visit the british museum on a monday"),
        ("who won the champions league final this year", [], [served("champions-league", "winner", how="winner")],
         "which club lifted the champions league trophy this season"),
        ("whats in the news today", [], [served("news-today", "story1", "story2", "story3", any_of=True)],
         "give me the main headlines this morning"),
        ("how much is a ticket to the top of the eiffel tower", [], [served("eiffel-ticket", "top", how="number")],
         "what does it cost to go up to the summit of the eiffel tower"),
        ("who won the last formula one race", [], [served("f1-last-race", "winner")],
         "who took the chequered flag at the most recent grand prix"),
        ("what films are on at the cinema in leicester square tonight", ["uk"], [served("cinema-films", "film1", "film2", "film3", any_of=True)],
         "whats showing at the leicester square cinemas this evening"),
        ("who is the current prime minister of japan", [], [served("japan-pm", "pm")],
         "who leads the japanese government at the moment"),
        ("what was the score in the arsenal game last night", ["uk"], [served("arsenal-result", "score", how="score")],
         "how did arsenal get on last night"),
        ("how did the yankees do last night", ["us"], [served("yankees-result", "score", how="score")],
         "what was the final score for the yankees yesterday evening"),
        ("are there any train strikes in the uk this week", ["uk"], [served("uk-strikes", "operator")],
         "is a rail strike planned anywhere in britain this week"),
        ("when is the next home game for the chicago bulls", ["us"], [served("bulls-home-game", "day", how="date")],
         "when do the chicago bulls next play at home"),
        ("who won the mens singles at wimbledon this year", [], [served("wimbledon-mens", "winner", how="winner")],
         "who is this years wimbledon mens champion"),
        ("what are the opening hours of the sagrada familia", [], [served("sagrada-hours", "open", how="time")],
         "when can i get into the sagrada familia, what hours is it open")]:
    case("search", text, tags=tags + ["search"], must=["web_search"], checks=check, held_out=held)
case("search", "what time does the louvre open on sundays", tags=["search", "nokey"], env={"ZB_SEARCH_KEY": None},
     checks=[ans(r"search|can'?t (check|look|confirm)|cannot|not set up|unable|not sure|may have changed|might have changed|check (the|their)|"
                 r"double.check|verify|usually|typically|normally|generally|i believe|from memory|as far as i know",
                 "that it could not check, or that the answer is from memory")])

# ------------------------------------------------------------------ music, TV, the device itself, breathing, camera
for text, key, subs in [("play some abba", "artist", ["abba"]), ("play a song by queen", "artist", ["queen"]), ("put on some jazz", "genre", ["jazz"]),
                        ("play bohemian rhapsody", "title", ["bohemian"]), ("play the beatles", "artist", ["beatles"]),
                        ("play a random rock song", "genre", ["rock"]), ("play something by pink floyd", "artist", ["floyd"])]:
    case("music", text, must=["music"], checks=[arg_is("music", "action", "play"), simple("music_filter", key=key, has=subs)])
for text, action in [("stop the music", "stop"), ("turn the music off", "stop"), ("pause the music", "stop"), ("skip this song", "next"),
                     ("next track please", "next"), ("what song is this", "now_playing"), ("whats playing", "now_playing"),
                     ("what music have i got", "library|search"), ("shuffle my music", "play"), ("play something relaxing", "play|search")]:
    # "next" finds nothing playing in a fresh state, so the call made is what counts there (routing).
    case("music", text, **({"tried": ["music"]} if action == "next" else {"must": ["music"]}),
         checks=[dict({"kind": "arg", "tool": "music", "key": "action", "any": action.split("|")}, **({"made": True} if action == "next" else {}))])
case("music", "play some aba", tags=["asr"], must=["music"], checks=[arg_is("music", "action", "play")])
# The songs queued in the first turn are skipped in the second: with ZB_DRY_RUN the music tool keeps a
# pretend queue, so "next" works only after a play that worked, and moves to the queue's next song.
case("music", ["play some abba", "skip this one"], must=["music"], checks=[on_turn(0, arg_is("music", "action", "play")), arg_is("music", "action", "next")])
case("music", ["put on some jazz", "stop"], must=["music"], checks=[arg_is("music", "action", "stop")])
for text, key, tags in [("turn on the tv", "power", []), ("turn the telly on", "power", ["uk"]), ("switch off the tv", "power", []),
                        ("turn the tv volume up", "volume_up", []), ("turn the telly down a bit", "volume_down", ["uk"]), ("mute the tv", "mute", []),
                        ("tv channel up", "channel_up", []), ("tv channel down", "channel_down", []), ("put the tv on channel 4", "4", []),
                        ("open the menu on the tv", "menu", []), ("press ok on the tv", "ok", []), ("change the input on the tv", "input", []),
                        ("turn the tv up by 5", "volume_up", [])]:
    case("tv", text, tags=tags, must=["tv_remote"], must_not=["device", "smart_home"], checks=[arg_is("tv_remote", "key", key)])
for text, setting, value in [("turn your volume down to 40 percent", "volume", 40), ("set your volume to 80", "volume", 80),
                             ("youre too loud", "volume", None), ("can you speak a bit louder", "volume", None),
                             ("make your screen brighter", "brightness", None), ("set the brightness to 50 percent", "brightness", 50),
                             ("how much battery have you got left", "status", None), ("whats your battery level", "status", None),
                             ("stop reading your answers out loud", "spoken_replies", None), ("turn your voice back on", "spoken_replies", None),
                             ("turn yourself down to 30", "volume", 30)]:
    checks = [arg_is("device", "setting", setting)] + ([arg_num("device", "value", value)] if value is not None else [])
    case("device", text, tags=["uk"] if "got left" in text else [], must=["device"], must_not=["tv_remote"], checks=checks)
for text, minutes, tags in [("im feeling really stressed right now", None, []), ("i feel anxious", None, []), ("i cant sleep", None, []),
                            ("help me calm down", None, []), ("can we do a breathing exercise", None, []), ("i need to relax", None, []),
                            ("lets do five minutes of breathing", 5, []), ("im freaking out", None, ["us"]), ("im a bit overwhelmed", None, []),
                            ("i need to meditate", None, []), ("can we do a three minute breathing exercise", 3, []), ("im so wound up", None, ["uk"])]:
    case("breathe", text, tags=tags, must=["breathe"], must_not=["memory_save"],
         checks=[arg_num("breathe", "minutes", minutes)] if minutes else [])
for text in ["what am i looking at", "can you translate this sign for me", "what does this say", "read this menu to me",
             "what kind of plant is this", "describe whats in front of me", "whats the name of this building", "how much does this cost"]:
    case("look", text, tags=["camera"], must=["look"])

# ------------------------------------------------------------------ things it cannot do
for text in ["send a text to marta saying im running late", "call my mum", "order me a pizza", "book a taxi to the airport",
             "email my boss that im off sick", "post this on instagram", "buy two tickets for the cinema tonight", "transfer 50 pounds to sam",
             "whatsapp dave that im outside", "lock the front door"]:
    case("unsupported", text, must_not=["memory_save", "reminder_set", "calendar_add"], checks=[ans(CANNOT, "that it cannot do this"), NO_CLAIM])
case("unsupported", "turn on the bedroom lamp", tags=["nohome"], must_not=["tv_remote", "device"],
     checks=[ans(CANNOT + r"|smart.home|set up|settings", "that no lamps are set up")])

# ------------------------------------------------------------------ the smart home (made-up homes on the stand-in network)
H = "eval"
for text, action, targets, values, tags in [
        ("turn on the bed heater", "on", ["Bed heater"], {}, []), ("switch the bed heater on", "on", ["Bed heater"], {}, ["uk"]),
        ("can you turn the bed heater on please", "on", ["Bed heater"], {}, []), ("bed heater on", "on", ["Bed heater"], {}, []),
        ("turn off the kitchen light", "off", ["Kitchen light", "Kitchen"], {}, []), ("kitchen light off please", "off", ["Kitchen light", "Kitchen"], {}, []),
        ("turn the bedroom lamp off", "off", ["Bedroom lamp", "Bedroom"], {}, []), ("lights off in the bedroom", "off", ["Bedroom", "Bedroom lamp"], {}, []),
        ("turn off the bedroom lights", "off", ["Bedroom", "Bedroom lamp"], {}, []),
        ("dim the floor lamp to 20 percent", "set", ["Floor lamp", "Lounge"], {"brightness": 20}, []),
        ("set the kitchen light to half brightness", "set", ["Kitchen light", "Kitchen"], {"brightness": 50}, []),
        ("bedroom lights to 30 percent", "set", ["Bedroom", "Bedroom lamp"], {"brightness": 30}, []),
        ("make the bedroom lamp blue", "set", ["Bedroom lamp", "Bedroom"], {"color": "blue"}, []),
        ("turn the floor lamp red", "set", ["Floor lamp", "Lounge"], {"color": "red"}, []),
        ("make the lounge warm white", "set", ["Lounge", "Floor lamp"], {"color": "warm white"}, ["uk"]),
        ("set the bedroom lamp color to purple", "set", ["Bedroom lamp", "Bedroom"], {"color": "purple"}, ["us"]),
        ("set the heating to 20", "set", ["Hall thermostat", "Hall"], {"temperature": 20}, ["uk"]),
        ("turn the thermostat up to 22", "set", ["Hall thermostat", "Hall"], {"temperature": 22}, []),
        ("make it 21 degrees in the hall", "set", ["Hall thermostat", "Hall"], {"temperature": 21}, []),
        ("set the heat to 19 degrees", "set", ["Hall thermostat", "Hall"], {"temperature": 19}, ["us"]),
        ("pause the kitchen speaker", "pause", ["Kitchen speaker", "Kitchen"], {}, []),
        ("turn the kitchen speaker down to 10", "set", ["Kitchen speaker"], {"volume": 10}, []),
        ("set the kitchen speaker volume to 20", "set", ["Kitchen speaker"], {"volume": 20}, []),
        ("turn on the lamp in the hall", "on", ["Lamp (Hall)", "Hall"], {}, []),
        ("switch on the hall lamp", "on", ["Lamp (Hall)", "Hall"], {}, ["uk"]),
        ("turn on the tv", "on", ["Living room TV"], {}, ["route"]), ("turn off the living room tv", "off", ["Living room TV"], {}, [])]:
    case("home", text, tags=tags, must=["smart_home"], must_not=["tv_remote"] if "tv" in text else [], home=H,
         checks=[home_call(action, targets, **values)] + ([simple("home_never_on")] if action == "off" else []))
for text in ["is the heater on", "is the kitchen light on", "whats the temperature in the hall", "is anything on in the bedroom",
             "is anything on in the kitchen", "whats switched on at the moment"]:
    case("home", text, tags=["status"], must=["smart_home"], home=H, checks=[home_call("status"), simple("home_never_on")])
for text, action, targets, values in [("turn on the bad heater", "on", ["Bed heater"], {}), ("switch of the kitchen light", "off", ["Kitchen light", "Kitchen"], {}),
                                      ("turn on the flaw lamp", "on", ["Floor lamp"], {}), ("um turn off the uh bedroom lamp", "off", ["Bedroom lamp", "Bedroom"], {}),
                                      ("set the heating two 20", "set", ["Hall thermostat", "Hall"], {"temperature": 20}),
                                      ("turn of the bedroom light", "off", ["Bedroom lamp", "Bedroom"], {})]:
    case("home", text, tags=["asr"], must=["smart_home"], home=H, checks=[home_call(action, targets, **values)])
case("home", "turn everything off", home=H, checks=[simple("home_never_on")])
case("home", "turn off all the lights", home=H, checks=[simple("home_never_on")])
for text in ["turn on the garage door", "open the blinds in the bedroom", "turn on the coffee machine"]:
    case("home", text, tags=["missing"], home=H,
         checks=[simple("home_never_on"), ans(CANNOT + r"|no (device|garage|blinds|coffee)|isn'?t|not (one|listed|among|set up)|don'?t see|couldn'?t find|aren'?t",
                                              "that there is no such device")])
case("home", "turn on the fan heater", tags=["confirm"], tried=["smart_home"], home=H, checks=[simple("home_asked", name="heater")])
case("home", "switch the fan heater on please", tags=["confirm"], tried=["smart_home"], home=H, checks=[simple("home_asked", name="heater")])
for yes in ["yes", "yes please", "go on then", "yeah do it"]:
    case("home", ["turn on the fan heater", yes], tags=["confirm"], must=["smart_home"], home=H,
         checks=[on_turn(0, simple("home_asked", name="heater")), home_call("on", ["Fan heater"]), simple("home_switched", name="Fan heater")])
for no in ["no", "leave it off please", "actually no leave it", "no thanks"]:
    case("home", ["turn on the fan heater", no], tags=["confirm"], home=H,
         checks=[on_turn(0, simple("home_asked", name="heater")), every_turn(simple("home_not_switched", name="Fan heater"))])
# The search comes from the stand-in web (tests/mocks/fake_web.py), as every case's does.
# The first turn searches, and the change waits for the user's word: asked about, never made.
WEB = {"ZB_EXP_WEB": "@web/default"}
case("home", ["look up when the sun sets today, then turn on the bedroom lamp", "no, leave it"], tags=["confirm", "search"], home=H,
     env=WEB, checks=[on_turn(0, arg_has("web_search", "query", "sun")), on_turn(0, simple("asks", name="lamp")),
                      every_turn(simple("home_not_switched", name="Bedroom lamp"))])
case("home", ["look up when the sun sets today", "turn on the bedroom lamp"], tags=["confirm", "search"], tried=["smart_home"], home=H,
     env=WEB, checks=[on_turn(0, arg_has("web_search", "query", "sun")), simple("home_asked", name="lamp"),
                      every_turn(simple("home_not_switched", name="Bedroom lamp"))])
case("home", ["turn everything off", "yes"], tags=["confirm"], must=["smart_home"], home="sockets", checks=[simple("home_all_off")])
case("home", ["turn on the bedroom lamp", "make it blue"], must=["smart_home"], home=H, checks=[home_call("set", ["Bedroom lamp", "Bedroom"], color="blue")])
case("home", ["turn on the floor lamp", "dim it to 20 percent"], must=["smart_home"], home=H, checks=[home_call("set", ["Floor lamp", "Lounge"], brightness=20)])
case("home", ["is the kitchen light on", "turn it off"], must=["smart_home"], home=H, checks=[home_call("off", ["Kitchen light", "Kitchen"])])
case("home", "turn off the kitchen light and set a timer for 20 minutes", tags=["compound"], must=["smart_home", "timer"], home=H,
     checks=[home_call("off", ["Kitchen light", "Kitchen"]), timer(1200)])
case("home", "turn on the bed heater and remind me at 7 tomorrow morning to turn it off", tags=["compound"], must=["smart_home", "reminder_set|calendar_add"],
     home=H, checks=[home_call("on", ["Bed heater"]), at("tomorrow", "07:00")])
for text, check in [("turn the tv up a bit", simple("home_step", sign=1)), ("turn the tv volume down three", home_call("set", ["Living room TV"], volume_step=-3)),
                    ("press down on the tv", home_call("press", ["Living room TV"], key="down")), ("press ok on the tv", home_call("press", ["Living room TV"], key="ok")),
                    ("put netflix on the tv", home_call("set", ["Living room TV"], source="netflix")),
                    ("open youtube on the tv", home_call("set", ["Living room TV"], source="youtube")),
                    ("go back on the tv", home_call("press", ["Living room TV"], key="back")), ("go to the home screen on the tv", home_call("press", ["Living room TV"], key="home")),
                    ("turn the telly down a bit", simple("home_step", sign=-1))]:
    case("home", text, tags=["tv"] + (["uk"] if "telly" in text else []), must=["smart_home"], must_not=["tv_remote", "device"], home="tv", checks=[check])
# "Turn the TV down" is its volume, not off; "off" still means off (ZB-072).
STEP_DOWN, STEP_UP, NOT_OFF = simple("home_step", sign=-1), simple("home_step", sign=1), simple("home_no_action", actions=["off"])
for turns, check, tags in [("turn the tv down", STEP_DOWN, []), ("turn the tv up", STEP_UP, []),
                           ("can you turn the tv down please", STEP_DOWN, []), ("turn the telly down", STEP_DOWN, ["uk"]),
                           (["turn the tv up a bit", "okay that worked, now can you turn the tv down"], STEP_DOWN, []),
                           (["put netflix on the tv", "turn it down a bit"], STEP_DOWN, []), (["open youtube on the tv", "turn it up"], STEP_UP, [])]:
    case("home", turns, tags=["tv", "route"] + tags, must=["smart_home"], must_not=["tv_remote", "device"], home="tv",
         checks=[check, NOT_OFF])
for turns, tags in [("turn the tv off", []), ("switch the telly off", ["uk"]), (["turn the tv up a bit", "now turn the tv off"], [])]:
    case("home", turns, tags=["tv", "route"] + tags, must=["smart_home"], must_not=["tv_remote"], home="tv",
         checks=[home_call("off", ["Living room TV"]), simple("home_no_action", actions=["set"])])
# After a network error for a TV the smart home lists, not the infrared remote: it sends the codes of
# the one brand in settings, which may be another TV's.
LIVE = {"ZB_DRY_RUN": None}
FAILED = (r"couldn'?t|could not|can'?t|cannot|unable|didn'?t|did not|isn'?t|is not|wasn'?t|not (replying|responding|answering|reachable)|"
          r"no (answer|response|reply)|offline|unreachable|trouble|problem|failed|off or unplugged")
# Nor is it offered: "I can try the infrared remote instead", "point the top edge of the device at the TV".
NO_IR_OFFER = nans(r"infra.?red|\bIR\b|top (edge )?of (the|this|my) device|point(ing)? (the|this|my) device|remote instead",
                   "offer the infrared remote for a TV the smart home lists")
for turns, home in [("turn the tv up a bit", "tv_silent"), ("turn the tv down", "tv_closed")]:
    case("home", turns, tags=["tv", "route", "offline"], tried=["smart_home"], must_not=["tv_remote", "device"], home=home, env=LIVE,
         checks=[ans(FAILED, "that it did not work"), NO_IR_OFFER])
for turns, home in [(["turn on the tv", "turn the volume up"], "tv_silent"), (["switch the tv on", "turn it down a bit"], "tv_closed")]:
    case("home", turns, tags=["tv", "route", "offline"], must_not=["tv_remote", "device"], home=home, env=LIVE,
         checks=[on_turn(0, simple("no_call", tool="tv_remote")), on_turn(0, NO_IR_OFFER), NO_IR_OFFER])
# "Down" in a longer, looser request, after other things were said about the TV.
for turns, tags in [(["turn on the tv", "turn the tv volume up", "okay that worked, and by the way the tv did come on earlier. "
                                                                  "can we try and turn the tv down now"], []),
                    (["turn the tv up a bit", "right that worked, and the t. v. on worked as well. so can you turn the tv down now"], ["asr"]),
                    ("um can we try and turn the t. v. down now", ["asr"]), ("turn the t v down a bit", ["asr"]),
                    (["press volume up on the tv with the infrared remote", "now turn the tv volume up with the smart home instead",
                      "okay that worked. can we try and turn the tv down now"], [])]:
    case("home", turns, tags=["tv", "route"] + tags, must=["smart_home"], must_not=["tv_remote", "device"], home="tv",
         checks=[STEP_DOWN, NOT_OFF])
# Routing: with a home, the other tools must still win their own requests.
for text, must, must_not, checks, home in [
        ("set a timer for 10 minutes", ["timer"], ["smart_home", "reminder_set"], [timer(600)], H),
        ("whats the weather like in lisbon tomorrow", ["weather"], ["smart_home"], [], H),
        ("turn your volume down to 40 percent", ["device"], ["smart_home", "tv_remote"], [arg_num("device", "value", 40)], H),
        ("play a song by abba", ["music"], ["smart_home"], [arg_is("music", "action", "play")], H),
        ("remind me at 7 tomorrow morning to call the hotel", ["reminder_set|calendar_add"], ["smart_home", "timer"], [at("tomorrow", "07:00")], H),
        ("the eggs need six minutes", ["timer"], ["smart_home", "reminder_set"], [timer(360)], H),
        (["set a timer for 10 minutes", "cancel the timer"], ["timer"], ["smart_home", "reminder_cancel"],
         [on_turn(0, timer(600)), timer_action("cancel"), timers_saved()], H),
        ("im feeling really stressed right now", ["breathe"], ["smart_home", "timer"], [], H),
        ("turn your volume up", ["device"], ["smart_home", "tv_remote"], [], "tv"),
        ("stop the music", ["music"], ["smart_home"], [arg_is("music", "action", "stop")], H),
        ("make your screen brighter", ["device"], ["smart_home"], [], H),
        ("whats the time in tokyo", ["world_time"], ["smart_home"], [], H)]:
    case("route", text, tags=["home"], must=must, must_not=must_not, checks=checks, home=home)

# ------------------------------------------------------------------ other languages
for text, cat, must, checks, lang in [
        ("pon un temporizador de diez minutos", "timer", ["timer"], [timer(600)], "es"),
        ("qué tiempo hace en madrid hoy", "weather", ["weather"], [arg_has("weather", "location", "madrid"), simple("language", lang="es")], "es"),
        ("cuánto es el 20 por ciento de 150", "calc", [], [number(30)], "es"),
        ("recuérdame mañana a las 8 de la mañana llamar al médico", "reminder", ["reminder_set|calendar_add"], [at("tomorrow", "08:00")], "es"),
        ("cómo se dice gracias en japonés", "notool", [], [ans(r"arigat")], "es"),
        ("stell einen timer auf fünf minuten", "timer", ["timer"], [timer(300), simple("language", lang="de")], "de"),
        ("wie wird das wetter morgen in berlin", "weather", ["weather"], [arg_has("weather", "location", "berlin")], "de"),
        ("erinnere mich morgen um 7 uhr an den zahnarzt", "reminder", ["reminder_set|calendar_add"], [at("tomorrow", "07:00")], "de"),
        ("was ist die hauptstadt von australien", "notool", [], [ans(r"canberra")], "de"),
        ("wie viel sind 50 euro in pfund", "currency", ["convert_currency"], [currency(50, "EUR", "GBP")], "de"),
        ("mets un minuteur de trois minutes", "timer", ["timer"], [timer(180)], "fr"),
        ("quel temps fait-il à paris", "weather", ["weather"], [arg_has("weather", "location", "paris"), simple("language", lang="fr")], "fr"),
        ("combien font 15 pour cent de 200", "calc", [], [number(30)], "fr"),
        ("rappelle-moi demain à 9 heures d'appeler la banque", "reminder", ["reminder_set|calendar_add"], [at("tomorrow", "09:00")], "fr"),
        ("je suis très stressé", "breathe", ["breathe"], [], "fr"),
        ("metti un timer di cinque minuti", "timer", ["timer"], [timer(300)], "it"),
        ("che tempo fa a roma domani", "weather", ["weather"], [arg_has("weather", "location", "rom")], "it"),
        ("quanto fa 12 per 15", "calc", [], [number(180)], "it"),
        ("qual é a capital da austrália", "notool", [], [ans(r"canberra|camberra")], "pt"),   # Camberra: the Portuguese spelling
        ("põe um temporizador de dez minutos", "timer", ["timer"], [timer(600)], "pt"),
        ("como está o tempo em lisboa", "weather", ["weather"], [arg_has("weather", "location", "lisbo")], "pt"),
        ("zet een timer voor drie minuten", "timer", ["timer"], [timer(180)], "nl"),
        ("wat voor weer is het in amsterdam", "weather", ["weather"], [arg_has("weather", "location", "amsterdam")], "nl"),
        ("hoeveel is 100 euro in dollars", "currency", ["convert_currency"], [currency(100, "EUR", "USD")], "nl")]:
    case(cat, text, tags=["lang-" + lang] + (["notool"] if cat == "notool" else []), must=must,
         must_not=["web_search"] if cat == "notool" else [], checks=checks)

# ------------------------------------------------------------------ several things in one request
for text, must, checks in [
        ("check the weather in rome for saturday and set a reminder to pack an umbrella on friday at 6pm", ["weather", "reminder_set|calendar_add"],
         [arg_has("weather", "location", "rom"), at("next:%d" % FRI, "18:00")]),
        ("set a timer for 10 minutes and remind me tomorrow at 9am to call the bank", ["timer", "reminder_set|calendar_add"],
         [timer(600), at("tomorrow", "09:00")]),
        ("whats the weather in tokyo and what time is it there", ["weather", "world_time"],
         [arg_has("weather", "location", "tokyo"), arg_has("world_time", "place", "tokyo")]),
        ("add lunch with sam on friday at 1pm and remind me on thursday at 6pm to book a table", ["calendar_add", "reminder_set|calendar_add"],
         [at("next:%d" % FRI, "13:00", tools="calendar_add"), at("next:%d" % THU, "18:00")]),
        # Each effect whole: the device's volume (not its brightness) at 30, and a jazz song really started.
        ("play some jazz and turn your volume down to 30", ["music", "device"],
         [simple("device_set", setting="volume", value=30), simple("music_playing", genre="jazz")]),
        ("remember that im vegetarian and add dinner with anna on saturday at 7pm", ["memory_save", "calendar_add"],
         [arg_has("memory_save", "text", "vegetarian"), at("next:%d" % SAT, "19:00", tools="calendar_add")]),
        ("set a timer for ten minutes and uh whats the weather like in leeds tomorrow", ["timer", "weather"],
         [timer(600), arg_has("weather", "location", "leeds")]),
        ("whats the weather in paris tomorrow and how much is 100 euros in pounds", ["weather", "convert_currency"],
         [arg_has("weather", "location", "paris"), currency(100, "EUR", "GBP")]),
        ("start a 25 minute timer and play some music", ["timer", "music"], [timer(1500)]),
        ("what time is it in new york and in tokyo", ["world_time"], [arg_has("world_time", "place", "new york", "new_york"), arg_has("world_time", "place", "tokyo")])]:
    case("compound", text, tags=["asr"] if " uh " in text else [], must=must, checks=checks)

# ------------------------------------------------------------------ dialogue: threads of several turns, as people have them
# Generalised from conversations saved on the device (SPACE twice; tests/suite/from_conversation.py
# drafts a case from one): other companies and events, never the user's own words. Kept last, with
# no seeded choices, so that adding to it leaves the cases above unchanged. Simple factual turns are
# held to a few words (max_words); analytical and deeper follow-ups are not: a long answer is
# wanted there (long_ok).
# Asking back takes many shapes ("not sure what X refers to", "could you say it again", "couldn't
# find an event called X"); each of these came from a live run that the first, narrower pattern let pass.
NOT_CLARIFY = nans(r"what do you mean|(could|can) you (clarify|say (it|that) again|spell|repeat|give me (a bit )?more|tell me (what|where|which|more))|"
                   r"not sure what (you mean|.{0,40}(refers? to|stands? for|means?))|what (it|that|.{0,30}) stands? for|"
                   r"couldn'?t find (an? |any )?(event|thing) called|which company|what is (an? )?o\.?p\.?o",
                   "ask what was meant instead of looking it up")
# ... and the search must be for the listing: a live run searched "Stripe Open Platform" and passed.
IPO_QUERY = arg_has("web_search", "query", "ipo", "initial public offering", "public offering", "listing", "go public", "going public",
                    "flotation", "float", "debut")
SAYS_TIME = ans(r"\b\d{1,2}(:\d\d)?\s*(am|pm|a\.m\.|p\.m\.)|\b\d{1,2}:\d\d\b|o'?clock|\b(noon|midday)\b|in the (morning|afternoon|evening)",
                "a time of day")
# A misheard "IPO": with a company and "what time", the likely reading is the listing. Look it up
# rather than ask what the letters mean.
for text, tags in [("what time uk time is the stripe o.p.o.", ["asr"]),
                   ("when is the revolut eye pee oh happening in uk time", ["asr"]),
                   ("what time does the monzo i.p.o. start in uk time", ["asr"]),
                   ("whats the date of the canva ipo and what time is that uk time", [])]:
    case("dialogue", text, tags=tags + ["uk", "search"], must=["web_search"], checks=[NOT_CLARIFY, IPO_QUERY])
# A follow-up that leans on the turn before: the subject is known, and the time is what the search found,
# converted, or "not announced" where it found none. The searches come from the stand-in web's "events"
# fixture (tests/mocks/fake_web.py): the launch's time is published (event_time works out the UK time it
# must be), the listing's is not (no time of day may be given for it, except as what usually happens).
EVENTS_WEB = {"ZB_EXP_WEB": "@web/events"}
case("dialogue", ["when is discord's ipo", "and what time would trading start uk time"], tags=["uk", "search"], env=EVENTS_WEB,
     checks=[on_turn(0, arg_has("web_search", "query", "discord")),
             # The exchange's usual hours may be given as such: 9:30 to 16:00 in New York, 14:30 to 21:00 in the UK
             # (13:30 to 20:00 in the weeks the clocks change on different dates).
             simple("unannounced_time", usual=["09:30", "13:30", "14:30", "16:00", "20:00", "21:00"]),
             nans(r"which company|what company", "ask which company")])
case("dialogue", ["when is the next apple product launch event", "what time is that in uk time"], tags=["uk", "search"], env=EVENTS_WEB,
     checks=[on_turn(0, arg_has("web_search", "query", "apple")), simple("event_time", event="product_launch"),
             nans(r"which event|what event", "ask which event")])
# The search budget: a clarified question is looked up once or twice, not four times in a row; nor
# is the misheard one before it (a live run made seven searches there, 55 s, and passed).
case("dialogue", ["what time uk time is the stripe o.p.o.", "sorry, i meant the stripe i-p-o, the stock market listing. when does it price"],
     tags=["asr", "uk", "search"], must=["web_search"], checks=[max_calls("web_search", 2), on_turn(0, max_calls("web_search", 2))])
case("dialogue", "when does the revolut i-p-o price, the initial public offering i mean", tags=["asr", "search"],
     must=["web_search"], checks=[max_calls("web_search", 2)])
# Two questions in one, both answered, within the same budget, at whatever length they need.
case("dialogue", "whats the market mood ahead of the monzo ipo, and can staff sell their shares straight after the listing or will the price crash",
     tags=["compound", "search"], must=["web_search"], long_ok=[0],
     checks=[max_calls("web_search", 2), ans(r"lock.?up|vest|restrict|can'?t sell|cannot sell|not allowed|wait|holding period", "the rules on selling"),
             ans(r"sentiment|analyst|investor|demand|expect|bullish|bearish|optimis|cautious|mood|appetite", "the market mood")])
case("dialogue", "what are analysts expecting from shell's results this week, and if they miss will the share price drop much",
     tags=["compound", "uk", "search"], must=["web_search"], long_ok=[0],
     checks=[max_calls("web_search", 2), ans(r"analyst|expect|forecast|estimate|consensus", "what is expected"),
             ans(r"drop|fall|fell|decline|react|move|reaction|slide|hit|knock|lose|lower|down", "what a miss would do"),
             served("shell-results", "profit", how="number")])
# A deeper follow-up keeps the subject, and may take the room it needs.
case("dialogue", ["when is the canva ipo", "could that mean the best designers cash out and leave the company"], tags=["search"], long_ok=[1],
     checks=[ans(r"canva|lock.?up|vest|retention|stock|shares|equity", "the company or how staff shares work"),
             nans(r"which company|what company", "ask which company")])
case("dialogue", ["when does the discord lock-up period end after its ipo", "so could a wave of staff selling then drag the share price down for months"],
     tags=["search"], long_ok=[1],
     checks=[ans(r"discord|lock.?up|sell|supply|price|shares", "the subject"), nans(r"which company|what company", "ask which company")])
# Length both ways in one thread: a simple fact stays short, the analysis after it does not have to.
case("dialogue", ["when is shein going public", "what will its ticker symbol be", "do you think staff selling after the lock-up could drag the price down"],
     tags=["search"], long_ok=[2],
     checks=[on_turn(1, max_words(40)), ans(r"lock.?up|price|sell|shares", "the subject")])
case("dialogue", ["whats the stock market listing date for starling bank", "which exchange will it list on", "and is it likely to be a good long-term investment"],
     tags=["uk", "search"], long_ok=[2],
     checks=[on_turn(1, max_words(40)), nans(r"which (company|bank)|what (company|bank)", "ask which company"),
             on_turn(1, served("starling-ipo", "exchange"))])


def main():
    text = json.dumps(CASES, indent=1, ensure_ascii=False) + "\n"
    if "--check" in sys.argv:
        same = os.path.exists(OUT) and open(OUT, encoding="utf-8").read() == text
        print("cases.json is %s (%d cases)" % ("up to date" if same else "OUT OF DATE: run tests/suite/generate.py", len(CASES)))
        sys.exit(0 if same else 1)
    names = [c["name"] for c in CASES]
    assert len(names) == len(set(names)), "duplicate case names"
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(text)
    tags = {}
    for c in CASES:
        for t in c["tags"]:
            tags[t] = tags.get(t, 0) + 1
    print("wrote %d cases to %s" % (len(CASES), os.path.relpath(OUT)))
    print("  " + ", ".join("%s %d" % kv for kv in sorted(tags.items(), key=lambda kv: (-kv[1], kv[0]))))


if __name__ == "__main__":
    main()
