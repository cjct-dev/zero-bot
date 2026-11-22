# Guide for coding agents

This file is for an AI coding agent (or a new developer) picking up Zero Bot. It explains what
the app is, the hardware it runs on, how the code is laid out, how to build, test and deploy,
and the mistakes already made so that they are not made again. `README.md` is the user-facing
description; read it first. `docs/agent-architecture.html` has diagrams of the request loop.

## 1. What this is

Zero Bot is a pocket voice assistant for the M5Stack CardputerZero. The user presses TAB,
speaks, presses TAB again; the app transcribes the speech, runs a tool-calling chat model, and
speaks the answer. It also takes typed input. It is a native C++17 app drawing with LVGL 9
straight to the framebuffer. All "intelligence" comes from an OpenAI-compatible service the
user configures (chat with tool calling, speech-to-text, text-to-speech, vision). The device is
online-only by design: without a network there is no inference, so do not add offline fallbacks.

Hard rule for this public repository: **never commit the name, address, keys or model names of
any particular AI provider, or any personal detail** (names, network names, addresses, e-mail).
Scan staged changes and the built binary (`strings`) before every push. The build uses
`-ffile-prefix-map` so the binary does not embed the build machine's paths; keep it.

## 2. The hardware, and what it means for code

| Part | Detail | Consequence |
|---|---|---|
| CPU / RAM | Raspberry Pi CM0: four Cortex-A53 at 1 GHz, about 350 MB usable RAM, zram swap | Starting a process is expensive (ffmpeg: 1 to 3 s; ffplay and rpicam-jpeg: 1 to 2 s). Prefer in-process work or a kept-open helper. Never spawn on the UI thread. |
| Screen | 320x170, RGB565, `/dev/fb0`, driven over SPI | Sending pixels costs several times what drawing them does. Redraw only what changed; keep animated areas small; 8 to 12 updates a second is plenty. |
| Keyboard | TCA8418 keypad, `/dev/input/by-path/platform-3f804000.i2c-event` | The driver's fn layer sends its own key codes (arrows on fn+F/Z/X/C, volume on fn+A/S/D, brightness on fn+U/I, media on fn+Q/W/E, F1..F10 on fn+digits). The sym layer also arrives as dedicated codes. No auto-repeat from the driver: `input.cpp` synthesizes it. |
| Audio | ES8389 codec through PipeWire (`pw-cat`), tiny speaker, weak microphone | The mic needs digital gain and normalising. Use `pw-cat`/`wpctl`, not ALSA directly. Bluetooth headsets must stay output-only (see lessons). |
| Camera | IMX219 via `rpicam-jpeg`, mounted upside down | Photos are rotated 180 degrees by default (`camera_flip`). |
| Radio | CYW43439: 2.4 GHz Wi-Fi and Bluetooth share one radio | Bluetooth audio stutters when the Wi-Fi signal is weak. Nothing in the app can fix that. |
| Infrared | TX `/dev/lirc0`, RX `/dev/lirc1` | Used by the TV remote tool, including learning buttons. |
| Battery | bq27220 gauge under `/sys/class/power_supply/bq27220-0/` | Reads 0 current on USB power. Idle cost matters most: the app is meant to be left running. |
| Launcher | `APPLaunch.service` (a systemd user service) lists `/usr/share/APPLaunch/applications/*.desktop` and stays alive behind the app | The app owns the screen while it runs. Holding ESC returns to the launcher. |

## 3. Code layout

```
src/main.cpp        entry point and the command-line test modes (section 6)
src/agent/          the request loop (agent), service client (llm), system prompt (prompt),
                    tools, sentence queue for speech (speech), arithmetic (calc)
src/core/           settings, calendar/reminder/timer/memory store, timers (the timer tool, which
                    timer words mean, the alarm's logic), music library, radio (the user's station
                    file, the radio tool, the check of a stream), breathing timing,
                    phone tethering, what an upgrade leaves behind (upgrade), utilities (dates,
                    files, strings)
src/net/            libcurl wrapper with a shared connection/DNS/TLS-session cache
src/platform/       display, keyboard, audio (recorder, speaker, music), codecs, camera,
                    infrared, subprocess helpers
src/smarthome/      discovery (mDNS, SSDP, TP-Link and WiZ broadcasts); driver.h, the contract every
                    device family implements; home.h, the layer above it: store.cpp (the 0600
                    file, migration, the network gate), targets.cpp (names, rooms, groups),
                    control.cpp (commands, safety rules, locking, status, refresh, pairing),
                    tool.cpp (the smart_home tool); color.h (colour names and conversions);
                    drivers/ (one per family; registry.cpp lists them); Tapo transport (tapo)
src/ui/             app controller (keys, timers, state), chat screen, menu system, settings
                    pages, the keys and their guide (key_guide), when the screen dims and goes off
                    (screen_idle), breathing screen, alarm banner, theme
tests/              run.py, the entry point that runs the groups below by profile (section 6);
                    scored agent test set (needs a live service), device-free unit tests
                    (run_unit.sh: smart home, HTTP options, timers, Fire TV, HomeKit, Hue, Kasa,
                    WiZ, Shelly, keys, radio, tools), Tapo, Fire TV, HomeKit, Hue, Kasa, WiZ and
                    Shelly end to end (tapo_e2e.py, firetv_e2e.py, homekit_e2e.py, hue_e2e.py,
                    kasa_e2e.py, wiz_e2e.py, shelly_e2e.py),
                    the loop against a scripted chat service (home_chat_e2e.py, agent_loop_test.py,
                    and selfheal_chat_e2e.py for failures), the generated request set (suite/,
                    agent_suite.py), the scorer's own test (scorer_test.py), the gate and
                    reports for live runs (exp_compare.py, exp_report.py), golden screenshots
                    (shots_compare.py, golden/shots/), simulated devices, chat service and web
                    (mocks/)
scripts/            toolchain setup, deploy over ADB, .deb packaging, ADB wrapper
```

Threads: the UI thread runs LVGL and the main loop in `ui/app.cpp`. The agent has one worker
thread (`Agent::worker`), speech has one (`SpeechQueue::run`), keys are read on one, music has
one per queue. Anything a worker wants on screen goes through `uiq::post()`; never touch LVGL
from another thread. Slow work started from a menu uses `background(work, done)` in
`settings_pages.cpp`. The settings are read on every thread and changed by the UI and by tools:
`Settings::get()` returns a snapshot (a read-only copy taken under a lock), and every change goes
through `Settings::update([](Settings &s) { ... })`, which makes it under that lock and saves.

State on disk (all under the user's home, mode 0600 where they hold secrets):
`~/.config/zero-bot/settings.json`, `~/.config/zero-bot/radio.json` (the user's own stations,
written by hand; the app ships none and never writes it), and in `~/.local/share/zero-bot/`: `calendar.json`,
`reminders.json` (running timers too, as records with `"kind": "timer"`), `memory.json`,
`ir_learned.json`, `music_index.json`, `smarthome.json`, and `conversations/` (saved with SPACE
twice; diagnostic, temporary, see section 7).

## 4. How a request flows

1. `App::handle_key` starts or stops the recorder (TAB) or collects typed text, then calls
   `Agent::submit_audio` / `submit_text`.
2. `Agent::run_turn` absorbs queued inputs (transcribing audio), then loops for up to
   `kMaxRounds` model calls: stream a reply, run any tool calls, repeat.
3. Text streams sentence by sentence into `SpeechQueue`, which synthesizes and plays it.

Things in that loop that exist for a reason; do not remove them without re-running the tests:

- **Fast mode** turns the model's reasoning off for speed. With reasoning off the model
  sometimes claims an action without calling the tool. So a reply that used no tool is held
  back and checked once ("second look") before it is spoken, and two regex guards
  (`claims_action`, `claims_not_saved`) catch "done" and "that isn't saved" said without a call.
- **`say`**: tools whose effect is fully known up front (timer, calendar, switch) take the
  confirmation sentence as a parameter, so success is spoken without a second model call.
- **Steering**: input that arrives while a request is being worked on is added to the same
  request. While the microphone is open nothing new is spoken and no action runs
  (`Agent::set_listening`): a later call of a batch waits for it to close, and once the user has
  said more, the calls not yet made are not made (each answered "this call was not made", not
  counted as failed), so the model decides again with their words.
- **Typed or spoken** (ZB-091): a request's answer is spoken only when something in it was said
  aloud (and spoken replies are on); a typed one's is only shown. Its state says which: `Speaking`
  (TAB stops it: `Agent::interrupt_speech`, and the conversation keeps "[the user interrupted
  here]") or `Answering` (TAB does not stop it). Music waits only for an answer that is heard
  (`tools::Context::in_conversation`), and each answer's end is signalled at its own request's end.
- **Numeric dates** are spelt out by rule (`util::spell_numeric_dates`) before the model sees
  them, in the user's date order with the year filled in.
- **Timestamps** ride on each user message, not in the system prompt, so the prompt stays
  byte-identical between calls and the service can reuse its cached prefix.
- **Music holds** (`audio::Music::hold`): music waits for the spoken reply to finish, and stays
  quiet from a spoken question to the end of its answer.
- **Smart-home questions**: a change that needs the user's yes (a thing with *Ask before
  switching on*, heating outside 10 to 25 degrees, more than eight things, loud volume, after a
  web page or photo) makes `home::execute` answer `CONFIRM: ...` and remember the change. The loop
  reports every user message (`home::user_spoke`) and every final reply, with its text
  (`home::assistant_replied`). The same change (action, target and values) goes ahead only when
  the question was put (a reply that asks, with a question mark, and names the target in words
  that pick it out: every word of its name, a word of it no other thing's name has, "the TV" for
  the only TV, "everything" or "all the lights" for a group), the very next user message is a
  short, plain yes (every word an agreement, politeness or a word for the target; a new request,
  "leave it off please" or "yes, and..." is not), and it is asked for again in that turn. There is
  deliberately no parameter the model could set to skip this. `web_search` and `look` call
  `home::saw_untrusted_text()`: from then on every change needs a yes for as long as the model can
  still see that text, which ends only when the conversation is cleared
  (`home::conversation_reset`) or trimmed past it (`Agent::trim_history` reports what it keeps
  through `home::context_kept`). So "no" after a web page or photo holds however long the model
  waits before asking again. Names in `ERROR`/`CONFIRM` sentences are always in double quotes, and
  `home::clean_name` removes every kind of double quotation mark (typographic and full-width ones
  too) from every name, so a device's name cannot end its quotes and read as an instruction.
- **Context notes** (`Agent::add_context`): something the user saw or heard without the model
  taking part (a timer's alarm) rides along with their next message, within ten minutes, so
  that "give it five more minutes" means something to the model.
- **Failed tool calls** (tested by `tests/selfheal_chat_e2e.py`):
  - *What the model is told.* `tools::run` checks each argument against the tool's own schema
    before the tool runs: a wrong type names the argument, what it must be and what was sent;
    "yes", "true" or 1 for a boolean and an enum value in other capitals are taken as meant; empty
    text for a number counts as not given, and other text must be a number as a whole
    (`util::to_number`, which `num_arg` uses too: "20 minutes" is not 20, "12oops" not reminder 12).
    A call cut short or broken says where, with an example; an unknown tool name gets the nearest
    one. Every failure has one shape, `ERROR: what was wrong. What to do` (a tool's JSON
    `{"error": ...}`, whatever "error" holds, is turned into it before anything is added to the
    result or it is cut short); `tools::failed()` recognises it.
    Tools whose schema is made at run time (`smart_home`, `radio`: `obj({})` in `build()`) check
    their own arguments.
  - *Retries.* Only what is safe to repeat is sent again by itself: the AI service, transcription,
    speech, and the tools' reads and searches (`fetch_json`), through `http::send_retrying`, after
    a failure on the way (`http::transient`: no connection, a dropped one, 408, 429, 5xx but 501
    and 505; never a refusal, and never a timeout once the request had gone out), after a pause
    and inside the request's own time limit. Nothing that changes, logs in or pairs is retried by
    this; a device change is resent only by the smart-home layer, at a device's new address, and
    only when it comes out the same if made twice (on, 40 %, an input; never a volume step, key
    press or playback control: some TVs toggle play and pause with one command). In the loop, a
    call that failed is not made again unchanged in the same request (`call_key`: tool and
    arguments, "say" aside): the model is told it already failed. What the user says next clears
    that, so "try again" works. A call refused only for what its `say` claimed, before anything was
    done (`weekday_slip`, `claims_new`: `Context::say_refused`), is not remembered: it may come again
    with the say put right, or after the step it lacked.
  - *Never silence.* "Done" in a reply after tool calls that all failed or wait for a yes is
    withdrawn once, like the guard for no tool at all (`claims_done`: an offer put as a question,
    "Shall I put it in for next year?", is not withdrawn; it is often the right answer then; the
    radio's and music's own confirmations, "Here is X." or "X is playing", are claims too, but not in
    a sentence that says no or asks: "Nothing is playing right now" is the honest answer to "skip"). An
    empty reply goes back to the model once, then an answer is made in code. When the service is
    lost after tools ran, the user hears the confirmations of the calls that worked and that the
    rest could not be finished.

## 5. Building

Everything is fetched into the project folder; nothing is installed system-wide.

```sh
scripts/setup-toolchain.sh             # once: cross compiler, device sysroot, LVGL, CMake (about 2 GB)
export PATH="$PWD/.tools/bin:$PATH"
cmake --preset cli && cmake --build --preset cli    # screenless agent for this PC: fastest loop
cmake --preset sim && cmake --build --preset sim    # desktop build with an SDL window
cmake --preset cp0 && cmake --build --preset cp0    # cross build for the device
```

The three builds share all the sources except that `cli` leaves out `src/ui/` and the display
and input code. Libraries: libcurl, libcrypto (OpenSSL 3.5 on the device, possibly newer on a
PC: use only non-deprecated interfaces), FreeType and libpng for the UI. MP3 encoding and Opus
decoding are loaded at run time with `dlopen` and are optional.

## 6. Testing

**Start with `tests/run.py`.** It runs the tests below in groups and prints one line per group (its
count and seconds), then a short summary; each group's full output goes to
`build/test-runs/<date-time>/<group>.log`. `--list` shows the groups, what each needs and the
profiles; `--plan` says what would run and why, and runs nothing; `--build` builds what the chosen
groups need first. Which to use:

- After a change, before committing: `tests/run.py --profile change` picks the groups that cover the
  files changed since `main` (working tree included; `--base REF` for another start). Code the device
  build shares also gets `sim-build`, which compiles the sim build: the no-experiments side of
  `experiment.h` and `#if !ZB_CLI_ONLY` compile nowhere else. For a change to what the model is told
  (prompt, a tool's description or schema, the loop) it also prints which live cases to run five
  times (suite tags; the scored set's `radio_*` for the radio, with `--scored-only`); `--live` runs them.
- To see that everything offline still passes: `tests/run.py --profile quick` (about 5 minutes; it
  leaves out Fire TV's end-to-end test and the live sets). `--profile offline` includes Fire TV.
- Named groups: `tests/run.py --groups unit,hue`; a live check:
  `tests/run.py --groups live-suite --tags timer --repeat 5`; a variant against a
  baseline: `--exp ZB_EXP_OFF=second_look --baseline B.json` (inherited `ZB_EXP_*` are never passed on).
- Before a release: `tests/run.py --profile release --baseline EVAL.json,SUITE.json` (or `--groups
  release`, the same mode) builds (unless `--no-build`: then a build older than any input stops it),
  runs every offline group, every case of both live sets once, then the gate of `tests/exp_compare.py`
  with `--release`. It refuses `--tags`, `--sample`, `--skip-tags`, `--only`, `--scored-only`,
  `--fast` and `--exp`. Live cases run only on a cli build
  that is an experiments build, whose hardware guard holds and which writes the records. It does not
  start without a baseline that covers the roster (below): record one with
  `tests/run.py --groups live-scored,live-suite` (its `eval.json` and `suite.json`). It also builds
  the device binary (cp0; with `--no-build` a stale one stops it) and fails when its version is not
  CMakeLists.txt's or it holds a cli-only string (`ZB_EXP_`, `ZB_EVAL_RECORDS`, `tool_done`, `[call]`,
  `[state]`) or a build machine's path (`/home/`). Last it runs the **held-out run** (group `held-out`, release
  only): both sets once on the held-out seed, judged on its own, not against the baseline
  (`exp_compare.py --held-out`, against the roster at the held-out seed: every case defined, run exactly as
  often as asked, by the current case and scorer, each record showing its run took place (its own `run_id`, so
  copies are not repeats, the traced app, the held-out requests, every turn answered and holding the app's trace of
  its request; a case whose answers move with the date is expected with the hash of the run's own fixture date, so
  a run judged after midnight is not stale); each set's pass rate, and
  that of the cases that rest on a web service (`web`), at least its floor, `HELD_OUT_FLOOR`; every critical case of
  the roster passing), reported
  beside the gate. The held-out run is refused anywhere else (`--groups held-out` outside the release, the
  scorers' `--held-out` without the `ZB_HELD_OUT_RUN` the release sets). **Recalibrating the floors** (after a
  change to the sets or the stand-in that moves the pass rates): `tests/run.py --groups held-out --calibrate 3`
  (3 or more; never for tuning: read only the per-group rates it prints) runs the held-out set three times and prints each group's mean, spread and a floor (the mean less the larger of
  2 points and two standard deviations); put those in `HELD_OUT_FLOOR` and say so in the commit. A change that wins on the everyday seed but loses on the held-out
  one is fitted to the everyday answers; never tune on the held-out results. **Before a release**, also run
  `tests/run.py --groups web-contract` (or `tests/web_contract.py`): a handful of direct requests, no model, to
  the real weather, place-name, exchange-rate and stock-price services, checking that the stand-in's shapes
  (keys and types, and every field the app reads) still match theirs. It needs the internet, so it is in no
  profile; it never calls the search.

`ZB_EVAL_WRAPPER` prefixes every live run (a lock, or a script that sets the service's variables);
live runs always use `--jobs 1`. **No test uses a live web service** (the owner's search allowance is
spent by test runs otherwise): every live case's web reads, search included, go to the stand-in web
(below), with a dummy search key; the runners remove any inherited `ZB_SEARCH_KEY` and refuse to start
when a case or the binary could still reach a real web service (`agent_eval.py` `preflight`, `web_guard`,
`probe_binary`'s `web_all`). **The network boundary** (`tests/netguard.py`): every test process that runs the
app gets `child_env` (no key it does not need, the inherited proxy settings dropped, every proxy variable at a
local trap that records and refuses, only 127.0.0.1, localhost and a live run's AI service exempt); mocks get
`mock_env` (a PATH and nothing else; the standalone device tests call `netguard.sanitize()` first); a live case
whose app sends anything to the trap is unscorable; every entry point that runs the cli refuses a build without the
experiments and the network pin (`require_experiments`, caps `web_all` and `net`); a case may set only `CASE_ENV`;
every app run has `ZB_TEST_NETWORK=1`. **Inside the app** (experiment builds, `ZB_EXP_NET`, which `child_env` always
sets): HTTP goes only to a numeric loopback address (127.0.0.0/8 or ::1, checked as an address; no name, not even
localhost), and in a live run to exactly the AI service's scheme, host and port (read by libcurl's own URL parser),
with no redirect followed (`http::send`); network identification never looks at the real network, so no test sends
a packet to the router (`discovery::current_network`). The proxy exemption for the AI host is only a fallback: the
pin is checked on every request, so another port, a subdomain or a redirect is refused before it leaves. `tools_cli.py`
(`no_real_web`, `web_wraps`) proves it: a case run by the runner itself against a scripted model that calls every
web tool makes no request outside 127.x, and every `fetch_json` address goes through `experiment::web()`.
`--no-search` is accepted and changes nothing. Groups never inherit switches that change what they test (`ZB_UPDATE_SHOTS`,
`ZB_UPDATE_SNAPSHOTS`, `ZB_UNIT_*`, `HUE_E2E_ONLY`, `ZB_FAST_MODE`, `ZB_EXP_*`). A new test script
gets a line in the group table at the top of `tests/run.py`, and a rule in its path map for the
files it covers.

**The gate's verdicts** (`tests/exp_compare.py`, `--self-test` in the runner group): *fails* (an
interval wholly below 0, overall, for a set or a tag; a case with 6+ runs a side that lost half its
passes; a critical case, the smart-home questions, refusals after a web page and the stale yes,
that did not pass every run), *incomplete* (a set it must judge has no compared case; in a release,
results without the manifest fields, under 90% of a set compared, a critical case missing),
*inconclusive* (the overall interval's lower end below the tolerable loss, `--tolerance`, 1.5 per 100
by default, or repeats unbalanced between the sides) or *passes*. A failure outranks the rest. Each
result carries a manifest: `case_hash` (the compiled case), `scorer` (the scorer's version) and
`trace`; only cases whose three agree on both sides are compared, the rest are counted by reason, and
results without them are compared by name only (the report says so). A record that is not valid (no
list of problems, an outcome its problems contradict, a `trace` its turns contradict) and an unscorable
run never count as a pass or as a zero: a case with no scorable run on a side is left out, and an
invalid record, an unscorable run of a critical case or over 2% unscorable runs make the verdict
*incomplete*. **A release is measured against the roster**, every case defined in this tree (the scored
set's `CASES` and `cases.json`, by name and current `case_hash`, with the critical cases read from the
definitions), never against the cases the results happen to hold: the variant needs a scorable run of
every case as defined now by the current scorer, 90% of each set must be compared, and every critical
case compared and passing; a critical case cannot be left out. The baseline's provenance must match:
the same `case_hash`, `scorer`, `trace` and, where recorded, `fixtures`. **Trace policy:** the runner
always passes `--trace`, which adds `stream_options.include_usage` to what is sent, so a baseline for
the gate must be traced too; traced against untraced pairs are left out. A live run whose cases
failed passes only under a gate verdict of *passes* that compared its set (`gate.json` in the run
folder). The rest of this section describes each test.

What exists:

- **Device-free tests**: `tests/run_unit.sh` (g++, nothing else) builds and runs
  `tests/unit/smarthome_test.cpp` (store and migration, names and rooms, schema snapshots in
  `tests/snapshots/`, the safety questions, rate limits, locking, pairing, the Tapo driver's
  refusals, colours, discovery from sample packets through `discovery::devices_from`; devices are
  a fake driver), `tests/unit/firetv_test.cpp`, `tests/unit/homekit_test.cpp`,
  `tests/unit/kasa_test.cpp`, `tests/unit/wiz_test.cpp` and `tests/unit/shelly_test.cpp` (each
  driver's known and hostile answers; Fire TV's test certificates are in `tests/mocks/firetv/`;
  WiZ's hostile answers come from a UDP peer on 127.0.0.1),
  `tests/unit/hue_test.cpp` (the Hue driver's known answers: certificate pins, colours, resource
  shapes, refusals kept in the secrets; its test certificates are in `tests/mocks/hue/certs/`),
  `tests/unit/http_test.cpp` (the request
  options for devices, against a local peer), `tests/unit/timers_test.cpp` (the timer tool,
  storage, the countdown, the alarm, its player against a stand-in `pw-cat`; it keeps everything
  it writes in a temporary folder of its own, so running it by hand is safe too) and
  `tests/unit/key_guide_test.cpp` (what each key does on the chat screen, and that the key guide
  says so), `tests/unit/screen_idle_test.cpp` (when the screen dims and goes off, what keeps it lit
  and what wakes it), `tests/unit/radio_test.cpp` (the station file, names and nicknames, the tool absent
  without stations, the check of a stream against `tests/unit/radio_peer.py`, the player's live
  mode against a stand-in `ffplay`), `tests/unit/tools_test.cpp` (the calculator, dates and
  zones that do not exist, helper programs: cancelling, time limits, no inherited descriptors) and
  `tests/unit/activity_test.cpp` (saved conversations: what a save holds, keys blanked out, size
  limits, modes, pruning, nothing written unless asked; diagnostic, temporary),
  then, if `build/cli/zero_bot` is built, runs the timer and radio tools through it (`--tool`),
  checks `--prompt`/`--schema` say nothing of radio without stations, and runs the other offline
  tools through `tests/tools_cli.py` (good, wrong and hostile arguments, each in a fresh state
  folder). `ZB_UPDATE_SNAPSHOTS=1`
  rewrites the snapshots: read the diff before committing it. `ZB_BUILD_WRAPPER=cmd` runs each
  compile through `cmd`. `ZB_UNIT_BUILD_ONLY=1` builds the programs and stops;
  `ZB_UNIT_SKIP_SCRIPTS=1` leaves out `tools_cli.py` and `agent_loop_test.py` (the runner's own groups).
- **Tapo end to end**: `tests/tapo_e2e.py` runs `build/cli/zero_bot` against two simulated plugs
  (`tests/mocks/mock_tapo.py`, a TPAP P110 and a KLAP P304M) on ports the system picks.
- **Fire TV end to end**: `tests/firetv_e2e.py` runs `build/unit/firetv_cli` (built by
  `run_unit.sh`) against `tests/mocks/firetv/mock_firetv.py`: pairing, waking, refusals and hostile
  answers, checked from the simulated TV's side. It takes a few minutes (it waits out real wake-up
  times). `tests/homekit_e2e.py` does the same for `build/unit/homekit_cli` against a simulated
  HomeKit bridge (`tests/mocks/homekit/mock_homekit.py`): pairing, refused codes, bridged
  accessories switched off, changed databases, Unpair. `tests/hue_e2e.py` runs `build/unit/hue_cli`
  against a simulated Hue Bridge (`tests/mocks/hue/mock_hue.py`): link-button pairing, the pinned
  key, rooms as one call, refusals kept across runs, hostile answers; then the `cli` build against
  it on `ZB_TEST_NETWORK` (about two minutes; `HUE_E2E_ONLY=test_groups,...` runs some groups).
- **Kasa end to end**: `tests/kasa_e2e.py` runs `build/unit/kasa_cli` against
  `tests/mocks/kasa/mock_kasa.py` (plugs, a strip, a dimmer, bulbs, a camera and a plug moved to the
  newer login): discovery answers, every kind of change, meters, hostile answers; then, when it is
  built, `build/cli/zero_bot` on `ZB_TEST_NETWORK` against the same devices. Checked from the
  devices' side.
- **WiZ end to end**: `tests/wiz_e2e.py` runs `build/unit/wiz_cli` (built by `run_unit.sh`) and the
  `cli` build against `tests/mocks/wiz/mock_wiz.py` (eleven models, lost datagrams, firmware that
  signs): exact setPilot params, resends, deadlines, and a refused signing key sent once only, all
  checked from the simulated device's side. About 15 s.
- **Shelly end to end**: `tests/shelly_e2e.py` (about 15 s) drives
  `build/unit/shelly_cli` against `tests/mocks/shelly/mock_shelly.py`, which plays Gen1 and Gen2+
  models with and without a password (its digest code reproduces RFC 7616's examples), then runs
  `build/cli/zero_bot --tool smart_home` against a saved home of simulated Shellies when that is
  built: passwords asked for, refused and never resent, covers, lights, refused models, another
  device at the saved address, hostile answers, and that no setup or calibration call is ever sent.
- **Code ported from other projects** keeps their notices in the source file and in
  `licenses/THIRD-PARTY-NOTICES.md`, which the package installs; add to it with any new port.
- **The request loop, scripted**: `tests/home_chat_e2e.py` runs `build/cli/zero_bot --chat` against
  `tests/mocks/mock_chat.py` (a chat-completions service that answers from a script, photo
  questions included) and simulated plugs and strips, with `ZB_FAKE_CAMERA` for a photo of a sign
  that tries to switch them, and checks from the devices' side that a change needing a yes waits
  for one. `tests/selfheal_chat_e2e.py` does the same for failures: a model that sends a call cut
  short, a number as a word or a misspelt tool, repeats a failed change, says "done" after a
  failure or replies with nothing, and a service that answers 503, is overloaded or is lost after
  tools ran (the script can answer with an HTTP status, an error inside the stream, raw arguments
  or several calls; a pretend plug can refuse to switch, `{"refuse_set": true}`). Use the scripted
  service for any change to the loop that a live model cannot be trusted to test.
- **The loop without a live service**: `tests/agent_loop_test.py` (run by `run_unit.sh` when the
  `cli` build exists; about 15 s; `--only NAME`, `-v` prints every run) drives `--ask`/`--chat`
  against `tests/mocks/fake_model.py`, whose rules match the newest user message, tool results,
  the results since the user spoke, whether tools are offered and the reasoning field, and whose
  replies can be split, delayed, failed (any HTTP status, an error in the stream) or carry several
  calls. It checks what the app printed and what it sent: the held draft and second look, the
  claim guards (a table of phrasings that must and must not trip them), `say` confirmations, tool
  errors, steering, talk-over, the round budget and its apology, retries and service errors, think
  markers, spelt-out dates, history trimming, smart-home questions, a byte-identical system prompt
  and the first request's size (a tripwire at 32,000 bytes, for a plain set-up and with every tool
  offered: web search, ten radio stations, a home; raise it knowingly). Add a scenario
  here with any change to `agent.cpp` or `llm.cpp`.

- **Command-line modes** in `src/main.cpp` (work in the `cli` build):
  `--ask "text"`, `--chat`, `--tool NAME 'JSON'`, `--steer "text"` (inject input during the
  first tool), `--talk-over MS[:text]` (hold the microphone open during a request; with
  `--talk-at tool|answer`, from its first tool or once its answer has begun, as TAB does),
  `--follow-ups "a|b|c"` (chain requests inside one run), `--stt FILE.wav`, `--tts "text" OUT.raw`,
  `--discover` (smart-home scan), `--home-refresh` (read saved smart-home devices),
  `--prompt` / `--schema` (the system prompt and tools a request would get now),
  `--shots DIR` (render every screen to PNG; `sim` build), `--keydump`, `--version`.
- **Scored agent set**: `tests/agent_eval.py` runs spoken-style requests through the `cli`
  build against a live service and checks tool calls, arguments and answers. It needs
  `ZB_API_BASE`, `ZB_API_KEY` (no search key), and model names in `ZB_EVAL_LLM_MODEL`
  etc. Use `--only`, `--repeat N` (models are not deterministic: repeat a changed case 5 to 8
  times) and `--fast 0|1`. Expect one or two flaky cases; compare against a run of the
  previous code before blaming a change. Cases run with `ZB_DRY_RUN`, except those that must reach a
  device (`env=LIVE`: a paired Fire TV on 127.0.0.1 that does not answer, `TV_SILENT_HOME` and
  `TV_CLOSED_HOME`, for what is said after a network error); those get stand-ins that do nothing for
  the programs that would change the PC (volume, sound, camera, network). What earns a pass: a change
  counts only when its call worked (no `ERROR`, not waiting for a yes) and the saved state shows it,
  and a compound request is checked part by part (`must`: the change worked; `tried`: only attempted,
  for routing and failure cases). The evidence is not read from what the answer prints: the `cli` build
  writes a records file named by `ZB_EVAL_RECORDS` (one JSON object per line: turn start and end with
  times, the answer, every tool call whole, the saved state after each turn, errors, `/new`, the end of
  the run), from the turn's own events, so a trimmed history hides nothing and an answer that quotes a
  record changes nothing. The parser checks the records as it reads; anything missing, out of order or
  unreadable makes the run **unscorable**, which is counted apart and never a pass. Saved runs from
  before fall back on the readable lines. Every result carries `case_hash`, `scorer`, `trace`, `set`
  and `outcome`, which the gate uses to compare like with like. `--validate` (here and in
  `agent_suite.py`) compiles every case offline. Both sets run each case with the stand-in music
  library, a fixture camera photo (`tests/mocks/camera/sign.jpg`) and a temporary home. A check looks at
  the last turn, at the one
  `on_turn` names, at `every_turn` (a later turn cannot hide an earlier mistake), or at the whole
  conversation (`chk_moved_item`: one saved, then exactly one at the new time). Numbers keep their
  sign and times their am or pm. A run that times out counts once, in the turn it stopped in; later
  turns are "not started", and the summary gives the completion rate and the time to the first
  sentence (with `--trace`) beside the time to the first text. Every case's web reads (search,
  forecasts, place names, exchange rates, stock prices) go to the stand-in web (`tests/mocks/fake_web.py`
  and its data in `tests/mocks/web/`, through the `cli` build's `ZB_EXP_WEB`, honoured only for an
  `http://127.x` address and compiled out of the device build); `web(fixture)` in a case's env picks a
  fixture other than `default`: forecasts (`rain59`, `rain60`, `rainmissing`), event times (`events`: one
  published, one not announced) and the edges (`search_empty`, `search_error`, `search_irrelevant`,
  `search_conflict`, `search_stale`, `search_long`, `weather_error`, `weather_partial`, `rates_missing`, each
  with a scored case of its own). **Variety, not volume:** every answer is drawn from a seed (values, wording,
  number of results, an unrelated result mixed in; forecasts made up for the place and season; rates and
  prices moved a few per cent), the same for the same seed and request, so checks work the right answer out
  from what was served, never from a number written into the case: `chk_served(rule, slots, how)` (suite
  kind `served`) reads what each search of the conversation served (`annotate_served`: the rule its query
  matched and that rule's facts for the run's seed and date) and requires that evidence: no search, a failed one
  or one for another subject fails, one about the subject that the stand-in matched to another rule is a fixture
  fault (unscorable). It scores meaning, not mention: the winner not as the side that lost, a score with its
  result the right way round, a date for "when", and no other value the slot can take said beside the served
  one (unless set aside in its own clause: "London, not New York"). Only what the model saw counts (`annotate_served`
  keeps a fact when the response shows it: an empty result credits nothing, and "the search did not say" is right
  then), and evidence ends at "/new". `chk_weather_served` (added to every case that must call the weather) fails
  any temperature, both ends of a range, that the forecast did not serve for the day it is said of (the day named in
  its clause, else the last one named, else the day asked about; a rain chance is no temperature; °F is converted); the currency checks require the answer to give the converted amount (rounded as
  speech rounds it) or the rate, in the right currency (its name, code or sign by the number).
  A web read that failed never counts as a `must` call (an expected failure uses `tried`).
  Search answers live in `tests/mocks/web/search.json` (a rule per question: `match`, `slots` with
  `held_out` values, answer and result wordings); add a rule with any case that searches something new.
  **Seeds:** day-to-day runs use one fixed seed (`EVERYDAY_SEED`, or `ZB_EVAL_SEED`/`--web-seed`), so a
  variant and its baseline see the same world; the seed is part of every case's `fixtures` and `case_hash`
  and is recorded in each result (`web_seed`), so results of different seeds are never paired. Seeds from
  1000 up are held out: they draw only held-out values and wordings (every slot has its own, disjoint from the
  everyday ones; `fake_web.pool_problems` checks; nothing falls back), and the search cases ask their held-out
  question forms (`held_out_turns`); the runners take one only with `--held-out` in the release's held-out run
  (`HELD_OUT_SEED`). The stand-in's clock is frozen per run (`--date`, recorded as `web_date` in each turn and
  result) and no served value depends on the date (forecasts for a fixed season, by days from today), so runs on
  other days see the same answers with their dates moved and stay paired; the date is therefore not hashed, except
  for the few cases whose answers do move with it (`date_workload`: a weather request for a weekday or the weekend,
  by how many days ahead it is; the launch case of the events fixture, by its time in London across daylight saving;
  a search rule that counts days from today, such as the Bulls' next game, by the weekdays they fall on), which
  pair only with a run on a day like it. The
  camera picture hashed is the one the cases get (`ZB_EVAL_CAMERA` or the fixture). The fixtures are versioned
  (`fake_web.VERSION`, and every file of `tests/mocks/web/` is hashed into `case_hash`). **The scorer's own test**: `tests/scorer_test.py`
  (offline, no service) feeds deliberately wrong transcripts that must fail and right ones that must
  pass, one per check, each asserting its problem text; run it after any change to a check, and add a
  wrong transcript for any new kind of mistake.
- **Generated request set**: `tests/suite/generate.py` (fixed seed) writes `tests/suite/cases.json`,
  about 570 requests in categories (timer, reminder, calendar, weather, home, route, ...) with
  feature tags (`asr`, `lang-xx`, `uk`, `us`, `followup`, `compound`, `notool`, `clarify`,
  `search`, `camera`). Edit the templates, regenerate, read the diff; `--check` says whether the
  file is current. `tests/agent_suite.py` runs it with the scored set's machinery and the same
  environment: `--list`, `--tags a,b+c` (`+` means both), `--skip-tags`, `--only`,
  `--sample N --seed S` (a few from every category), plus `--repeat`, `--fast`, `--json`. It
  prints pass rates per tag and the slowest cases. Checks are declarative (`"kind"` in the file,
  compiled by `KINDS` in the runner) and accept every right answer: a timer or a reminder for "in
  20 minutes", several right tools, any of the right numbers. A check looks at the last turn
  unless it names another (`"turn"`: a number, or `"all"` for every turn; `moved` looks at the whole
  conversation), and checks of changes read the saved state as the scored set's do (`timers_saved`,
  `events_saved`, `memory`, `memory_lacks`); `long_ok` lists the turns that may answer at length, which
  the owner wants for analytical follow-ups (so `max_words` is for simple factual turns only;
  `max_calls` caps a tool, such as searches in a row). The `dialogue` category holds threads of
  several turns generalised from saved conversations (`tests/suite/from_conversation.py` drafts
  one). Each case gets a small stand-in music library (`MUSIC_LIBRARY` in `agent_suite.py`: tags,
  no sound) unless `ZB_EVAL_MUSIC_DIR` names a folder; set it empty for the empty library that runs
  before 2026-11-19 had (9 music cases then failed on any model). Run a sample or the tags you
  touched, not the whole set, unless asked.
- **Environment switches**: `ZB_DRY_RUN=1` (tools do not play music, send infrared or switch
  devices), `ZB_NO_AUDIO=1`, `ZB_FAKE_CAMERA=file.jpg`, `ZB_FAST_MODE=0|1`, `ZB_DATE_ORDER=dmy|mdy`,
  `ZB_TEST_NETWORK=1` (a stand-in network, id `net-58678b031ae6e411`, instead of the real one:
  only devices at 127.x addresses are contacted and nothing is scanned; use it for any smart-home
  test on a PC, never `--discover`), `ZB_TEST_TETHER=1` (the radio acts as on a phone's
  connection).
- **Measurement switches** (`src/agent/experiment.h`; compiled into the `cli` build only, CMake option
  `ZB_EXPERIMENTS`, which CMake refuses outside the `cli` build): `ZB_EXP_TRACE=file` appends a JSON
  line per model call (purpose, timings, token counts the service reports, size sent) and per request
  (first sentence, tools, guards, second look); no words, keys or model ids. `ZB_EXP_CACHE_BUST=1`, `ZB_EXP_TTS_PROBE=1`.
  `ZB_EXP_OFF=NAME[,NAME]` switches small-model controls off (`src/agent/controls.h`); an unknown name
  stops the run. The variants once measured against the default (`ZB_EXP_TOOLS`, shorter tool text;
  `ZB_EXP_DECIDE`, deciding without the second look; `ZB_EXP_CHECK_MODEL`) were retired, none adopted:
  set, any of them stops the run at start, so an old command cannot measure the default under their
  name. `ZB_EXP_WEB` (the stand-in web) takes only plain decimal `http://127.a.b.c[:port][/path]`
  and sends to the address it read, as `local`: no proxy, no redirect followed.
  **The hardware guard:** `ZB_EXP_NO_HARDWARE=1`, which the live sets' runners set for every case, makes
  the camera refuse unless `ZB_FAKE_CAMERA` names a picture, and the infrared transmitter refuse, so a
  test run never sends a real photo to the service or a code into the room (`tests/tools_cli.py`
  checks it, and the stand-in's proxy and redirect rules). The runners' `--trace` puts each turn's trace
  into `--json`; `tests/exp_report.py` summarises such files (completion beside every latency figure)
  and `tests/exp_compare.py` compares a variant with a baseline case by case and by tag (repeat the same
  cases the same number of times on both sides before pooling), and ends with the gate's verdict
  (above; exit status 0 passes, 1 fails, 3 inconclusive, 4 incomplete). A tag's point estimate alone is
  noise at these sizes. With no `ZB_EXP_*` variable set nothing sent or done changes (checked in
  `agent_loop_test.py`).
- **Screens**: `build/sim/zero_bot --shots out/` and look at the PNGs. Every UI change should
  be checked this way before it goes to the device. The script in `App::render_shots` walks
  the menus by counting key presses, so adding a settings row shifts later screenshots: fix
  the counts in the same change. `tests/shots_compare.py` renders them into a temporary folder
  and compares each with `tests/golden/shots/` (small tolerance; differences drawn to
  `build/shots-diff/`), with the wall clock pinned (`tests/shots/fixclock.c`, built with `cc` and
  preloaded) and stand-ins on `PATH` for the audio, volume and network helpers, so it plays no
  sound and changes nothing on the PC (plain `--shots` plays a chime and sets the system volume).
  After an intended change, `ZB_UPDATE_SHOTS=1 tests/shots_compare.py` and look at every changed
  image before committing. The images depend on the installed fonts: refresh them on a new
  machine first. `tests/golden/shots/masks.json` hides the strip where the now-playing title
  scrolls with real time.
- **A fresh state per test**: point `XDG_CONFIG_HOME`, `XDG_DATA_HOME` and `XDG_CACHE_HOME` at
  a temporary folder. Do the same on the device so the user's calendar and data are untouched.

When a model-facing change is made (prompt, tool description, tool schema, loop), run the
affected cases repeated, then the whole set once. When something cannot be tested (a physical
key, real audio), say so plainly rather than implying it was.

## 7. Working with the device

- **Diagnosing what the assistant did**: the app's output goes to the launcher's socket, not to
  the journal. `~/.cache/zero-bot/activity.log` on the device (`src/core/activity.cpp`) has every
  tool call with its arguments, result and duration, request by request; read it as root over ADB.
  It holds no words of the user's and no secrets; keep it that way.
- **Saved conversations**: SPACE twice on the chat screen writes the conversation so far, as the
  model saw it, to `/home/pi/.local/share/zero-bot/conversations/` (one JSON file per save, named
  by the time). Pull them as root, check the copy, and keep them out of the repository (they hold
  the owner's own words):
  `timeout 60 scripts/adbx pull /home/pi/.local/share/zero-bot/conversations .local-notes/conversations/`,
  then compare `md5sum` on both sides (`scripts/adbx shell 'md5sum /home/pi/.local/share/zero-bot/conversations/*'`).
  `tests/suite/from_conversation.py FILE` prints a draft multi-turn case from one; generalise its
  words (other names, other events) before it goes into `tests/suite/generate.py`.
  Both records are diagnostic and temporary: `src/core/activity.h` lists every call site, each
  marked `DIAGNOSTICS (temporary)`, for removing them (ZB-052).

- The device is reached over USB ADB as root: left-side switch on Slave, cable in the
  right-hand USB-C port, ADB enabled in the device's Settings > Developer. Use `scripts/adbx`
  (it recovers an "offline" link). With a phone also attached, set `ANDROID_SERIAL`.
- **The link drops under load and can corrupt transfers.** `scripts/deploy.sh` verifies an MD5
  after each push; do the same for anything else you copy. Wrap device commands in `timeout`.
  A long-running test should be started detached (`setsid nohup`) and polled.
- Run things as the app's user: `sudo -u pi env HOME=/home/pi XDG_RUNTIME_DIR=/run/user/1000 ...`.
  NetworkManager actions are only authorised from the user's own service manager.
- Installing replaces `/usr/bin/zero_bot`; a running copy keeps the old code until the user
  leaves (hold ESC) and reopens it. Do not restart the app or inject keys while the user is
  using it; check `pgrep -x zero_bot` first.
- Never use `pkill -f` or `pgrep -f` with a pattern that appears in your own command line: it
  kills your own shell. Find processes by exact name or by listening port.
- This development PC's firewall drops multicast replies, so smart-home discovery finds
  nothing here. Run `zero_bot --discover` on the device.

## 8. Lessons learnt (each of these cost real time)

**Networking**
- Name lookups took exactly 5 s each on the device: the C library asks for IPv4 and IPv6 at
  once over one socket and the second reply was lost. `main()` sets
  `RES_OPTIONS=single-request` before anything resolves. Other programs run on the device
  (curl, python) need the same or they look like a slow service.
- An outage page from a proxy is HTML, not JSON. `llm.cpp:api_error` turns statuses into plain
  sentences; errors the user must read are shown with `Menu::problem` (stays until dismissed),
  not a toast.
- On a phone's Bluetooth tether uploads run at about 10 kB/s. Speech is sent as MP3 and replies
  fetched as Opus for that reason. The tether is never a "home network": the smart-home tool
  and scan do not exist on it. Nor does a phone's USB tether, which can be named like a wired
  port (an iPhone's is `eth0`): `discovery::link_kind` decides by the interface's driver, not its
  name. `home::network_now` checks the default route and the router's hardware address on every
  call, so a move to a tether takes effect at once rather than when a cached answer expires; a
  router whose hardware address the system has not learnt yet counts as a move (many home routers
  use the same IP address). A command for several devices asks again before each driver call,
  because it can outlast the Wi-Fi.
- The Wi-Fi chip's firmware ignored the country code, hiding channels 12 and 13;
  `docs/wifi-channels-12-13.md` has the device-tree fix.

**Audio**
- The music player's default buffer is about 12 ms and skipped whenever the processor was busy.
  It is started with `PULSE_LATENCY_MSEC=300`. Synthetic tests (silent playback while counting
  graph errors) did not show the problem; recording the speaker through the microphone and
  comparing with the source did.
- Recording from "the default input" let the system switch a connected Bluetooth headset into
  phone-call mode, which is silent on this hardware in both directions. The recorder asks for
  the built-in microphone by name.
- Music must not start before the spoken reply, nor resume between a question and its answer.
  That is what the hold flags are for.
- `pw-cat` raw input counts underruns while its source is slow to start; that is not a fault.
- Music skipped from the second track on: each track was an ffplay of about 53 MB (it loads the
  whole video and audio library; ffmpeg is the same) on a device with 350 MB and some 200 MB already
  in swap, so the system swapped in bursts and the player stalled. ffplay then hands the sound graph
  silence, so `pw-top` counted no dropout: measure the player's memory and the swap (`vmstat`), not
  only the graph. MP3s are now decoded in the app (libmpg123, loaded at run time) and played by
  `pacat` (about 8 MB in all; 0 gaps and almost no swap in a 90 s test); `pacat` goes through the
  PulseAudio layer as ffplay did, so a held track (SIGSTOP) stalls nothing else. Never record the
  speaker while the owner's music plays (it is licensed): test with a generated tone, and only when
  nothing else is playing; and do not poll `pw-top` during a test, it disturbs playback itself.
- The sound graph runs in the smallest chunk any client asks for. The launcher keeps a stream open
  at about 5 ms, which pulled the graph down to 128 samples (2.7 ms): music skipped whenever the
  processor was busy, whatever latency the player itself asked for (`pw-top` showed it, ERR
  counting dropouts). The app cannot raise it; a device-wide `default.clock.min-quantum = 1024`
  can (README, Troubleshooting), with the owner's agreement. Look at `pw-top` first when sound skips.
- With a plug in the 3.5 mm jack the board's driver moves the codec's input to the jack's
  microphone pins (PGAL/PGAR Select Line 2P/2N); a plug without a microphone then recorded silence.
  The recorder points it back at Line 1P/1N for each recording (`audio::detail`), and puts it back.
- Radio: an HLS master playlist can point at variants that carry hours of rewind. One public
  broadcaster's was a ~200 KB playlist that a player re-reads every 6.4 s segment, more data than
  the sound itself; its live-only variant of one quality was 600 bytes. The README tells users to
  pick such an address. A live stream is never suspended under a hold, it is ended and joined
  again (`Music::play_live`): a stalled connection is dropped by the server, and resuming would
  play stale audio. ffplay refuses to start on an option nothing consumed, so add player options
  only after checking them with `ffprobe` (`-rw_timeout` is safe for any http(s) address).
  A stream never ends by itself, so its player is started with `proc::spawn(..., die_with_parent)`:
  without it, an app that crashed or was killed left the station playing with nothing to stop it.
  `Music::play`, `play_live` and `stop` are called from the UI thread and the agent's at once
  (fn+E while the radio tool starts a station); unserialised, that crashed in a stress test.
- ffplay cannot open a `.pls` or a plain `.m3u` playlist file ("Failed to open file", "Invalid data
  found"; an HLS `.m3u8` it can), and the README promises both work. Every way of starting a
  station goes through `radio::check`, which follows the playlist to its stream: the radio tool
  directly, fn+W/fn+E and the settings page through `radio::check_async` (off the UI thread). The
  first version of the keys and the page gave the address straight to the player, so a playlist
  station played when asked for and failed from the keys, blamed on the station.
- ICY `StreamTitle` on the stations tried was empty or carried advert markers, so "what's on" is
  left to web search rather than read from the stream.

**The model**
- Turning reasoning off made requests about 2 s faster and wrong about a fifth of the time
  until the second look and the guards went in. Measure accuracy and latency together.
- A prompt rule alone was not reliable for numeric dates or for ages; doing the work in code
  and handing the model the answer was. Field names matter: `years_since_first` was misread
  as an ordinal, `age_on_that_day` was not.
- New wording in one tool description changed behaviour elsewhere (the assistant began asking
  to confirm misheard words). Re-run neighbouring cases, not only the ones you touched.
- A "past" escape-hatch parameter on the calendar tool was used by the model to bypass the
  check it was meant to refine. Do not give the model a way round a guard.
- Give tools the allowed names as an `enum` (smart-home devices) rather than describing them.
- A rule in a tool's description is not always heeded where it matters, and words added to a
  description move routing elsewhere. tv_remote's said "if a listed TV does not answer over the network,
  say so rather than trying this", yet after every network error of a listed TV (35 runs in 35) the reply
  offered the infrared remote, and a "yes" then sent another TV's codes (ZB-072). The rule added to both
  tools' descriptions cut the offers to 3 in 55, but drew TV key presses ("press ok on the tv") to the
  infrared remote (5 runs in 50, none in 30 before), and naming tv_remote there made replies tell the user
  to "try the tv_remote tool". Said once in the TV's failure result (`control.cpp`), where it applies, it
  left the prompt and tools byte for byte as they were: 2 offers in 55 and no infrared call.
- The model's own weekday arithmetic slips: on a Wednesday, "every Tuesday" was saved on Monday 23
  November six times in six while the confirmation said Tuesday. The calendar and reminder tools
  refuse a call whose `say` names one weekday the date is not (`weekday_slip`), naming the nearest
  date that is; the model corrects it at once. Check what a confirmation claims against what is saved.
  Likewise a cancel or delete whose `say` claims something new: "actually make it 10" got
  reminder_cancel alone, saying "I'll remind you at 10 instead", three runs in three (suite case
  reminder-028), and nothing was set. Such a call is refused (`claims_new`) unless something was set
  or changed earlier in the request (`Context::worked`).
- Text that came from the network or a device (names, descriptions) is untrusted: strip
  control characters, cap the length, pass it as data.
- Models send `1e300`, `"nan"` and numbers where text was meant. Converting a double that is not
  finite or not in range to an int is undefined (it crashed `date_info`), so the tools' `num_arg`
  takes finite values only and `clamp_int` brings them into range first; text arguments accept a
  number as its digits. A number sent as text is read as a whole (`util::to_number`): `strtod`
  stops quietly at the first character it cannot read, so `"id": "12oops"` once cancelled reminder
  12. Nested brackets overflowed the calculator's stack: recursive parsers need a depth limit.
- `mktime` makes 31 February into 3 March without a word, and the zone database holds folders
  named like countries ("Australia") that the C library reads as UTC: `parse_local` and
  `timezone_exists` refuse both. Check that a date, time or zone exists before using it.
- nlohmann::json: a function taking `const json &` called with `{x}`, where `x` is already a
  json value, gets a copy of `x`, not a one-element array (nested lists do become arrays). Build
  lists as `std::vector<json>` or `json::array({...})`. It also sorts object keys when dumping.

**The loop**
- Requests chained inside one run shared one budget of rounds and the last was silently
  dropped. A follow-up now gets a fresh budget, and running out is spoken.
- Anything "known" about the world goes stale: the calendar refuses one-off events in the past
  because a date said without a year means its next occurrence.
- The chat call's own retry counted a timeout with no answer yet as "worth another try", so one
  request could wait 180 s three times. Retries now share one rule (`http::transient`) and one
  budget (`http::send_retrying`). Whether a request can have reached the peer is `Response::sent`
  (curl's request size or connect time); curl's pre-transfer time is no guide, it is set for a
  refused connection too.
- A model whose call failed tends to send the identical call again, and a change that timed out
  may have happened: sent twice, a volume step is two steps. The loop does not make a failed call
  again unchanged until the user has spoken; ask "could this happen twice?" of anything that
  resends.

**Devices and protocols**
- Tapo plugs lock out after rejected logins. The transport never retries a rejected login,
  checks the account e-mail against the plug's announced owner before trying, and is tested
  against a simulated plug (`tests/mocks/mock_tapo.py`), never by trial and error on real plugs.
  The transport only remembers a refusal while the program runs, so the driver keeps it in the
  file (`secrets.tapo.refused`) until a password is typed again: one wrong password, one attempt,
  however often the app is restarted.
- The shared layer saves `Device::auth` after a pairing step and a successful read, not after a
  failed one, and `read`/`apply` get the device read-only. What a driver learns from a failure that
  must survive a restart (a key the device rejected) goes in the family's secrets, keyed by device
  id, as the Hue driver does (`refused`, `cert_changed`); its first version kept it in `auth`,
  where it was lost.
- A scene switches everything it holds, and the shared layer sees only the scene. The Hue app puts
  a room's smart plugs in the room's scenes, so starting one switched on a plug marked *Ask before
  switching on* (a heater) without the question. A driver offers no scene that switches on a plug
  or socket, and reads the scene again right before recalling it: the list is read only when the
  user asks, and a scene edited in the maker's app since (a plug added) was recalled unchecked. It
  also judges the scene against `Device::things`, which carry the user's *Ask before switching on*
  and *enabled* settings, so a scene never switches on what the user has asked to be asked about.
- Smart-home discovery uses only what the devices' own apps use (mDNS, SSDP, the vendor's
  broadcast). No address sweeps, no port scans, opt-in per network with a warning. "This
  network" is the SSID plus the router's hardware address, not the SSID alone.
- A change to discovery's ranking can give a saved device a new id (a Kasa EP25 saved as a
  HomeKit accessory now answers as Kasa). The new record must not reopen what the user closed on
  the old one ("Assistant may use it" off, "Ask before switching on"): `remember_scan` adds a
  record whose MAC another record already has with the assistant's use switched off, when that
  other record holds a decision (things read from it, or its use switched off). One that only
  listed the device (a Hue Bridge heard only as HomeKit, which no driver claims) holds none, and
  must not leave the right record switched off.
- An address is not an identity. Routers hand a device's address to another one (a router that
  loses its leases when it restarts, a bulb switched off at the wall for a week), and a device
  with no key of its own (WiZ, and any family whose protocol has no login) answers whoever asks.
  The first review of the WiZ driver found that a command for a light switched the plug that now
  had its address. Such a driver checks the device's MAC (the one discovery saved it under)
  before every change, and fails with a word `unreachable()` knows so the app looks for it.
  A login is no identity either: every plug of one Tapo account accepts the same password, and
  until the integration review a command for a desk lamp switched on the heater plug that had
  been given the lamp's address, with no question. The Tapo transport now asks each new session's
  plug for its MAC (`tapo::Device::mac`) before sending anything else in it.
- A default that reads a device's name must also read the name the user types. WiZ devices keep
  no name, so the heater question (on for a plug named like a heater) never applied to one until
  `set_thing_alias` learnt to switch it on for a name that newly sounds like a heater.
- Before promising a device can be controlled, check whether it has a local interface at all.
  Several popular products are cloud-only.
- Judge a change by what it needs, not by everything the device will do next. The owner's Fire TV
  took the DIAL wake and came on, but its control API accepted connections without answering for
  longer than the 2.5 s change deadline while it started; waiting for the API turned a power-on
  that worked into "not replying", and the assistant offered the infrared remote (a power toggle).
  Power-on alone is now judged by the DIAL answer, and a command in the minute after a wake says
  the TV is still starting up (`firetv.cpp`, `detail::power_on_done`, the `starting_` memory).
  A wake sent to deliver a command can be the opposite of what was asked: "off" used to wake a TV
  whose API refused so as to send it the sleep key, and from deep standby that wake switched it on.
  By the owner's rule "off" now wakes only a TV heard from or woken in the last minute
  (`FireTvDriver::apply` and `press`, `on_lately`), for every part of the change. Its "off already"
  first said "did not answer", so the app searched the network for 2.5 s for a TV in standby; a
  refused port is the device there with the port closed (`Reply::refused`, errno ECONNREFUSED),
  and only an attempt that timed out reads as "did not answer". In deep standby the owner's TV lets
  the port time out, so the search still runs (a TV that moved and is on can still be switched off);
  for an off change a device found nowhere "seems to be off already" (`after_lookup`), not "switched
  off at the wall, or out of Wi-Fi range", which the model passed on as a fault (ZB-075). A driver's
  error is joined to more words without its closing full stop (`clause`).
- The keyboard's three lights (caps lock, fn, symbols) have no kernel interface: the keypad takes no
  EV_LED events and they are not LED-class devices. The M5Stack keypad driver sets them by writing a
  mode (0 off, 1 blink, 2 fast, 3 on) into registers 0xBD to 0xBF of the M5IOE1 expander, the
  chip that also holds the power rails, RESET and the power-off timer. Writing those registers from
  the app (over `/dev/i2c-1`, forcing the address the expander's driver holds) works, but the owner
  ruled it out: the app does not animate the lights until the keypad driver offers an interface
  for it.
- New device code is reviewed before it touches a real device: in this project the first
  independent review of each driver and of the smart-home layer found blockers every time (a
  login that would lock out every plug of a second account, a confirmation the model could skip,
  a discovery port that sent pairing to the wrong service). A second review found only smaller
  problems; two rounds and a final check was a sensible stopping point.
- libcurl's digest authentication (`http::Request::digest_user`) sends a POST first with an
  empty body, to draw the device's challenge, and only then the body with the credentials. A
  device that asks for no password takes that empty request as the call. The Shelly driver
  therefore makes its digest calls as GET (Shelly's `/rpc/<Method>?<params>`); a mock that
  parses the body before checking the login hides the problem.
- A garage-door exclusion that read only the channel's name missed the usual set-up: the owner
  names the device ("Garage door") and leaves its relay unnamed, and a Gen1 blind has no name at
  all. Check the name the thing will be called by, and what the wiring says (a relay that switches
  itself off within seconds is an opener's pulse, whatever it is called).
- Ask a device who it is before anything that carries a password, not only before a change: after
  an address change another device of the same family gets the login (in the clear, with HTTP
  basic), and its refusal is kept against the right one, which typing the password again cannot
  clear.
- Tie a password to the device that accepted it when it is typed (Shelly: `auth.dev`), not only
  to the things read from it later: a paired device that is never listed (a garage door with a
  password has nothing to offer) has no things to tell it apart, and its password went to whatever
  answered at its old address.
- A safety rule applied when a device is listed goes stale: the app reads names and settings again
  only when asked to in Settings, and the owner can rename a relay "Garage door" or rewire it as an
  opener in the maker's app at any time. Where the rule depends on such settings, ask again just
  before the change (the Shelly driver reads the channel's settings before switching a relay or
  blind).

**The screen**
- A label sized by LVGL from its text (width `LV_SIZE_CONTENT` with a max width) left out its padding
  when measuring, so a line that fitted the max width but not the max width less the padding wrapped
  onto a second line with no room: the chat bubble showed "...cybersecurity" for "...cybersecurity
  news", and the user took it for a speech-recognition fault. Size such labels with
  `theme::fit_width` after setting their text; `--shots` checks a bubble of that length with
  `theme::label_fits`. When a word seems to go missing, the activity log's tool arguments show
  what the model actually received.
- Screen off (`display::screen_off`): the panel's own blanking is not used. Its driver
  (panel-mipi-dbi, write-only ST7789V) only switches the backlight off for it, and resets and sets
  up the panel again on every wake because it cannot read back whether the panel is on. The app
  draws black once, then stops LVGL drawing: invalidation off, and the display's refresh timer
  given a callback that does nothing (LVGL resumes that timer for any change of layout, a label's
  new text, even when nothing is to be drawn). Waking marks the whole screen, refreshes once, then
  switches the backlight on. `--shots` checks the frame itself: black, unchanged while off, and
  exactly what the objects show after a waking key, which does nothing else. The launcher does not
  put the backlight back when an app ends, so while the screen is dimmed or off a fatal signal
  writes the brightness setting back (`display::restore_after_crash`, only calls safe in a signal
  handler) before the process ends as it would have. A modifier tapped on its own, and a key the
  app has no use for, reach `handle_key` as a `Key::None` press (`LoneModifier`): it wakes the
  screen and does nothing else.

**Power**
- The fuel gauge (`/sys/class/power_supply/bq27220-0/`) reports `current_now` in µA: positive
  while charging on USB, negative on battery, 0 only when full on USB. To measure the app, log it
  every 10 s from a detached process (`setsid nohup ... &`), unplug, use the device, plug back in
  and read the log. Plugging USB back in restarts adbd, which kills processes started from an ADB
  shell; that only ends the log.
- Measured: the platform (processor, Wi-Fi, audio service, launcher) draws about 260 mA with the
  screen off; Zero Bot idling with a dimmed screen adds only about 15 mA, and answering takes it
  to about 330 mA. Battery work inside the app has a small ceiling; device-wide settings matter
  more, and change the whole device, so they need the owner's agreement.

**Process**
- Do not report something as fixed when only the mechanism was fixed and the symptom was not
  reproduced; say which it is.
- A wrapper that "recovers" a shared resource by restarting it (the ADB server) breaks other
  work using it. Prefer the narrowest recovery.
- Temporary folders are cleared; anything worth keeping goes in the repository or in
  `.local-notes/` (git-ignored, may hold private network details, never to be published).
- Long sessions are interrupted (usage limits, dropped links). Save as you go: briefs, status
  files and results on disk, not only in an agent's context. `.local-notes/PROGRESS.md` holds the
  running state; the local ticket tracker (section 10) holds the plan, and a ticket that is not
  finished links to its research, briefs and partial work so anyone can carry on.
- When several agents share this PC: compile through `.local-notes/tools/buildlock` (four
  compiles at a time, memory is short), build a checkout only with `.local-notes/tools/repobuild`
  (one build per checkout), and give test servers ephemeral ports.
- Never edit the test scripts while a live run is going: a result's `case_hash` fingerprints its checks from their
  source on disk, so an edit part-way through crashed the scored set's held-out run at its last case and lost
  three repeats (40 minutes). Run what changed afterwards, or in another checkout.
- A multi-agent workflow resumed after an interruption reuses finished steps only when it makes
  the same calls in the same order. To continue with a changed plan, save the finished results to
  a file and start a new run from there.
- Make release builds in the main checkout. A worktree reaches LVGL through a link to the main
  checkout's `.cache`, which lies outside the worktree's `-ffile-prefix-map`, so a device binary built
  in a worktree contains a path of the build machine (with the user's name). The `strings` scan
  before a release catches it.
- A sandbox does not close what it inherits: `bwrap` handed the app's open descriptors (the
  framebuffer among them, which LVGL opens without close-on-exec) to the sandboxed Python.
  `proc::spawn` closes everything above stderr in the child; keep it. Anything slow that a tool
  starts takes `ctx.cancel` (`proc::run`'s last argument), so ESC stops it.

## 9. Adding things

**A tool.** In `src/agent/tools.cpp`: write `t_name(const json &, Context &)` returning text or
JSON for the model (use `fail("...")` for errors, phrased as an instruction about what to do
next); add it to `build()` with a description written for the model and a compact schema (the
runner checks argument types against it before the tool runs, so the tool keeps only its domain
rules; a reply that reads from the network uses `fetch_json`, which retries what is safe); add
it to `kConfirmable` if success is fully known from the arguments. If it needs the screen or
hardware, add a callback to `tools::Context` and an event in `AgentEvents`, set in
`Agent::run_turn` and handled in `App::wire_agent` through `uiq::post`. Honour `dry_run()`.
Add cases to `tests/agent_eval.py`, including a neighbour that must not trigger it. Document
it in `README.md`.

**A setting.** Add the field to `Settings` (`src/core/settings.{h,cpp}`: member, `to_json`,
`from_json`), a row under its section's header in `open_settings` (each header is a page of its
own, opened from the top page; a new header is a new section), the setup guide if a new user
needs it, and a line in the README's settings table. Fix the screenshot key-press counts. Rows read
the value with `S()` and change it with `change(...)` (`Settings::update`), never by writing to a
copy.

**A key.** Chat-screen keys outside the input line are decided in `keyguide::chat_key`
(`src/ui/key_guide.cpp`); add the key's line to the guide there too (`H` shows it), and keep the
bottom row's hint short: it names only the keys wanted most.

**A screen.** Follow `breath_view` or `chat_view`: create objects once, update only what
changed, drive it from `App::periodic`, route keys in `App::handle_key`, add a `--shots` frame.

**Something shown over every screen** (the timer's alarm banner, `AlarmView`): make it a child
of the screen, created after the other views, and `raise()` it again whenever another view
brings itself forward (`BreathView::start` does). Not `lv_layer_top()`: `--shots` snapshots the
screen only, so it would be missing from the screenshots.

**The status bar** is redrawn by anything that sets one of its labels or colours, even to the
value it already has. `ChatView`'s setters compare with what is shown and return early; keep
that for anything added there (the timer countdown changes once a second, not every tick). A
label that scrolls redraws the bar on every screen refresh (about 40 times a second, measured in
the simulator), so nothing there should scroll for long: the now-playing text scrolls only when
it has all of its room, and beside the countdown it is cut short with dots instead. A radio
station's name, there for hours, never scrolls (`set_now_playing(text, true)`).

**A settings value that changes by itself** (a timer's time left): set `MenuPage::live`.
`Menu::tick` then reads the values on screen 15 times a second and redraws only those whose
text changed, so their value functions must be cheap: no file reads, no subprocesses. Rows that
come and go by themselves (a timer ends) also need `MenuPage::revision`, a number that changes
with them: the page is rebuilt when it does.

**A sound that repeats** (the timer's alarm): `audio::AlarmSound` gives each ring a player
process and a thread of its own, so `start()` and `stop()` never wait on the UI thread, not even
for a player that has hung (`Speaker::kill` ends it without waiting; its thread reaps it). It
holds back while the assistant's voice is audible (`SpeechQueue::playing`, not `active`, which
also covers waiting for the service) and for a few seconds at most. Stop it on every way out,
leaving the app included (`stop_and_wait`): a player process outlives the app. Whatever holds
the music (`audio::Music::hold`) lets go on every way out too.

**Wall-clock time in a countdown** (timers): a board without a battery-backed clock is set by a
time sync some while after it starts, so the wall clock can jump while the app runs.
`Store::follow_clock` moves the timers set in this run along with it (the app calls it every
tick, and uses the time it read for the rest of the tick); anything else counted down from "now"
needs the same care. A time read on another thread may be from the other side of a step the main
loop has yet to see, or has just seen: the timer tool reads both clocks and passes how far apart
they were (`Store::reckon`, `add_timer`), so a timer started just then is moved across the step
once, not twice and not never.

**A smart-home device family.** It is a driver: a class implementing `home::Driver`
(`src/smarthome/driver.h`) in `src/smarthome/drivers/<family>.{h,cpp}`, which defines
`std::unique_ptr<home::Driver> make_<family>_driver();`. Registering it is one line,
`X(<family>, "<Name>") \`, in the list in `src/smarthome/drivers/registry.cpp`; the name is what the
settings pages call its devices. Discovery must report the family's devices under the kind its
`claims()` accepts (`kServices` or a vendor query in `discovery.cpp`). Nothing else changes (CMake
and `tests/run_unit.sh` both build every `.cpp` in `src/smarthome/drivers/`): the
shared layer does names, rooms, the tool's schema (it grows with the capabilities a home has),
the safety questions, rate limits, locking, fan-out and the generic pairing pages (after a
pairing step, the family's other devices are asked again, so an account typed once serves them
all). The rules a
driver keeps: map only allow-listed types (lights, plugs, switches, thermostats, covers, fans,
speakers, TVs, scenes, read-only sensors; never locks, garage doors, alarms, cameras, valves or
cooking appliances); state in the units and keys documented in `home.h`; secrets only in
`Device::auth` or `secrets()`, never in errors; never retry a rejected code or password, even in
a later run; no retries of its own of a change (the shared layer resends one only at a device's
new address, and only when it comes out the same if made twice; `http::send_retrying` is for
the internet, not for devices); an error for a device that did not answer reads as one ("did not
answer", "timed out", "could not connect"), which is what makes the layer look for it at a new
address (a device that spends half its 2.5 s and then did not answer is not asked again for its
other things in that command: the shared layer gives them the same answer); honour
`Device::port`; deadlines of about 2.5 s for a change and 4 s for listing;
no threads, sockets or connections of its own between calls; errors are short sentences. Use
the `http::Request` options (`local`, `timeout_ms`, `insecure`, `pinned_pubkey`, `want_peer_cert`,
digest, client certificate) and `color.h` rather than private copies. Set `local` on every
request to a device (the TLS and digest options imply it): it never uses a proxy from the
environment, returns a redirect instead of following it (a key in a header must not travel to
another host), and speaks only http and https. Build and
test the protocol against a simulated device in `tests/mocks/` first, with an end-to-end test
like `tests/tapo_e2e.py` on `ZB_TEST_NETWORK`, and add the family to the README.

**Releasing.** Bump `project(... VERSION ...)` in `CMakeLists.txt`, build all three presets,
run the full agent set, `scripts/package-deb.sh`, scan tracked files, the binary and the
package for anything private, then commit, push and attach the `.deb` to a GitHub release.

## 10. Planned work

Plans are kept locally, not in this repository. On the maintainer's machine, `.local-notes/TICKETS.md`
(git-ignored) holds the sprints, a board and one entry per ticket; if it exists, read the tickets
for the current sprint before starting, keep their status current while you work, and check the
file with `python3 .local-notes/tools/tickets.py check`. Lessons worth keeping belong in section 8
of this file.
