# Guide for coding agents

This file is for an AI coding agent (or a new developer) picking up Zero Bot. It explains what
the app is, the hardware it runs on, how the code is laid out, how to build, test and deploy,
and the mistakes already made so that they are not made again. `README.md` is the user-facing
description; read it first. `docs/agent-architecture.html` has diagrams of the request loop
(drawn before the November 2026 refactor: some of the names and files it cites have moved since;
sections 3 and 4 here are current).

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
src/main.cpp        entry point and the command-line test modes (tests/README.md): one row of kOptions
                    per option, and a function and one row of kModes per mode
src/agent/          the request loop (agent: Agent::Turn and its steps), what the loop checks a
                    reply for and the notes it sends the model (guards), the small-model controls
                    (controls), service client (llm; reading the reply stream: llm_stream.h),
                    system prompt (prompt), sentence queue for speech (speech), the cli build's
                    measurement switches (experiment), arithmetic (calc), and the tools: tools.h
                    (Context, Tool, Say) and tools/, one file per tool (<name>.cpp), registry.cpp
                    (the list in the order the model reads it, the schema, "say" confirmations),
                    run.cpp (a call checked against its schema, run, its failure shaped),
                    common.h (argument and schema helpers), say_checks (weekday_slip, claims_new),
                    web (fetch_json, with its options: Fetch, Fetched), places and events (what several
                    tools share); and web search in search/: provider.h (the contract every search
                    service implements, like smarthome/driver.h), providers/ (one file per service;
                    registry.cpp lists them), search.{h,cpp} (which service answers, from the user's
                    list in Settings::search, and the failover between them; what the model reads, the
                    same whatever the service; the ten-minute cache; how each service stands, for the
                    settings page), usage.{h,cpp} (the allowance file: searches counted per service and
                    per day, what each service said is left, spent allowances, the daily cap)
src/core/           settings, calendar/reminder/timer/memory store, timers (the timer tool, which
                    timer words mean, the alarm's logic), music library, radio (the user's station
                    file, the radio tool, the check of a stream), breathing timing,
                    phone tethering, what an upgrade leaves behind (upgrade), the one failure
                    shape (result.h: fail, failed, words), the UI queue (ui_queue.h), the
                    diagnostics (activity; temporary), utilities (dates, files, strings)
src/net/            libcurl wrapper: DNS answers and TLS sessions shared by every thread, connections kept by
                    each thread for its next request
src/platform/       display, keyboard (input, keys.h), audio (recorder, speaker, music), codecs,
                    camera, infrared, subprocess helpers (proc), the battery (power), the Wi-Fi
                    network and the default route (net_status), the monotonic clock (sys.h)
src/smarthome/      discovery (mDNS, SSDP, TP-Link and WiZ broadcasts); driver.h, the contract every
                    device family implements; home.h, the layer above it: store.cpp (the 0600
                    file, migration, the network gate), targets.cpp (names, rooms, groups),
                    control.cpp (commands, safety rules, locking, status, refresh, pairing),
                    tool.cpp (the smart_home tool); color.h (colour names and conversions);
                    drivers/ (one per family; registry.cpp lists them; support.{h,cpp}, the
                    helpers they share); Tapo transport (tapo)
src/ui/             the app controller: app.cpp (wiring, the agent's events, recording, the
                    first-run guide's start, run_app and the main loop), app_keys.cpp (what each
                    key does), app_status.cpp (footer and status bar, timers and the alarm banner,
                    reminders, the screen's light, the periodic work), app_impl.h (the App struct,
                    shared by those and shots.cpp only), shots.cpp (--shots, one function per
                    group of screens); chat screen (chat_view), menu system (menu); settings
                    pages: settings_pages.cpp (the top page and sections, the status page, the
                    key guide, pick_model), settings_data.cpp (Your data), settings_home.cpp
                    (Smart home), settings_search.cpp (Web search, built from the search services'
                    registry), setup_guide.cpp (the first-run guide), settings_internal.h (what
                    they share: S, change, background); models (the service's model list and how
                    models are chosen: without the user only one labelled "chat" becomes the
                    language model, ZB-088, the owner's rule; by hand the settings list those
                    labelled for the use, else the unlabelled ones, else all); the keys and their
                    guide (key_guide), when the screen dims and goes off (screen_idle), breathing
                    screen, alarm banner, theme
tests/              run.py, the entry point that runs the tests in groups by profile (section 6), and
                    netguard.py, the network boundary of every test process; unit/ (the C++ unit
                    tests, built and run by run_unit.sh), sim/ (offline end to end against simulated
                    devices and services: each driver's e2e, the request loop against scripted
                    chat services, the offline tools, the package, the golden screenshots in
                    sim/screens/), agent/ (the live sets: the scored set, the generated suite in
                    suite/, the scorer's own test, the gate and the reports), tools/ (the stand-in
                    web against the real services), mocks/ (simulated devices, chat services, the
                    stand-in web, fixtures), lib/ (code they share); tests/README.md says what each is
scripts/            toolchain setup, deploy over ADB, .deb packaging, ADB wrapper
packaging/          the launcher entry and icon, Zero's picture for the launcher's wake screen
                    (lockscreen.png), and the package's maintainer scripts (preinst, postrm)
```

Threads: the UI thread runs LVGL and the main loop in `ui/app.cpp`. The agent has one worker
thread (`Agent::worker`), speech has one (`SpeechQueue::run`), keys are read on one, music has
one per queue. Anything a worker wants on screen goes through `uiq::post()`; never touch LVGL
from another thread. Slow work started from a menu uses `background(work, done)` in
`settings_internal.h`. The settings are read on every thread and changed by the UI and by tools:
`Settings::get()` returns a snapshot (a read-only copy taken under a lock), and every change goes
through `Settings::update([](Settings &s) { ... })`, which makes it under that lock and saves.

| Thread | Started in | What to know |
|---|---|---|
| UI (main) | `ui::run_app` (app.cpp) | LVGL, keys from the reader's queue, `App::periodic` (app_status.cpp), whatever `uiq::post` queued. Between passes it sleeps in `uiq::wait` until LVGL's next timer or the app's next periodic work (`App::sleep_ms`); a key or a post wakes it at once. Never waits on the network or a child process. |
| Agent worker | `Agent::start` (`Agent::worker`) | One request at a time (`Agent::Turn`); `Agent::mtx_` guards its inputs, history and state. |
| Speech | `SpeechQueue::run` | Synthesises and plays sentence by sentence; a watcher per sentence lets `stop()` cut a synthesis short. |
| Key reader | `input::start` (device only) | Reads the keypad and makes the auto-repeat; its queue is drained by the UI thread. |
| Recorder, music | `audio::Recorder`, `audio::Music` (and a feeder per MP3, `run_live` for the radio) | `Music::ctl_` serialises `play`, `play_live` and `stop`, which the UI and the agent call at once. |
| Short helpers, detached | alarm rings (`AlarmSound`, one per ring), chimes, `set_volume_async`, the microphone lookup and the jack's put-back (audio.cpp), `radio::check_async`, `background()`, the app's thumbnail, model choice and phone link (app.cpp), `upgrade`'s reload | Each hands its result to the UI through `uiq::post`, or touches nothing shared. |
| Smart-home fan-out | `run_parallel` in `home::execute` (control.cpp) | Joined before the command returns; one lock per device (`device_lock`), so two commands never talk to one device at once. |
| cli build only | `main.cpp`'s talk-over and follow-up helpers, the experiments' TTS probe | Not in the device build. |

Locks to know before adding shared state: the settings (`g_mtx` for the snapshot, `g_file_mtx` for
one save at a time; settings.cpp), `Store::mtx_` (calendar, reminders, timers, memory: the UI reads
them every tick and tools change them), the smart-home file (`store.cpp`: `g_mtx` for the file,
`g_net_mtx` for the network last seen), `control.cpp` (`g_ctl_mtx` for the questions awaiting a yes
and the rate limit, `g_locks_mtx` and then one lock per device), web search (`search.cpp`: `g_health_mtx`,
`g_cache_mtx`, `g_asked_mtx`; `usage.cpp`: `g_mtx` for the allowance as read, `g_file_mtx` for one save at a
time, written outside `g_mtx`, which the settings page takes too), the time zone (`g_tz_mtx` in
util.cpp, under which `TZ` is switched), `uiq::mtx()` and curl's share locks (http.cpp), which guard DNS answers
and TLS sessions only: no connection is shared between threads (section 8, Networking). A
descriptor or pointer read under a lock must not be used after it is let go if another thread may
close or free it meanwhile (section 8, Audio). `Store` writes no file with `Store::mtx_` held, which
the UI thread takes every second: it queues a file's new contents under it and writes them once it is
let go, before the change returns (a `WriteAfter` declared before the lock in every function that
saves; `write_mtx_` makes writers take turns). Still open: `TZ` is switched with `setenv` while other
threads may read it.

State on disk (all under the user's home, mode 0600 where they hold secrets):
`~/.config/zero-bot/settings.json`, `~/.config/zero-bot/radio.json` (the user's own stations,
written by hand; the app ships none and never writes it), and in `~/.local/share/zero-bot/`: `calendar.json`,
`reminders.json` (running timers too, as records with `"kind": "timer"`), `memory.json`,
`ir_learned.json`, `music_index.json`, `smarthome.json`, `search_usage.json` (web search's counts and
allowances: no query, no key), and `conversations/` (saved with SPACE
twice; diagnostic, temporary, see section 7).

## 4. How a request flows

1. `App::handle_key` (`src/ui/app_keys.cpp`) starts or stops the recorder (TAB) or collects typed
   text, then calls `Agent::submit_audio` / `submit_text`.
2. `Agent::run_turn` (`Agent::Turn` in agent.cpp: `run`, then `one_round` for each model call,
   whose body lists its steps: `messages_for`, `call_model`, `withdraw_if_steered`, `cut_short`,
   `on_error`, `second_look`, `judge_draft`, `release_held`, then `run_tools` or `finish_answer`)
   absorbs queued inputs (transcribing audio), then calls the model up to `kMaxRounds` times (a
   `Budget`): stream a reply, run any tool calls, repeat.
3. When the request had spoken input, text streams sentence by sentence into `SpeechQueue`, which
   synthesizes and plays it; a typed request's answer is only shown (Typed or spoken, below).

Things in that loop that exist for a reason; do not remove them without re-running the tests:

- **Fast mode** turns the model's reasoning off for speed. With reasoning off the model
  sometimes claims an action without calling the tool. So a reply that used no tool is held
  back and checked once ("second look") before it is spoken, and two regex guards
  (`claims_action`, `claims_not_saved`) catch "done" and "that isn't saved" said without a call.
  The guards, `claims_done` (below) and the notes the loop sends with them are one table,
  `guards::kClaims` in `src/agent/guards.cpp`.
- **Small-model controls** (`src/agent/controls.h`): `second_look`, `decide_note`, `claim_guards`,
  `say`, `weekday_check`, `cancel_claims` and `date_spelling` are named so that what each is worth
  can be measured by switching it off in the cli build (`ZB_EXP_OFF`, tests/README.md); the device and
  simulator builds always have them all on, and nothing the model sends can switch one.
- **`say`**: tools whose effect is fully known up front (timer, calendar, switch) take the
  confirmation sentence as a parameter, so success is spoken without a second model call.
- **Steering**: input that arrives while a request is being worked on is added to the same
  request. While the microphone is open (`Agent::set_listening`), or what the user said is still
  on its way (the app connects to the phone first: `App::when_online`, `Agent::set_input_coming`),
  nothing new is spoken and no action runs: every call of a batch waits. Whatever the user has said
  more by the time a call would be made, while the microphone was open or before, stops it: a reply
  nothing of which has been shown or said is withdrawn, as one without a call is, and otherwise the
  calls not yet made, the first included, are not made (each answered "this call was not made", not
  counted as failed), so the model decides again with their words. Input that waits for the phone
  is handed over in the order it came, and is dropped with the request on ESC or a new conversation:
  the agent's cancel first, while the gate is still shut, then the input dropped and the gate opened
  (`App::drop_waiting_input`); opened first, a call waiting at it could be made before the cancel.
- **Typed or spoken** (ZB-091): a request's answer is spoken only when something in it was said
  aloud (and spoken replies are on); a typed one's is only shown. Its state says which, by whether
  it is heard (`Turn::heard`): `Speaking` (TAB stops it: `Agent::interrupt_speech`, and the
  conversation keeps "[the user interrupted here]") or `Answering` (only shown, a spoken request's
  too with spoken replies off: TAB does not stop it). Music waits only for an answer that is heard
  (`tools::Context::in_conversation`; a recording holds music as the microphone closes only when
  replies are spoken, `Agent::replies_spoken`, and music held for an answer that turns out to be
  only shown comes back as soon as that is known: `Turn::set_spoken`), and each answer's end is
  signalled at its own request's end.
- **Numeric dates** are spelt out by rule (`util::spell_numeric_dates`) before the model sees
  them, in the user's date order with the year filled in (the same rule as a date typed in the
  calendar editor: `year_meant` in `util.cpp`).
- **Timestamps** ride on each user message, not in the system prompt, so the prompt stays
  byte-identical between calls and the service can reuse its cached prefix.
- **Music holds** (`audio::Music::hold`): music waits for the spoken reply to finish, and stays
  quiet from a spoken question to the end of its answer when that is spoken. A typed request, and
  one said aloud with spoken replies off, holds nothing (`tools::Context::in_conversation` is false
  for it).
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
- **Failed tool calls** (tested by `tests/sim/selfheal_chat_e2e.py`):
  - *What the model is told.* `tools::run` checks each argument against the tool's own schema
    before the tool runs: a wrong type names the argument, what it must be and what was sent;
    "yes", "true" or 1 for a boolean and an enum value in other capitals are taken as meant; empty
    text for a number counts as not given, and other text must be a number as a whole
    (`util::to_number`, which `num_arg` uses too: "20 minutes" is not 20, "12oops" not reminder 12).
    A call cut short or broken says where, with an example; an unknown tool name gets the nearest
    one. Every failure has one shape, `ERROR: what was wrong. What to do` (a tool's JSON
    `{"error": ...}`, whatever "error" holds, is turned into it before anything is added to the
    result or it is cut short); `result::failed()` (`src/core/result.h`; `tools::failed` forwards
    to it) recognises it, and a JSON object holding "error" as JSON reads the key (`"err\u006fr"`
    too), parsed once. Tools whose schema is made at run time (`smart_home`, `radio`: `Tool::spec`)
    check their own arguments, as do tools reading an argument their schema lacks (the timer's
    `id`): a number that is not one is refused before any target is chosen, and in `smart_home` a
    percentage may carry its sign ("40%").
  - *Retries.* Only what is safe to repeat is sent again by itself: the AI service, transcription,
    speech, and the tools' reads and searches (`fetch_json`), through `http::send_retrying`, after
    a failure on the way (`http::transient`: no connection, a dropped one, 408, 429, 5xx but 501
    and 505; never a refusal, even one whose body was then cut short, and never a timeout once the
    request had gone out), after a pause
    and inside the request's own time limit. Nothing that changes, logs in or pairs is retried by
    this; a device change is resent only by the smart-home layer, at a device's new address, and
    only when it comes out the same if made twice (on, 40 %, an input; never a volume step, key
    press or playback control: some TVs toggle play and pause with one command). In the loop, a
    call that failed is not made again unchanged in the same request (`call_key`: tool and
    arguments, "say" aside): the model is told it already failed. What the user says next clears
    that, so "try again" works. A call refused only for what its `say` claimed, before anything was
    done (`weekday_slip`, `claims_new`: `Context::say_refused`), is not remembered: it may come again
    with the say put right, or after the step it lacked. Web search fails over by itself, never back
    through the model (`search.cpp`): the next service of the user's list is asked at once, inside 8 s
    for the whole search (the first of several gets 5 and is sent once), and the model reads one
    `ERROR` only when none answered; a key a service refused is marked beside it in the settings and
    not sent again until it is typed again, by this run or a later one (as `home::trial`; only a 401 or
    the service's own word for it counts, never a bare 403, which a firewall or proxy answers too), a spent
    allowance is passed over until it comes back (kept in `search_usage.json`, so by a later run too), two
    failures in a row rest a service for two minutes, and the order never changes by itself. Each service is
    looked at again just before it is sent (`Search::still_set_up`): a key refused, a service switched off,
    another key, an entry removed or an allowance found spent while another was asked is not sent, and a
    refusal marks only the key as it was sent (`SearchService::typed`: one typed again meanwhile is a key to
    try). Every request to a service goes through `search::send`: no redirect is followed (the key travels in
    a header) and an answer is read up to 512 kB; an answer's status stands whatever happened to its body (a
    401 cut short is a refusal, not resent), and a service's own words never reach the model or the settings
    page (`Reply` has no error text; every key is taken out of what it answered). An answer with results is
    kept in memory for ten minutes, so the same query again asks no service (any change to the services in
    the settings, `Settings::search_revision`, starts it afresh); past 100 searches a day
    (`usage::kDailyCap`) the model reads one `ERROR` until the next day.
  - *Never silence.* "Done" in a reply after tool calls that all failed or wait for a yes is
    withdrawn once, like the guard for no tool at all (`claims_done`: an offer put as a question,
    "Shall I put it in for next year?", is not withdrawn; it is often the right answer then; the
    radio's and music's own confirmations, "Here is X." or "X is playing", are claims too, but not in
    a sentence that says no or asks: "Nothing is playing right now" is the honest answer to "skip"). An
    empty reply goes back to the model once, then an answer is made in code. When the service is
    lost after tools ran, the user hears the confirmations of the calls that worked and that the
    rest could not be finished. When it fails before any tool ran and before a word of the answer
    was heard, or a recording cannot be transcribed or holds no words, a spoken request hears one
    short sentence ("Sorry, that did not work. The details are on the screen.", or "I didn't catch
    that."), and the reason is shown as before; a typed one stays silent (ZB-094). One place decides
    it (`Agent::Turn::say_failed`): spoken replies on, the microphone closed, not cancelled. The
    sentence is not kept in the conversation (the service failed, not the model, which is sent
    nothing new), and it is only queued, never waited for: speech uses the same service, and when
    it fails too the speech queue's own note follows the reason, which stays.

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
PC: use only non-deprecated interfaces), FreeType and libpng for the UI. MP3 encoding (libmp3lame),
MP3 decoding for music (libmpg123) and Opus decoding (libopusfile) are loaded at run time with
`dlopen` and are optional. The device build (`cp0`) is `-O2` with every function in a section of its
own and `--gc-sections`, LVGL included (a quarter smaller than `-O3` without them); `scripts/deploy.sh`
and `scripts/package-deb.sh` install it stripped, and `build/cp0/zero_bot` keeps the symbols for
reading a crash.

## 6. Testing

The tests are in `tests/` by kind: `unit/` (offline unit tests), `sim/` (offline end to end against simulated
devices and services, and the screenshots), `agent/` (the live sets: a real AI service with simulated tools, and
their gate), `tools/` (real services, no AI), `mocks/` (simulated devices and services, shared) and `lib/` (shared
code). **`tests/README.md` says what each test is and checks, how the live sets score, and what the app's test
modes and switches do** (`--ask`, `--tool`, `--shots`, `ZB_DRY_RUN`, `ZB_TEST_NETWORK`, `ZB_EXP_*`, ...). This
section says how to run them.

**Start with `tests/run.py`.** It runs the tests in groups and prints one line per group (its
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
  runs every offline group, every case of both live sets once, then the gate of `tests/agent/exp_compare.py`
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
  `tests/run.py --groups web-contract` (or `tests/tools/web_contract.py`): the stand-in web's shapes against the
  real services (tests/README.md, tools/). It needs the internet, so it is in no profile; it never calls the search.

**A release is measured against the roster**, every case defined in this tree (the scored
set's `CASES` and `cases.json`, by name and current `case_hash`, with the critical cases read from the
definitions), never against the cases the results happen to hold: the variant needs a scorable run of
every case as defined now by the current scorer, 90% of each set must be compared, and every critical
case compared and passing; a critical case cannot be left out. The baseline's provenance must match:
the same `case_hash`, `scorer`, `trace` and, where recorded, `fixtures`. A change to a check, a home function or the
stand-in web changes the hashes of the cases that use them (tests/README.md, agent/): run those cases again into the
baseline. **Live runs and the gate:** with `--baseline`, a live run whose cases
failed passes only under a gate verdict of *passes* that compared its set (`gate.json` in the run
folder); the verdicts are in tests/README.md (agent/, the gate). **Trace policy:** the runner
always passes `--trace`, which adds `stream_options.include_usage` to what is sent, so a baseline for
the gate must be traced too; traced against untraced pairs are left out.

`ZB_EVAL_WRAPPER` prefixes every live run (a lock, or a script that sets the service's variables);
live runs always use `--jobs 1`. **No test uses a live web service** (the owner's search allowance is
spent by test runs otherwise): every live case's web reads, search included, go to the stand-in web
(`tests/mocks/fake_web.py`), with a dummy search key; the runners remove any inherited `ZB_SEARCH_KEY` and refuse to
start when a case or the binary could still reach a real web service (`agent_eval.py` `preflight`, `web_guard`,
`probe_binary`'s `web_all`). Behind that rule is **the network boundary** (`tests/netguard.py`; tests/README.md has
it whole): every test process that runs the app gets `child_env` (no key it does not need, every proxy variable at a
local trap that records and refuses), mocks get `mock_env`, every entry point that runs the cli refuses a build without
the experiments and the network pin, and inside an experiments build (`ZB_EXP_NET`) the app's HTTP goes only to a
numeric loopback address and, in a live run, to exactly the AI service's origin, with no redirect followed.
`--no-search` is accepted and changes nothing. Groups never inherit switches that change what they test (`ZB_UPDATE_SHOTS`,
`ZB_UPDATE_SNAPSHOTS`, `ZB_UNIT_*`, `HUE_E2E_ONLY`, `ZB_FAST_MODE`, `ZB_EXP_*`). A new test script goes in the
`tests/` folder for its kind, gets a line in the group table at the top of `tests/run.py`, a rule in its path map for
the files it covers, and a line in `tests/README.md`.

- **A fresh state per test**: point `XDG_CONFIG_HOME`, `XDG_DATA_HOME` and `XDG_CACHE_HOME` at
  a temporary folder. Do the same on the device so the user's calendar and data are untouched.
- **Code ported from other projects** keeps their notices in the source file and in
  `licenses/THIRD-PARTY-NOTICES.md`, which the package installs; add to it with any new port.

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
  `tests/agent/suite/from_conversation.py FILE` prints a draft multi-turn case from one; generalise its
  words (other names, other events) before it goes into `tests/agent/suite/generate.py`.
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
- libcurl sends a header set with the request (Brave's `X-Subscription-Token`) on to whatever host a redirect
  names; only `Authorization` and cookies stay with the first host. The first search providers followed
  redirects with their key in such a header, and the test of redirects ran with the stand-in web, which follows
  none anyway, so it could not see it. Every request to a search service goes through `search::send`, which
  follows none, and `tests/unit/search_test.cpp` sends to two local origins with the options a provider uses.
- The Wi-Fi chip's firmware ignored the country code, hiding channels 12 and 13;
  `docs/wifi-channels-12-13.md` has the device-tree fix.
- libcurl does not let threads use one connection cache at once, even through a share with lock
  functions (`CURLSHOPT_SHARE(3)`), and the app's first share held connections too: the agent's worker,
  speech, the settings page's lookups and every `background()` thread sent through it together (found by
  review, not by a crash). The share (`http.cpp`) now holds DNS answers and TLS sessions only, and each
  thread keeps one easy handle between its requests (`ThreadHandle`, reset for each), and with it its own
  connections: the agent's next call to the AI service still goes out on the connection the last one left
  open. A request with a device's own TLS settings, or one made while the thread's handle is busy, gets a
  handle and a connection of its own. `tests/unit/http_test.cpp` counts the connections at a local peer:
  reused within a thread, never one used by two.
- A status received stands whatever then happens to its body: `http::transient` took a 401 whose body
  was cut short for a dropped connection and sent the request again, refusal and all. Only a success cut
  short (a read that lost its body on the way) or no answer at all is resent.

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
- A descriptor another thread may close must not outlive the lock it was read under.
  `Speaker::write` took the player's pipe under its lock and wrote outside it, while
  `SpeechQueue::stop()` on the UI thread closed it: the number was then free, and a file or socket
  opened meanwhile got the rest of the sentence. A write now goes through a copy of its own
  (`F_DUPFD_CLOEXEC`), which stays on the pipe (it fails once the player has gone) and leaves `stop()`
  and `kill()` as fast as before; `tests/unit/speaker_test.cpp` holds the writer between the two steps
  through `-Wl,--wrap=write`.
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
- A model the service lists without a label is not necessarily a chat model: an embedding model
  listed first became the language model in the setup guide, which took "chat or unlabelled". By
  the owner's rule only a model labelled `chat` is chosen without the user (ZB-088); the settings
  list, where the user chooses, falls back to the unlabelled ones so that it is never empty
  (`ui/models.h`, `tests/unit/models_test.cpp`).
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
  12. A number that is not one must be refused, not passed over as if not given: the timer's
  `"id": "1oops"` then fell through to "the only timer running" and cancelled it. Nested
  brackets overflowed the calculator's stack: recursive parsers need a depth limit.
  nlohmann's parser does not recurse, but copying, comparing and printing the tree do, a level a
  call: JSON from another machine (a device, a web service, the AI service, a tool call's arguments)
  is read with `util::parse_untrusted_json`, which refuses unread anything nested deeper than
  `util::kMaxJsonDepth` (32), and the caller treats that as an answer it cannot read (ZB-093).
- `mktime` makes 31 February into 3 March without a word, and the zone database holds folders
  named like countries ("Australia") that the C library reads as UTC: `parse_local` and
  `timezone_exists` refuse both. Check that a date, time or zone exists before using it.
- nlohmann::json: a function taking `const json &` called with `{x}`, where `x` is already a
  json value, gets a copy of `x`, not a one-element array (nested lists do become arrays). Build
  lists as `std::vector<json>` or `json::array({...})`. It also sorts object keys when dumping.
- A request that names a need rather than a thing gets a question unless a tool says what it asks
  for. With a station tagged focus, "help me study" got "What are you studying?" or an offer of focus
  music (2 runs in 10 played it) while the radio tool said only "help to focus, study or concentrate";
  naming the phrase and saying to play rather than offer (`radio::focus_note`) made it 9 in 10, with
  its neighbours (a study timer, "help me relax", a focus timer) unchanged.
- Nothing that differs between runs belongs in the prompt or a tool's description. The radio tool's
  sentence for a missing focus tag first named the station file's path, which is each test run's own
  folder, so every such case missed the service's cached prefix (about 8 s to the first byte instead
  of under 1 s). A path the model needs goes in a tool's result.

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
- No saved id may be a family's prefix alone. Tapo answers without a `device_id` became "tapo:",
  shared by every such plug, so a rescan moved one plug's name, settings and MAC to another. An id
  now falls back to the MAC (from the answer, else the system's neighbour table) or the device is
  not listed, and `remember_scan` and the search for a moved device never merge a record with a
  device of the same id but another known MAC.
- A typed password, code or key is on trial until a device accepts it (`home::trial` in driver.h):
  the trial is written to the file before the first send, nothing is sent when that write fails,
  and a trial an earlier run left behind counts as refused until the user types it again (the app
  may have stopped before the device answered, and a Tapo plug locks after a few refusals).
  `pairing_submit` writes a "trying" mark on the device record before it hands anything typed to a
  driver, and takes it back in the write of the outcome; while it stands, the record's pairing data
  are handed to no driver, but kept as they are (`Saved::held_auth`, written back by every write). They
  were first simply not read, so the next write of the record erased them: taking the mark back when
  the network changed before the handover, or any other change saved meanwhile, lost a Shelly's user
  name or a Fire TV's pairing that nothing had been sent with. A mark taken back puts back the one
  before it (an earlier run's still stands). `pairing_submit` holds the device's lock from its first
  read of the record to that write: a second HomeKit code typed while the first was with the accessory
  read the marked record (no pairing data), failed once the first had paired, and wrote its failure
  over the keys the first had saved. It also takes the step the page showed, and refuses, sending
  nothing, when the driver no longer asks for it. Shelly ended a password's trial when a pairing was refused, before
  that write: when the write failed, the record kept the old password with neither its refusal nor
  a trial, and it was sent again. Every failed write of `smarthome.json` is reported
  (`home::kNotSaved`), not lost.
- A refusal is kept in memory for the rest of the run as well as in the file (`home::trial::refuse`,
  per credential): the file can fail to take it, and Tapo's transport remembers a refusal per plug
  only, so the account's other plugs, or another WiZ device, were sent the refused password or key
  again in the same run. The two protect differently: the memory is this run's and cannot fail; the
  file is what stops a later run, and a write can fail. A refusal the file could not take is said (in
  the error the user and the model read): a credential still on trial counts as refused in a later run
  anyway (its trial stays), but one a device had accepted before is then held by the memory alone, and
  could be tried once more after a restart. A credential on trial goes to one device at a time (each
  family's `g_first_try`, taken before the account or key is read, let go once the outcome is kept, not
  after the slow reads that follow, such as a plug's energy readings or a strip's sockets, which
  `tapo::get_info` read inside the first answer's call): a command that fans out sent a
  wrong password to four plugs at once. A refusal on such a later read is said as any other, with
  whether the file took it; the energy readings first dropped it, being extra. It is also sent once: WiZ resends a datagram after 250 ms, which
  sent a key on trial again while the device's first answer was on the way; on trial it goes out in
  one. A refusal is what the call that sent the credential said (`tapo::request`'s `refused`), not
  looked up afterwards: saving the Tapo account, even unchanged, drops the transport's sessions and
  what each plug said, and a refusal that arrived meanwhile was lost, so the next plug was sent the
  same password. And it is the refusal of the entry that was sent: each password typed is a new
  generation (`generation` in the account, kept over a change of e-mail), so the late refusal of a
  login with the same password typed before no longer refuses the new entry before it is tried. Only
  the credential's own rejection is a refusal: a TPAP plug locked by another app says so at
  `pake_register`, before any proof of the password, and that lock is the plug's own (kept by the
  transport for that plug), not the account's; it first stopped every plug of the account.
- A scan is of the network the user agreed to: `home::scan` takes the network the settings page
  showed (`discovery::Options::network_id`), refuses once the device is on another, and drops what
  it heard when the network changed while it listened. So does the short scan for a device that
  stopped answering (`relocate`), through the same `scan_on`: it first scanned whatever network the
  device was on by then, and saved the addresses it heard under the network the command was for.
- An address is not an identity. Routers hand a device's address to another one (a router that
  loses its leases when it restarts, a bulb switched off at the wall for a week), and a device
  with no key of its own (WiZ, and any family whose protocol has no login) answers whoever asks.
  The first review of the WiZ driver found that a command for a light switched the plug that now
  had its address. Such a driver checks the device's MAC (the one discovery saved it under)
  before every change, and fails with words `home::who_answered` reads as another device answering
  ("answered at this ... address"), so the app looks for it.
  Only a positive match counts: an answer with no MAC, or an empty one, is not the saved device
  (Kasa's get_sysinfo; WiZ's getPilot, where firmware that leaves the MAC out is asked
  getSystemConfig within the same time), and a record saved without a MAC is never changed. The
  first versions took "no MAC" for "nothing to compare" and switched whatever answered. Twelve zeros
  are no MAC either: one check (`support::mac_digits`) serves what is saved, compared and
  controlled, after a scan that reported zeros replaced a real saved MAC and a later scan with
  another real MAC took over the record and its settings. A record whose `mac` is empty but whose
  id was made from the MAC (Kasa's "tplink:<MAC>", WiZ's "wiz:<MAC>") is checked against that MAC
  (`support::saved_mac`); Kasa first refused such records from earlier versions outright.
  A login is no identity either: every plug of one Tapo account accepts the same password, and
  until the integration review a command for a desk lamp switched on the heater plug that had
  been given the lamp's address, with no question. The Tapo transport now asks each new session's
  plug for its MAC (`tapo::Device::mac`) before sending anything else in it, and takes only a plug that
  names it; a plug saved without a MAC (an earlier version saved twelve zeros, which are none) is never
  switched: it was first taken as "nothing to compare", so the plug given its address was.
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
- A device that answered is not lost. The shared layer took any error holding "answer" (or
  "connect", "reach", "timed out"...) for silence, so a reply that could not be read or nested too
  deep, a Tapo error code, a Shelly answer too long to read, even the layer's own "the device's
  answer could not be understood", sent it searching the network for 2.5 s, after which the model was
  told the device was "not found at another address ... switched off at the wall" (ZB-108). Only
  exact phrases count now (`home::who_answered`, driver.h): nobody answering, or another device
  answering at the address, is looked for; anything else is the device's own answer. But only once
  it has said who it is: Tapo logs in before the plug names its MAC, so an answer no plug would give
  before that (a web server's 404 to a login step, a reply to "who are you" that cannot be read)
  says it "may be another device that now answers at this plug's address", and is looked for; the
  same answer after it is the plug's. The drivers' end-to-end scripts read those phrases from the
  function itself (`tests/lib/answered.py`), and on the stand-in network, where the app never scans,
  `ZB_EXP_SCAN` notes each search it would make.
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
- A 401 to a login is the device's answer whatever then happens to its body. One whose connection
  dropped part way through the body made libcurl report a transfer error, which the Shelly driver
  took for no answer: it kept the password and sent it again. `http::Response` keeps the status of
  an answer whose body failed, and says whether a digest login was answered (`digest_answered`):
  the 401 that only challenges, before libcurl sends the login, refused nothing, even cut short.
  (Told that the connection closes, libcurl leaves a challenge's body unread and sends the login on a
  new connection; otherwise it reads on, and a challenge cut short ends the request there.)
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
- A test that walks menus by counted key presses breaks without a word when a row is added: every
  later screenshot shifted. `--shots` walks by row label (`Shots::menu_to`), pressing the keys a
  user would, and the pictures came out identical, pixel for pixel.

**Power**
- The main loop slept 16 ms at a time whatever happened: 62 wake-ups a second for as long as the app
  ran (measured on the device, 2026-11-17). It now sleeps in `uiq::wait` until LVGL's next timer or
  the app's next work (`App::sleep_ms`), woken at once by a key or a post: 1.3 wake-ups a second on a
  quiet chat screen and 2.4 with a timer counting down, against 61.9 before, measured on the device
  build under qemu. The sim cannot show it (its SDL driver polls every 5 ms). The device build runs on
  a PC under `qemu-aarch64 -L <sysroot>`, with an `LD_PRELOAD` shim that answers the fbdev ioctls for
  a regular file (`APPLAUNCH_LINUX_FBDEV_DEVICE`) and its keyboard at a FIFO of `input_event`
  records (`APPLAUNCH_LINUX_KEYBOARD_DEVICE`): its frames can be read back and keys pressed, which
  checked the alarm, its banner, keys, reminders and dimming against the new loop (ZB-036; the
  scripts are in the maintainer's notes).
- A file written with a lock held makes every thread waiting for the lock wait for the card too: a
  tool's save of the calendar or memory held `Store::mtx_` through its fsync, and the UI thread takes
  that lock every second (299 ms of a 300 ms write in `core_test`). Queue under the lock, write after.
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
- A case's hash covers the source of its checks and home functions and every byte of the stand-in web, comments
  included (tests/README.md, agent/). Moving the tests into folders changed one case's hash, because its home
  function built a path from `__file__` (it reads `MOCKS` now), and fixing the old paths named in the comments of
  `tests/mocks/fake_web.py` would have changed all 743. Keep paths out of hashed functions, and leave the stand-in
  web's comments to a change that moves its fixtures anyway.
- A multi-agent workflow resumed after an interruption reuses finished steps only when it makes
  the same calls in the same order. To continue with a changed plan, save the finished results to
  a file and start a new run from there.
- Make release builds in the main checkout. A worktree reaches LVGL through a link to the main
  checkout's `.cache`, which lies outside the worktree's `-ffile-prefix-map`, so a device binary built
  in a worktree contains a path of the build machine (with the user's name). The `strings` scan
  before a release catches it.
- A rename made with a regular expression also renames inside string literals and comments:
  `correction` to `correction_` turned the trace's purpose "correction" into "correction_". The
  request dump did not see it (purposes are never sent); the loop test of the trace did. Restrict
  such renames to code, and compare the set of string literals before and after.
- Copies that look alike are not always the same. The three UTF-8 repairers in the drivers accepted
  exactly the same sequences but put '?' or U+FFFD in a bad byte's place; the three name cleaners
  differ in which characters they drop and in capping at 40 bytes or 40 characters; HomeKit's depth
  check never lets a stray closing bracket count below zero; Kasa's deadline wait polls once more at
  the deadline, HomeKit's does not. Before merging copies, run the old and the new side by side over
  every short input (ZB-038 compared about a million strings) and keep a difference as a parameter or
  a separate function.
- A sandbox does not close what it inherits: `bwrap` handed the app's open descriptors (the
  framebuffer among them, which LVGL opens without close-on-exec) to the sandboxed Python.
  `proc::spawn` closes everything above stderr in the child; keep it. Where close_range is missing
  or refused it closes what /proc/self/fd lists, and where that cannot be read either, or a
  descriptor it lists will not close (a sandbox may refuse close() too), it does not run the program
  (the child exits with 127): its fallback first stopped at 4096, then at the soft
  limit, then at the hard limit, and a process can lower either below a descriptor it already
  holds, so no count up to a bound is sure to reach them all. Anything slow that a tool
  starts takes `ctx.cancel` (`proc::run`'s last argument), so ESC stops it.

## 9. Adding things

**Keep it as tidy as the refactor left it.** The code was restructured in November 2026 (ZB-035) so that
each job has one place. New work keeps it that way; a change that works but undoes the structure is not
finished:

- Put code where section 3 says its job lives, and extend the file that owns the job. A new folder
  or a new kind of file needs a reason that does not fit anywhere else.
- Use the shared helpers rather than writing another copy: `result::fail`/`failed`,
  `util::to_number`, `sys::now_ms`, `power::`, `net_status::`, `drivers/support`, `background()`,
  `Settings::get`/`update`, `uiq::post`. Two copies of the same logic found while working become one.
- Small functions with one job. Grow a long flow by adding a named step (as `Agent::Turn` does),
  not by lengthening one; a function past about 60 lines needs a reason.
- No dead or commented-out code, debug prints, or TODOs without a ticket. Comments say why, in plain
  words; a forwarder left for callers goes once nothing calls it.
- Tests beside the existing ones, and the documents (this file, the README) changed in the same change.
- Before handing a change over, read its whole diff once only for tidiness.

**A tool.** A new file `src/agent/tools/<name>.cpp` (include `agent/tools/common.h`) with
`t_<name>(const json &, Context &)` returning text or JSON for the model (use `fail("...")`, which
is `result::fail`, for errors, phrased as an instruction about what to do next) and
`Tool make_<name>()`, its spec: name, label, a description written for the model and a compact
schema (`obj`, `S`, `N`, `B`, `E` from `tools/common.h`; the runner checks argument types against
it before the tool runs, so the tool keeps only its domain rules; a reply that reads from the
network uses `fetch_json` from `tools/web.h`, with its address through `experiment::web()`, which
retries what is safe; `Fetch` sets headers, a body, the deadline and attempts, and `Fetched` says what came
back). If success is fully known from the arguments, give it `takes_say(note,
when)`: the registry adds the "say" parameter, and `when` picks the calls whose say is spoken
without the model reading the result. `offered` and `spec` are for a tool offered only sometimes
(web search needs a service set up: `search::configured`) or whose schema is made from the user's data
(smart_home, radio). Then one
line, `X(<name>)`, in `ZB_TOOLS` in `src/agent/tools/registry.cpp`, where the order is the order the
model reads them in; nothing else changes (CMake builds every file in the folder). A tool that
creates or moves records (as reminder_set, calendar_add and timer's start do) goes in `claims_new`'s
list in `tools/say_checks.cpp`, so that a cancel after it in the same request may say "instead". If
it needs the screen or hardware, add a callback to `tools::Context` and an event in `AgentEvents`,
set in `Agent::Turn::run` and handled in `App::wire_agent` through `uiq::post`. Honour `dry_run()`.
Test it with good, wrong and hostile arguments in `tests/sim/tools_cli.py` (offline tools) and add cases
to `tests/agent/agent_eval.py`, including a neighbour that must not trigger it; `--schema` shows what the
model will read. Document it in `README.md`.

**A setting.** Add the field to `Settings` (`src/core/settings.{h,cpp}`: member, `to_json`,
`from_json`), a row in its section's function in `src/ui/settings_pages.cpp` (`assistant_rows`,
`device_rows`, ...; `open_settings` puts each section's rows under its header, and each header is a
page of its own, opened from the top page: a new header is a new section), the setup guide
(`setup_guide.cpp`) if a new user needs it, and a line in the README's settings table. Rows read the
value with `S()` and change it with `change(...)` (`Settings::update`; both in
`settings_internal.h`), never by writing to a copy. A key, password or code is typed with
`Menu::edit_text`'s `secret`: the field starts empty, shows bullets (each character for a moment as it
is typed), is emptied whenever it closes, and `--keydump` logs none of it (`input::set_secret`); the
first versions showed a new key in full while it was typed. `--shots` walks to rows by label, so a new row
moves no later screenshot; if it walks to a row you rename, rename it in `shots.cpp` too.

**A key.** Chat-screen keys outside the input line are decided in `keyguide::chat_key`
(`src/ui/key_guide.cpp`); add the key's line to the guide there too (`H` shows it), and keep the
bottom row's hint short: it names only the keys wanted most.

**A screen.** Follow `breath_view` or `chat_view`: create objects once, update only what
changed, drive it from `App::periodic` (`app_status.cpp`), route keys in `App::handle_key`
(`app_keys.cpp`), and add a `--shots` frame in the function of `shots.cpp` for its group of screens.
The main loop ticks only while something needs it (`App::lively`, and a view's `needs_tick`) and for
a second after anything happens; otherwise it sleeps to the next second's work. Whatever changes on
screen by itself, without an LVGL animation or timer, a key or a `uiq::post` (a countdown, a polled
state), must be added to `App::lively` or to `App::sleep_ms`'s deadlines, or it is drawn up to a
second late; `--shots` checks the sleep (`loop_sleeps_ok`).
A settings page that needs something slow (a subprocess, the network, the first full look at the
network) reads it with `background()` and refreshes itself when the answer comes, showing a
placeholder meanwhile. A late answer acts only while the page that asked is still the one on top,
not left and gone back to, with no text field open over it (`background_on_page`, or `Menu::on_top`
and `showing`, a number each showing of a page gets: pushed, or gone back to with ESC): a check of
`menu.open()` let a list of models fetched on the setup guide open over its next step, and a number
for each page pushed let a "Test and continue" answered after Skip and ESC move the user on again.
So does a row's value, which is read at every redraw of its page: the Smart home row on Your data
shows the last count while it counts again (`home_things`); counting at each key press ran `iw` on
the UI thread. A device's page asks what its driver still needs (`home::pairing_step`, which looks
at the network) the same way, and "Set up…" is handed the answer. An answer that only redraws the
page that asked (these two, the Smart home page's look at the network) redraws it on a page gone
back to as well (`Menu::page_on_top` and `refresh_page`). A list of models is asked of the service's
address and key as read once (`llm::Service`, from which every request to the service is addressed),
and used only while they are still the settings' (`models::fetch`, `App::models_answered`): the
lookup read the settings again to build its request, so an address saved meanwhile got the old key.
`--shots` opens these pages with a network that answers only after 1.5 s (`opens_at_once`), and
holds the setup guide's late answers in the UI queue until the user has moved on, then delivers
them (`late_setup_answers_ignored`, `returned_setup_answers_ignored`); it notes where each lookup's
key went at a stand-in service on 127.0.0.1 (`lookups_keep_their_service`).

**Something shown over every screen** (the timer's alarm banner, `AlarmView`): make it a child
of the screen, created after the other views, and `raise()` it again whenever another view
brings itself forward (`BreathView::start` does). Not `lv_layer_top()`: `--shots` snapshots the
screen only, so it would be missing from the screenshots.

**The status bar** is redrawn by anything that sets one of its labels or colours, even to the
value it already has. `ChatView`'s setters compare with what is shown and return early; keep
that for anything added there (the timer countdown changes once a second, not every tick). A
label that scrolls redraws the bar on every screen refresh (about 40 times a second, measured in
the simulator when LVGL refreshed every 16 ms; at most 30 since `LV_DEF_REFR_PERIOD` is 33), so
nothing there should scroll for long: the now-playing text scrolls only when
it has all of its room, and beside the countdown it is cut short with dots instead. A radio
station's name, there for hours, never scrolls (`set_now_playing(text, true)`).

**A settings value that changes by itself** (a timer's time left, a web search service's state and
use, which a search made meanwhile changes): set `MenuPage::live`.
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

**A web search service.** It is a provider: a class implementing `search::Provider`
(`src/agent/search/provider.h`) in `src/agent/search/providers/<kind>.cpp`, which defines
`std::unique_ptr<search::Provider> make_<kind>_provider();`, and one line, `X(<kind>, "<Name>")`, in the
list in `providers/registry.cpp` (the kind is what the settings' list calls it, `SearchService::kind`).
Nothing else changes (CMake builds every file in the folder): the web_search tool, its description and
schema, whether it is offered and what the model reads (`search.cpp`: the summary and four results, each
capped) are the same whichever service answers, so a new service changes nothing the model sees but the
words. A provider builds its request, sends it with `search::send(address, how, ctx, got, err)` (`fetch_json` to
`experiment::web(address)`, never following a redirect and reading at most 512 kB; the deadline and attempts are the
layer's, in `how`; the provider adds its headers and body, and `Fetch::settled` for an answer a resend cannot change,
as Brave's 429 for a spent month), maps the answer onto `Reply` (summary, hits, read only up to `kMaxHits` that say
something; the wrong shape is `Garbled`) and says what it meant as an `Outcome` (`outcome_of` for what any HTTP
service means, plus the service's own codes: Tavily's 432 and 433 for a spent allowance, Brave's error code with the
status it belongs to, from the body, `Fetched::body`, and its allowance in reply headers kept through
`Fetch::keep_headers`, taken only when they are whole numbers in the windows they should be), with what the service
says of its allowance (`left` and `limit` in its own units, `reset_s`): the layer acts on them (a refused key is never
sent again, a spent allowance is passed over until it comes back, the figure is kept for the settings page), so a
refusal must never be reported as a mere failure. A service that can say what is left without spending any of it
(Tavily's `GET /usage`) says so in `tells_allowance` and `allowance`, which only the settings page's button calls
(`search::ask_allowance`, once a minute at most), never a search. It never retries, waits, picks another service,
counts allowance, caches, writes a file or shapes what the model reads: those are done once, in `search.cpp` and
`usage.cpp`. Its settings page needs nothing of its own: `settings_search.cpp` lists every registered service. The settings hold only generic fields per service (kind, key,
on or off, and the refusal's mark); an entry of a kind this version does not know is kept and passed over, and every key is
blanked from saved conversations through `Settings::secret_values()`. Test it against the stand-in web, never a
real search: one entry in `SEARCH_SHAPES` in `tests/mocks/fake_web.py` (its address, how its key travels, how an answer
is wrapped, how it refuses a key and says the allowance is spent, one hostile reply), and `tests/sim/tools_cli.py`'s
search checks, which read the registry, run it with nothing more to write; `tests/unit/search_test.cpp` tests the layer
with stand-in providers (`search::register_provider`). The stand-in's bytes are hashed into every live case, so a new
shape needs a new baseline of both live sets (section 6). Add the service to the README.

**A smart-home device family.** It is a driver: a class implementing `home::Driver`
(`src/smarthome/driver.h`) in `src/smarthome/drivers/<family>.{h,cpp}`, which defines
`std::unique_ptr<home::Driver> make_<family>_driver();`. Registering it is one line,
`X(<family>, "<Name>") \`, in the list in `src/smarthome/drivers/registry.cpp`; the name is what the
settings pages call its devices. Discovery must report the family's devices under the kind its
`claims()` accepts (`kServices` or a vendor query in `discovery.cpp`). A vendor's broadcast query
takes a socket in `Sockets` (`open_sockets`), its question and times in `Questions`, a parser called
from `receive_answers` into `Seen`, and a function of its own in `Merge`, called from
`Merge::devices()`, whose order is part of the ranking (a later pass can overrule an earlier one, as
the Kasa answers do). Nothing else changes (CMake
and `tests/unit/run_unit.sh` both build every `.cpp` in `src/smarthome/drivers/`): the
shared layer does names, rooms, the tool's schema (it grows with the capabilities a home has),
the safety questions, rate limits, locking, fan-out and the generic pairing pages (after a
pairing step, the family's other devices are asked again, so an account typed once serves them
all; what a step asks for is hidden while it is typed unless the driver marks it `PairStep::plain`,
an e-mail or a user name). The rules a
driver keeps: map only allow-listed types (lights, plugs, switches, thermostats, covers, fans,
speakers, TVs, scenes, read-only sensors; never locks, garage doors, alarms, cameras, valves or
cooking appliances); state in the units and keys documented in `home.h`; secrets only in
`Device::auth` or `secrets()`, never in errors; never retry a rejected code or password, even in
a later run, and keep a typed one on trial (`home::trial`) until a device accepts it, a refusal in
this run's memory too (`trial::refuse`), and, for one credential that serves several devices, its
first try to one device at a time; an id that
is never a family's prefix alone, and, where the protocol has no login, a change only to a device
that positively names the MAC it was saved under (section 8); no retries of its own of a change
(the shared layer resends one only at a device's new address, and only when it comes out the same if made twice; `http::send_retrying` is for
the internet, not for devices); an error for a device that did not answer, or for another device
answering at its address, says so in the words `home::who_answered` knows (driver.h lists them: "did
not answer", "could not be reached", "cannot connect", "refused the connection", "closed the
connection"; "answers at this" or "answered at this" address), which is what makes the layer look
for it at a new address (a device that spends half its 2.5 s and then did not answer is not asked
again for its other things in that command: the shared layer gives them the same answer), and no
other error uses them: a device that answered, only wrongly (a reply that could not be read, an error
code, a refusal), is where it was, once it has said who it is; before that, an answer no device of the
family would give may be another device's, and says so (ZB-108; every driver's sentences are in `test_who_answered`,
`tests/unit/smarthome_test.cpp`, and a new one goes there too); honour
`Device::port`; deadlines of about 2.5 s for a change and 4 s for listing;
no threads, sockets or connections of its own between calls; errors are short sentences. Use
the `http::Request` options (`local`, `timeout_ms`, `insecure`, `pinned_pubkey`, `want_peer_cert`,
digest, client certificate), `color.h` and `drivers/support.h` (the JSON depth check and a
guarded parse, `support::parse_reply` or `util::parse_untrusted_json`, before anything a device sent is
parsed; UTF-8 repair and decoding, MAC digits, hex, digests and HMAC, the PEM certificate and
its SHA-256 pin, an RAII socket) rather than private copies; where a family's rule differs (which
characters a name keeps, how a wait ends at its deadline), it keeps its own and says why. Set `local` on every
request to a device (the TLS and digest options imply it): it never uses a proxy from the
environment, returns a redirect instead of following it (a key in a header must not travel to
another host), and speaks only http and https. Build and
test the protocol against a simulated device in `tests/mocks/` first, with an end-to-end test
like `tests/sim/tapo_e2e.py` on `ZB_TEST_NETWORK`, and add the family to the README.

**Releasing.** Bump `project(... VERSION ...)` in `CMakeLists.txt`, build all three presets,
run the full agent set, `scripts/package-deb.sh`, scan tracked files, the binary and the
package for anything private, then commit, push and attach the `.deb` to a GitHub release.
On the device a release is installed with `dpkg -i` or `apt install ./zero-bot_*.deb`, never with
`scripts/deploy.sh`, which copies files without running the package's maintainer scripts: the wake
screen's diversion would never be made.

**The wake-screen picture** (`packaging/lockscreen.png`) replaces one of the launcher's files: the
launcher decodes it once when it starts and draws it unscaled and centred in the 320x150 below its
20 px top bar (its text stays in that bar), so it is exactly 320x150, opaque, with no metadata. The
package's preinst moves the launcher's picture aside with `dpkg-divert --rename` (to
`lockscreen.png.distrib`, where the launcher's updates then go) before ours is unpacked, and its postrm
puts it back when Zero Bot is removed, the way Debian policy describes; never copy over the launcher's
file, which its next update would undo. Either change shows from the launcher's next start (a restart
of the device). `FIRST_WITH_PICTURE` in the postrm is the first version that carries the picture (0.4.9):
a failed upgrade from an older version undoes the diversion. `packaging_test.py` checks that the built
package's version is not below it: the picture came in while the project's version still said 0.4.8, a
version the postrm counts as one without it, so an aborted upgrade back to such a package would have undone
the diversion. The postrm removes only a diversion `dpkg-divert --listpackage` says zero-bot holds: after a
remove, another package may take the picture before the purge, and dpkg-divert refuses to remove its
diversion (the purge would fail).
Downgrades are not handled by the scripts and stay a documented step (README, Install): going back to a
version older than 0.4.9 means removing Zero Bot first (`sudo apt remove zero-bot`); installed over this
one, the older version's scripts leave the diversion in place and the launcher's own picture never comes
back.

## 10. Planned work

Plans are kept locally, not in this repository. On the maintainer's machine, `.local-notes/TICKETS.md`
(git-ignored) holds the sprints, a board and one entry per ticket; if it exists, read the tickets
for the current sprint before starting, keep their status current while you work, and check the
file with `python3 .local-notes/tools/tickets.py check`. Lessons worth keeping belong in section 8
of this file.
