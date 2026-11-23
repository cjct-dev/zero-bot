# Zero Bot's tests

What each test is and where it lives. How to run them (`tests/run.py`, its profiles and groups), the release
procedure and the rule that no test uses a live web service are in `AGENTS.md` section 6. Start with
`tests/run.py --list`: every group, what it needs and the profiles.

| Where | What |
|---|---|
| `run.py` | the runner: groups, profiles, the path map from changed files to groups, the release |
| `netguard.py` | the network boundary of every test process (below) |
| `lib/` | code the folders share: `records.py` reads the cli build's account of a run as turns (its records file, or its printed output), `music.py` makes the stand-in music library, `answered.py` reads a smart-home driver's error as the app does |
| `unit/` | offline unit tests in C++, built and run by `unit/run_unit.sh`, with the schema snapshots in `unit/snapshots/` |
| `sim/` | offline end-to-end tests against simulated devices and services, and the screenshot check (`sim/screens/`) |
| `agent/` | the live sets (a real AI service with simulated tools) and their tooling: the generated suite (`agent/suite/`), the scorer's own test, the gate and the reports |
| `tools/` | real services, no AI: `web_contract.py` |
| `mocks/` | simulated devices, chat services, the stand-in web and the fixtures, shared by all |

Python test scripts and mocks use the standard library only. A script finds `netguard.py`, `lib/` and `mocks/` in
`tests/`, one level up from its own folder, and the builds in `build/` at the repository's root. A new test goes in
the folder for its kind, with a line in the group table at the top of `run.py`, a rule in its path map for the files
it covers (AGENTS.md section 6), and a line here.

## netguard.py: the network boundary

Every test process that runs the app gets `child_env` (no key it does not need, the inherited proxy settings
dropped, every proxy variable at a local trap that records and refuses, only 127.0.0.1, localhost and a live run's
AI service exempt); mocks get `mock_env` (a PATH and nothing else; the standalone device tests call
`netguard.sanitize()` first); a live case whose app sends anything to the trap is unscorable; every entry point that
runs the cli refuses a build without the experiments and the network pin (`require_experiments`, caps `web_all` and
`net`); a case may set only `CASE_ENV`; every app run has `ZB_TEST_NETWORK=1`. Its own test is
`netguard.py --self-test` (the runner group).

**Inside the app** (experiment builds, `ZB_EXP_NET`, which `child_env` always sets): HTTP goes only to a numeric
loopback address (127.0.0.0/8 or ::1, checked as an address; no name, not even localhost), and in a live run to
exactly the AI service's scheme, host and port (read by libcurl's own URL parser), with no redirect followed
(`http::send`); network identification never looks at the real network, so no test sends a packet to the router
(`discovery::current_network`). The proxy exemption for the AI host is only a fallback: the pin is checked on every
request, so another port, a subdomain or a redirect is refused before it leaves. `sim/tools_cli.py` (`no_real_web`,
`web_wraps`) proves it: a case run by the live sets' runner itself against a scripted model that calls every web
tool makes no request outside 127.x, and every `fetch_json` address goes through `experiment::web()`.

## lib/: shared code

- `records.py`: the cli build's records file (`ZB_EVAL_RECORDS`; `turns_from_records`, which checks the stream as it
  reads it) and, for older builds and saved runs, its printed output (`parse_turn`, `turns_from_output`), read as
  turns. The live sets' scorer (`agent/agent_eval.py`) and the loop's offline test (`sim/agent_loop_test.py`) use it.
- `music.py`: the stand-in music library (`MUSIC_LIBRARY`: tags, no sound) both live sets and
  `sim/agent_loop_test.py` play from.
- `answered.py`: who answered at a smart-home device's address, as the app reads a driver's error (`who_answered`,
  `looked_for`): the app looks for the device at a new address only when nobody, or another device, answered there
  (ZB-108). Its words are read from `home::who_answered` in `src/smarthome/control.cpp` itself, so they cannot drift.
  The drivers' end-to-end scripts (`sim/{wiz,kasa,shelly,homekit,firetv}_e2e.py`) check their errors with it.

## unit/: offline unit tests

`unit/run_unit.sh` (g++, nothing else) builds and runs:

- `smarthome_test.cpp`: store and migration, names and rooms, schema snapshots in `unit/snapshots/`, the safety
  questions, rate limits, locking, pairing, the Tapo driver's refusals, colours, discovery from sample packets through
  `discovery::devices_from`; devices are a fake driver.
- `firetv_test.cpp`, `homekit_test.cpp`, `kasa_test.cpp`, `wiz_test.cpp` and `shelly_test.cpp`: each driver's known
  and hostile answers; Fire TV's test certificates are in `mocks/firetv/`; WiZ's hostile answers come from a UDP peer
  on 127.0.0.1.
- `hue_test.cpp`: the Hue driver's known answers: certificate pins, colours, resource shapes, refusals kept in the
  secrets; its test certificates are in `mocks/hue/certs/`.
- `support_test.cpp`: the helpers the drivers share (`drivers/support`): the JSON depth check and guarded parse, UTF-8
  repair and decoding, MAC digits, hex, SHA-256/512 and HMAC against FIPS 180-2 and RFC 4231, the PEM certificate read
  and its SHA-256 pin, the RAII socket.
- `http_test.cpp`: the request options for devices, against a local peer, `http_peer.py`.
- `timers_test.cpp`: the timer tool, storage, the countdown, the alarm, its player against a stand-in `pw-cat`; it
  keeps everything it writes in a temporary folder of its own, so running it by hand is safe too.
- `key_guide_test.cpp`: what each key does on the chat screen, and that the key guide says so.
- `screen_idle_test.cpp`: when the screen dims and goes off, what keeps it lit and what wakes it.
- `radio_test.cpp`: the station file, names and nicknames, the tool absent without stations, the check of a stream
  against `radio_peer.py`, the player's live mode against a stand-in `ffplay`.
- `tools_test.cpp`: the calculator, dates and zones that do not exist, helper programs: cancelling, time limits, no
  inherited descriptors.
- `core_test.cpp`: what every thread shares: one file saved by two threads, the settings read and changed on several
  threads, all-day alerts when the clocks change, the store read at once while a slow write of its file is under way
  (fsync wrapped), the main loop's sleep and what wakes it (`uiq::wait`); what an older version left behind; and the
  shared helpers: failure shape, battery, Wi-Fi list, default route, monotonic count, start of a day; numeric dates
  spelt out for the model (`util::spell_numeric_dates`) and dates typed in the calendar editor (`util::parse_typed`),
  in both date orders; when a calendar event comes round again (`next_occurrence`); the breathing exercise's timing
  (`core/breath.h`).
- `speaker_test.cpp`: the speaker written on one thread and stopped or killed on another: nothing reaches a file
  opened after `stop()`, a stuck write is freed, stops never wait for the writer, the next answer's player gets only
  its own sound.
- `mic_route_test.cpp`: the built-in microphone with a plug in the jack, against a stand-in `amixer`.
- `music_test.cpp`: the music queue under the holds, against a stand-in `ffplay` (a queue asked for during a request
  waits for the spoken reply; the microphone, a question and its answer and the user's pause suspend the player; "next"
  while paused waits; "previous"; stop); then the player's MP3 path against a stand-in `pacat` (without ffmpeg or
  libmpg123 that part says so and passes).
- `models_test.cpp`: the two model-choosing rules of `ui/models.h`.
- `llm_stream_test.cpp`: the event stream's lines, the thinking-marker filter and tool calls put together from their
  pieces.
- `activity_test.cpp`: saved conversations: what a save holds, keys blanked out, size limits, modes, pruning, nothing
  written unless asked (diagnostic, temporary).
- `firetv_cli.cpp`, `homekit_cli.cpp`, `hue_cli.cpp`, `kasa_cli.cpp`, `wiz_cli.cpp`, `shelly_cli.cpp`: not tests, the
  small programs the drivers' end-to-end tests in `sim/` drive (`build/unit/<family>_cli`).

Then, if `build/cli/zero_bot` is built, it runs the timer and radio tools through it (`--tool`), checks
`--prompt`/`--schema` say nothing of radio without stations, and runs `sim/tools_cli.py` and `sim/agent_loop_test.py`.
`ZB_UPDATE_SNAPSHOTS=1` rewrites the snapshots: read the diff before committing it. `ZB_BUILD_WRAPPER=cmd` runs each
compile through `cmd`. `ZB_UNIT_BUILD_ONLY=1` builds the programs and stops; `ZB_UNIT_SKIP_SCRIPTS=1` leaves out
`tools_cli.py` and `agent_loop_test.py` (the runner's own groups).

## sim/: offline end to end, against simulated devices and services

**The device families.** Each runs against its simulated device in `mocks/`, on ports the system picks, and checks
from the device's side:

- `tapo_e2e.py` runs `build/cli/zero_bot` against two simulated plugs (`mocks/mock_tapo.py`, a TPAP P110 and a KLAP
  P304M).
- `firetv_e2e.py` runs `build/unit/firetv_cli` (built by `unit/run_unit.sh`) against `mocks/firetv/mock_firetv.py`:
  pairing, waking, refusals and hostile answers, checked from the simulated TV's side. It takes a few minutes (it waits
  out real wake-up times).
- `homekit_e2e.py` does the same for `build/unit/homekit_cli` against a simulated HomeKit bridge
  (`mocks/homekit/mock_homekit.py`): pairing, refused codes, bridged accessories switched off, changed databases,
  Unpair.
- `hue_e2e.py` runs `build/unit/hue_cli` against a simulated Hue Bridge (`mocks/hue/mock_hue.py`): link-button
  pairing, the pinned key, rooms as one call, refusals kept across runs, hostile answers; then the `cli` build against
  it on `ZB_TEST_NETWORK` (about two minutes; `HUE_E2E_ONLY=test_groups,...` runs some groups).
- `kasa_e2e.py` runs `build/unit/kasa_cli` against `mocks/kasa/mock_kasa.py` (plugs, a strip, a dimmer, bulbs, a
  camera and a plug moved to the newer login): discovery answers, every kind of change, meters, hostile answers; then,
  when it is built, `build/cli/zero_bot` on `ZB_TEST_NETWORK` against the same devices. Checked from the devices' side.
- `wiz_e2e.py` runs `build/unit/wiz_cli` (built by `unit/run_unit.sh`) and the `cli` build against
  `mocks/wiz/mock_wiz.py` (eleven models, lost datagrams, firmware that signs): exact setPilot params, resends,
  deadlines, and a refused signing key sent once only, all checked from the simulated device's side. About 15 s.
- `shelly_e2e.py` (about 15 s) drives `build/unit/shelly_cli` against `mocks/shelly/mock_shelly.py`, which plays Gen1
  and Gen2+ models with and without a password (its digest code reproduces RFC 7616's examples), then runs
  `build/cli/zero_bot --tool smart_home` against a saved home of simulated Shellies when that is built: passwords asked
  for, refused and never resent, covers, lights, refused models, another device at the saved address, hostile answers,
  and that no setup or calibration call is ever sent.

**The request loop, scripted.** `home_chat_e2e.py` runs `build/cli/zero_bot --chat` against `mocks/mock_chat.py` (a
chat-completions service that answers from a script, photo questions included) and simulated plugs and strips, with
`ZB_FAKE_CAMERA` for a photo of a sign that tries to switch them, and checks from the devices' side that a change
needing a yes waits for one. `selfheal_chat_e2e.py` does the same for failures: a model that sends a call cut short, a
number as a word or a misspelt tool, repeats a failed change, says "done" after a failure or replies with nothing, and a
service that answers 503, is overloaded or is lost after tools ran (the script can answer with an HTTP status, an error
inside the stream, raw arguments or several calls; a pretend plug can refuse to switch, `{"refuse_set": true}`). Use the
scripted service for any change to the loop that a live model cannot be trusted to test.

**The loop without a live service.** `agent_loop_test.py` (run by `unit/run_unit.sh` when the `cli` build exists;
about 15 s; `--only NAME`, `-v` prints every run) drives `--ask`/`--chat` against `mocks/fake_model.py`, whose rules
match the newest user message, tool results, the results since the user spoke, whether tools are offered and the
reasoning field, and whose replies can be split, delayed, failed (any HTTP status, an error in the stream) or carry
several calls; its speech and transcription services can be made to fail too. It checks what the app printed and what
it sent: the held draft and second look, the claim guards (a table of phrasings that must and must not trip them),
`say` confirmations, tool errors, steering, talk-over, the round budget and its apology, retries and service errors
(and what a spoken request that comes to nothing hears), think markers, spelt-out dates, history trimming, smart-home
questions, a byte-identical system prompt and the first request's size (a tripwire at 32,000 bytes, for a plain set-up
and with every tool offered: web search, ten radio stations, a home; raise it knowingly). With no `ZB_EXP_*` variable
set nothing sent or done changes (checked here). Add a scenario here with any change to `agent.cpp` or `llm.cpp`.

**The tools.** `tools_cli.py` runs the offline tools through the `cli` build, with good, wrong and hostile arguments,
each in a fresh state folder; it also checks the hardware guard (`ZB_EXP_NO_HARDWARE`), the stand-in web's proxy and
redirect rules, and that a live case reaches no real web service (`no_real_web`, which imports the live sets' runner,
`agent/agent_eval.py`, to run one).

**The package.** `packaging_test.py` (group `packaging`) runs `scripts/package-deb.sh` with a stand-in binary in a
temporary copy of what it reads, reads the package back with its own ar and tar reader (files, owners, modes, control
fields, maintainer scripts), checks the wake picture (320x150, 8-bit RGB, no chunk but IHDR, IDAT and IEND) and runs
`packaging/preinst` and `postrm` for every action dpkg can pass them (an upgrade from 0.2.0 and 0.4.8, without the
picture, among them) against a stand-in `dpkg-divert` that keeps each diversion with its owner and renames the files
as dpkg 1.22's does (refusing another package's diversion, or a rename over a file already there), and a stand-in
`dpkg --compare-versions`; each case is checked by the calls made and the state left. What dpkg itself then does with
the diversion is not tested here (this PC has no dpkg).

**The screens** (`sim/screens/`). `build/sim/zero_bot --shots out/` renders every screen to PNG: look at them. Every
UI change should be checked this way before it goes to the device. The script (`src/ui/shots.cpp`:
`App::render_shots` calls one function per group of screens) walks the menus by row label (`Shots::menu_to`, which
presses Up or Down as a user would), so a new settings row changes only the screens that show its page; renaming a row
the script walks to fails the run until the script is given the new label. It also checks what needs the real screen
code (a key on a dark screen only wakes it, labels fit, the status bar redraws only what changed, the two
model-choosing rules of `ui/models.h`, where a lookup of the models sends its key, at a stand-in service on 127.0.0.1,
when the service is asked for its models, that input waiting for the phone goes with a cancelled request, and how
long the main loop sleeps: `shots_idle`). `sim/screens/shots_compare.py` renders them into a temporary folder and
compares each with `sim/screens/golden/` (small tolerance; differences drawn to `build/shots-diff/`), with the wall
clock pinned (`sim/screens/fixclock.c`, built with `cc` and preloaded) and stand-ins on `PATH` for the audio, volume
and network helpers, so it plays no sound and changes nothing on the PC (plain `--shots` plays a chime and sets the
system volume).
After an intended change, `ZB_UPDATE_SHOTS=1 tests/sim/screens/shots_compare.py` and look at every changed image before
committing. The images depend on the installed fonts: refresh them on a new machine first.
`sim/screens/golden/masks.json` hides the strip where the now-playing title scrolls with real time.

## agent/: the live sets, a real AI service with simulated tools

**The scored set.** `agent_eval.py` runs spoken-style requests through the `cli` build against a live service and
checks tool calls, arguments and answers. It needs `ZB_API_BASE`, `ZB_API_KEY` (no search key), and model names in
`ZB_EVAL_LLM_MODEL` etc. Use `--only`, `--repeat N` (models are not deterministic: repeat a changed case 5 to 8 times)
and `--fast 0|1`. Expect one or two flaky cases; compare against a run of the previous code before blaming a change.
Cases run with `ZB_DRY_RUN`, except those that must reach a device (`env=LIVE`: a paired Fire TV on 127.0.0.1 that does
not answer, `TV_SILENT_HOME` and `TV_CLOSED_HOME`, for what is said after a network error); those get stand-ins that do
nothing for the programs that would change the PC (volume, sound, camera, network). What earns a pass: a change counts
only when its call worked (no `ERROR`, not waiting for a yes) and the saved state shows it, and a compound request is
checked part by part (`must`: the change worked; `tried`: only attempted, for routing and failure cases).

The evidence is not read from what the answer prints: the `cli` build writes a records file named by
`ZB_EVAL_RECORDS` (one JSON object per line: turn start and end with times, the answer, every tool call whole, the
saved state after each turn, errors, `/new`, the end of the run), from the turn's own events, so a trimmed history
hides nothing and an answer that quotes a record changes nothing. The parser (`lib/records.py`) checks the records as it
reads; anything missing, out of order or unreadable makes the run **unscorable**, which is counted apart and never a
pass. Saved runs from before fall back on the readable lines. Every result carries `case_hash`, `scorer`, `trace`,
`set` and `outcome`, which the gate uses to compare like with like. `--validate` (here and in `agent_suite.py`)
compiles every case offline. Both sets run each case with the stand-in music library (`lib/music.py`), a fixture
camera photo (`mocks/camera/sign.jpg`) and a temporary home.

A check looks at the last turn, at the one `on_turn` names, at `every_turn` (a later turn cannot hide an earlier
mistake), or at the whole conversation (`chk_moved_item`: one saved, then exactly one at the new time). Numbers keep
their sign and times their am or pm. A run that times out counts once, in the turn it stopped in; later turns are "not
started", and the summary gives the completion rate and the time to the first sentence (with `--trace`) beside the
time to the first text.

**The stand-in web.** Every case's web reads (search, forecasts, place names, exchange rates, stock prices) go to the
stand-in web (`mocks/fake_web.py` and its data in `mocks/web/`, through the `cli` build's `ZB_EXP_WEB`, honoured only
for an `http://127.x` address and compiled out of the device build); `web(fixture)` in a case's env picks a fixture
other than `default`: forecasts (`rain59`, `rain60`, `rainmissing`), event times (`events`: one published, one not
announced) and the edges (`search_empty`, `search_error`, `search_irrelevant`, `search_conflict`, `search_stale`,
`search_long`, `weather_error`, `weather_partial`, `rates_missing`, each with a scored case of its own). **Variety, not
volume:** every answer is drawn from a seed (values, wording, number of results, an unrelated result mixed in;
forecasts made up for the place and season; rates and prices moved a few per cent), the same for the same seed and
request, so checks work the right answer out from what was served, never from a number written into the case:
`chk_served(rule, slots, how)` (suite kind `served`) reads what each search of the conversation served
(`annotate_served`: the rule its query matched and that rule's facts for the run's seed and date) and requires that
evidence: no search, a failed one or one for another subject fails, one about the subject that the stand-in matched to
another rule is a fixture fault (unscorable). It scores meaning, not mention: the winner not as the side that lost, a
score with its result the right way round, a date for "when", and no other value the slot can take said beside the
served one (unless set aside in its own clause: "London, not New York"). Only what the model saw counts
(`annotate_served` keeps a fact when the response shows it: an empty result credits nothing, and "the search did not
say" is right then), and evidence ends at "/new". `chk_weather_served` (added to every case that must call the
weather) fails any temperature, both ends of a range, that the forecast did not serve for the day it is said of (the
day named in its clause, else the last one named, else the day asked about; a rain chance is no temperature; °F is
converted); the currency checks require the answer to give the converted amount (rounded as speech rounds it) or the
rate, in the right currency (its name, code or sign by the number). A web read that failed never counts as a `must`
call (an expected failure uses `tried`). Search answers live in `mocks/web/search.json` (a rule per question: `match`,
`slots` with `held_out` values, answer and result wordings); add a rule with any case that searches something new.

**Seeds.** Day-to-day runs use one fixed seed (`EVERYDAY_SEED`, or `ZB_EVAL_SEED`/`--web-seed`), so a variant and its
baseline see the same world; the seed is part of every case's `fixtures` and `case_hash` and is recorded in each result
(`web_seed`), so results of different seeds are never paired. Seeds from 1000 up are held out: they draw only held-out
values and wordings (every slot has its own, disjoint from the everyday ones; `fake_web.pool_problems` checks; nothing
falls back), and the search cases ask their held-out question forms (`held_out_turns`); the runners take one only with
`--held-out` in the release's held-out run (`HELD_OUT_SEED`). The stand-in's clock is frozen per run (`--date`,
recorded as `web_date` in each turn and result) and no served value depends on the date (forecasts for a fixed season,
by days from today), so runs on other days see the same answers with their dates moved and stay paired; the date is
therefore not hashed, except for the few cases whose answers do move with it (`date_workload`: a weather request for a
weekday or the weekend, by how many days ahead it is; the launch case of the events fixture, by its time in London
across daylight saving; a search rule that counts days from today, such as the Bulls' next game, by the weekdays they
fall on), which pair only with a run on a day like it. The camera picture hashed is the one the cases get
(`ZB_EVAL_CAMERA` or the fixture). The fixtures are versioned (`fake_web.VERSION`, and every file of `mocks/web/` is
hashed into `case_hash`).

**What a case's hash covers.** `case_hash` is taken from the case as it is scored: its turns, its checks (a scored-set
check by its function's source and the values it closes over; a suite check by its JSON), its env, its home (a home
function by its source), radio stations, `must` and the rest, and what its fixtures hold (`fixtures_of`: the music
library, the camera picture, the stand-in web's code and data, its seed, and the simulated Fire TV's code for a case
whose home starts one). So editing a check, a home function or a file of the stand-in web changes the hash of every
case that uses it, and those cases no longer pair with a baseline until they are run again; a comment in
`mocks/fake_web.py` counts too.

**The generated request set.** `suite/generate.py` (fixed seed) writes `suite/cases.json`, about 600 requests in
categories (timer, reminder, calendar, weather, home, route, ...) with feature tags (`asr`, `lang-xx`, `uk`, `us`,
`followup`, `compound`, `notool`, `clarify`, `search`, `camera`). Edit the templates, regenerate, read the diff;
`--check` says whether the file is current. `agent_suite.py` runs it with the scored set's machinery and the same
environment: `--list`, `--tags a,b+c` (`+` means both), `--skip-tags`, `--only`, `--sample N --seed S` (a few from
every category), plus `--repeat`, `--fast`, `--json`. It prints pass rates per tag and the slowest cases. Checks are
declarative (`"kind"` in the file, compiled by `KINDS` in the runner) and accept every right answer: a timer or a
reminder for "in 20 minutes", several right tools, any of the right numbers. A check looks at the last turn unless it
names another (`"turn"`: a number, or `"all"` for every turn; `moved` looks at the whole conversation), and checks of
changes read the saved state as the scored set's do (`timers_saved`, `events_saved`, `memory`, `memory_lacks`);
`long_ok` lists the turns that may answer at length, which the owner wants for analytical follow-ups (so `max_words`
is for simple factual turns only; `max_calls` caps a tool, such as searches in a row). The `dialogue` category holds
threads of several turns generalised from saved conversations (`suite/from_conversation.py` drafts one). Each case
gets a small stand-in music library (`MUSIC_LIBRARY`, `lib/music.py`: tags, no sound) unless `ZB_EVAL_MUSIC_DIR`
names a folder; set it empty for the empty library that runs before 2026-11-19 had (9 music cases then failed on any
model). Run a sample or the tags you touched, not the whole set, unless asked.

**The scorer's own test.** `scorer_test.py` (offline, no service) feeds deliberately wrong transcripts that must fail
and right ones that must pass, one per check, each asserting its problem text; run it after any change to a check, and
add a wrong transcript for any new kind of mistake.

**The gate** (`exp_compare.py`, its `--self-test` in the runner group) compares a variant with a baseline case by
case and by tag (repeat the same cases the same number of times on both sides before pooling) and ends with a verdict
(exit status 0 passes, 1 fails, 3 inconclusive, 4 incomplete): *fails* (an interval wholly below 0, overall, for a set
or a tag; a case with 6+ runs a side that lost half its passes; a critical case, the smart-home questions, refusals
after a web page and the stale yes, that did not pass every run), *incomplete* (a set it must judge has no compared
case; in a release, results without the manifest fields, under 90% of a set compared, a critical case missing),
*inconclusive* (the overall interval's lower end below the tolerable loss, `--tolerance`, 1.5 per 100 by default, or
repeats unbalanced between the sides) or *passes*. A failure outranks the rest. Each result carries a manifest:
`case_hash` (the compiled case), `scorer` (the scorer's version) and `trace`; only cases whose three agree on both
sides are compared, the rest are counted by reason, and results without them are compared by name only (the report
says so). A record that is not valid (no list of problems, an outcome its problems contradict, a `trace` its turns
contradict) and an unscorable run never count as a pass or as a zero: a case with no scorable run on a side is left
out, and an invalid record, an unscorable run of a critical case or over 2% unscorable runs make the verdict
*incomplete*. A tag's point estimate alone is noise at these sizes. How a release uses it (the roster, the held-out
run) is in AGENTS.md section 6.

**The reports.** The runners' `--trace` puts each turn's trace into `--json`; `exp_report.py` summarises such files
(completion beside every latency figure).

## tools/: real services, no AI

`web_contract.py` (group `web-contract`, run by name before a release) makes a handful of direct requests, no model,
to the real weather, place-name, exchange-rate and stock-price services, and checks that the stand-in's shapes (keys
and types, and every field the app reads) still match theirs. It needs the internet, so it is in no profile; it never
calls the search. `--self-test` checks the comparison itself offline (the runner group).

## mocks/: simulated devices and services, and fixtures

Shared by the folders above. The simulated devices and services, each started on 127.0.0.1 with a port the system
picks and `netguard.mock_env()`: `mock_tapo.py` (Tapo plugs, TPAP and KLAP), `firetv/` (a Fire TV, with test
certificates), `homekit/` (a HomeKit bridge), `hue/` (a Hue Bridge, with test certificates made by `make_certs.sh`),
`kasa/`, `wiz/` and `shelly/`; `mock_chat.py` (a chat-completions service that answers from a script) and
`fake_model.py` (a richer one, with speech and transcription, for `sim/agent_loop_test.py` and `sim/tools_cli.py`);
`fake_web.py` and `web/` (the stand-in web and its data). And the fixture photo every camera request gets,
`camera/sign.jpg`.

## The app's test modes and switches

**Command-line modes** in `src/main.cpp` (work in the `cli` build): `--ask "text"`, `--ask-wav FILE.wav` (a spoken
request, transcribed by the service), `--chat` (`/wav FILE.wav` in it is a spoken request too), `--tool NAME 'JSON'`, `--steer "text"` / `--steer-wav FILE.wav`
(inject input during the first tool), `--talk-over MS[+LATE][:text]` (hold the microphone open during a request; with
`--talk-at tool|answer`, from its first tool or once its answer has begun, as TAB does; with `+LATE`, the words arrive
that long after it closes, as when the app connects to the phone), `--follow-ups "a|b|c"` (chain requests inside one
run), `--stt FILE.wav`, `--tts "text" OUT.raw`, `--codec-bench FILE.wav FILE.opus` (time the MP3 and Opus codecs on
this machine), `--discover` (smart-home scan), `--home-refresh` (read saved smart-home devices), `--prompt` /
`--schema` (the system prompt and tools a request would get now), `--shots DIR` (render every screen to PNG; `sim`
build), `--keydump`, `--version`. `--help` lists them; each is one row of `kOptions` in `main.cpp`.

**Environment switches**: `ZB_DRY_RUN=1` (tools do not play music, send infrared or switch devices), `ZB_NO_AUDIO=1`
(which `--ask`, `--ask-wav` and `--chat` set unless it is set; `ZB_NO_AUDIO=0` makes them speak, and the tests then put
a stand-in `pw-cat` on `PATH`), `ZB_FAKE_CAMERA=file.jpg`, `ZB_FAST_MODE=0|1`, `ZB_DATE_ORDER=dmy|mdy`,
`ZB_TEST_NETWORK=1` (a stand-in network, id `net-58678b031ae6e411`, instead of the real one: only devices at 127.x
addresses are contacted and nothing is scanned; use it for any smart-home test on a PC, never `--discover`),
`ZB_TEST_TETHER=1` (the radio acts as on a phone's connection).

**Measurement switches** (`src/agent/experiment.h`; compiled into the `cli` build only, CMake option
`ZB_EXPERIMENTS`, which CMake refuses outside the `cli` build): `ZB_EXP_TRACE=file` appends a JSON line per model call
(purpose, timings, token counts the service reports, size sent) and per request (first sentence, tools, guards, second
look); no words, keys or model ids. `ZB_EXP_CACHE_BUST=1`, `ZB_EXP_TTS_PROBE=1`. `ZB_EXP_OFF=NAME[,NAME...]|all`
switches small-model controls off (`src/agent/controls.h`: `second_look`, `decide_note`, `claim_guards`, `say`,
`weekday_check`, `cancel_claims`, `date_spelling`); an unknown name stops the run, and the trace's start line names
those off. The variants once measured against the default (`ZB_EXP_TOOLS`, shorter tool text; `ZB_EXP_DECIDE`,
deciding without the second look; `ZB_EXP_CHECK_MODEL`) were retired, none adopted: set, any of them stops the run at
start, so an old command cannot measure the default under their name. `ZB_EXP_WEB` (the stand-in web) takes only plain
decimal `http://127.a.b.c[:port][/path]` and sends to the address it read, as `local`: no proxy, no redirect followed.
**The hardware guard:** `ZB_EXP_NO_HARDWARE=1`, which the live sets' runners set for every case, makes the camera
refuse unless `ZB_FAKE_CAMERA` names a picture, and the infrared transmitter refuse, so a test run never sends a real
photo to the service or a code into the room (`sim/tools_cli.py` checks it, and the stand-in's proxy and redirect
rules). `ZB_EXP_SCAN=file`: on `ZB_TEST_NETWORK`, where nothing is scanned, the short search for a smart-home device
that may have moved is made all the same: it hears nothing, and appends a line to the file, so an end-to-end test sees
whether a command looked for a device and what its error then says (`sim/shelly_e2e.py`, `sim/tapo_e2e.py`). With no
`ZB_EXP_*` variable set nothing sent or done changes (checked in `sim/agent_loop_test.py`).
