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
                    timer words mean, the alarm's logic), music library, breathing timing,
                    start-at-power-on, phone tethering, utilities (dates, files, strings)
src/net/            libcurl wrapper with a shared connection/DNS/TLS-session cache
src/platform/       display, keyboard, audio (recorder, speaker, music), codecs, camera,
                    infrared, subprocess helpers
src/smarthome/      discovery (mDNS, SSDP, TP-Link broadcast); driver.h, the contract every
                    device family implements; home.h, the layer above it: store.cpp (the 0600
                    file, migration, the network gate), targets.cpp (names, rooms, groups),
                    control.cpp (commands, safety rules, locking, status, refresh, pairing),
                    tool.cpp (the smart_home tool); color.h (colour names and conversions);
                    drivers/ (one per family; registry.cpp lists them); Tapo transport (tapo)
src/ui/             app controller (keys, timers, state), chat screen, menu system, settings
                    pages, breathing screen, alarm banner, theme
tests/              scored agent test set (needs a live service), device-free unit tests
                    (run_unit.sh: smart home, HTTP options, timers, Fire TV, HomeKit, Hue), Tapo,
                    Fire TV, HomeKit and Hue end to end (tapo_e2e.py, firetv_e2e.py, homekit_e2e.py,
                    hue_e2e.py),
                    the loop against a scripted chat service (home_chat_e2e.py), simulated devices
                    and service (mocks/)
scripts/            toolchain setup, deploy over ADB, .deb packaging, ADB wrapper
```

Threads: the UI thread runs LVGL and the main loop in `ui/app.cpp`. The agent has one worker
thread (`Agent::worker`), speech has one (`SpeechQueue::run`), keys are read on one, music has
one per queue. Anything a worker wants on screen goes through `uiq::post()`; never touch LVGL
from another thread. Slow work started from a menu uses `background(work, done)` in
`settings_pages.cpp`.

State on disk (all under the user's home, mode 0600 where they hold secrets):
`~/.config/zero-bot/settings.json`, and in `~/.local/share/zero-bot/`: `calendar.json`,
`reminders.json` (running timers too, as records with `"kind": "timer"`), `memory.json`,
`ir_learned.json`, `music_index.json`, `smarthome.json`.

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
  (`Agent::set_listening`).
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

What exists:

- **Device-free tests**: `tests/run_unit.sh` (g++, nothing else) builds and runs
  `tests/unit/smarthome_test.cpp` (store and migration, names and rooms, schema snapshots in
  `tests/snapshots/`, the safety questions, rate limits, locking, pairing, the Tapo driver's
  refusals, colours, discovery from sample packets through `discovery::devices_from`; devices are
  a fake driver), `tests/unit/firetv_test.cpp` and `tests/unit/homekit_test.cpp` (each driver's
  known answers; Fire TV's test certificates are in `tests/mocks/firetv/`),
  `tests/unit/hue_test.cpp` (the Hue driver's known answers: certificate pins, colours, resource
  shapes, refusals kept in the secrets; its test certificates are in `tests/mocks/hue/certs/`),
  `tests/unit/http_test.cpp` (the request
  options for devices, against a local peer) and `tests/unit/timers_test.cpp` (the timer tool,
  storage, the countdown, the alarm, its player against a stand-in `pw-cat`; it keeps everything
  it writes in a temporary folder of its own, so running it by hand is safe too), then runs the
  timer tool through `build/cli/zero_bot --tool timer` if that is built. `ZB_UPDATE_SNAPSHOTS=1`
  rewrites the snapshots: read the diff before committing it. `ZB_BUILD_WRAPPER=cmd` runs each
  compile through `cmd`.
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
- **Code ported from other projects** keeps their notices in the source file and in
  `licenses/THIRD-PARTY-NOTICES.md`, which the package installs; add to it with any new port.
- **The request loop, scripted**: `tests/home_chat_e2e.py` runs `build/cli/zero_bot --chat` against
  `tests/mocks/mock_chat.py` (a chat-completions service that answers from a script, photo
  questions included) and simulated plugs and strips, with `ZB_FAKE_CAMERA` for a photo of a sign
  that tries to switch them, and checks from the devices' side that a change needing a yes waits
  for one. Use the scripted service for any change to the loop that a live model cannot be
  trusted to test.

- **Command-line modes** in `src/main.cpp` (work in the `cli` build):
  `--ask "text"`, `--chat`, `--tool NAME 'JSON'`, `--steer "text"` (inject input during the
  first tool), `--talk-over MS[:text]` (hold the microphone open during a request),
  `--follow-ups "a|b|c"` (chain requests inside one run), `--stt FILE.wav`, `--tts "text" OUT.raw`,
  `--discover` (smart-home scan), `--home-refresh` (read saved smart-home devices),
  `--shots DIR` (render every screen to PNG; `sim` build), `--keydump`, `--version`.
- **Scored agent set**: `tests/agent_eval.py` runs spoken-style requests through the `cli`
  build against a live service and checks tool calls, arguments and answers. It needs
  `ZB_API_BASE`, `ZB_API_KEY`, optionally `ZB_SEARCH_KEY`, and model names in `ZB_EVAL_LLM_MODEL`
  etc. Use `--only`, `--repeat N` (models are not deterministic: repeat a changed case 5 to 8
  times) and `--fast 0|1`. Expect one or two flaky cases; compare against a run of the
  previous code before blaming a change.
- **Environment switches**: `ZB_DRY_RUN=1` (tools do not play music, send infrared or switch
  devices), `ZB_NO_AUDIO=1`, `ZB_FAKE_CAMERA=file.jpg`, `ZB_FAST_MODE=0|1`, `ZB_DATE_ORDER=dmy|mdy`,
  `ZB_TEST_NETWORK=1` (a stand-in network, id `net-58678b031ae6e411`, instead of the real one:
  only devices at 127.x addresses are contacted and nothing is scanned; use it for any smart-home
  test on a PC, never `--discover`).
- **Screens**: `build/sim/zero_bot --shots out/` and look at the PNGs. Every UI change should
  be checked this way before it goes to the device. The script in `App::render_shots` walks
  the menus by counting key presses, so adding a settings row shifts later screenshots: fix
  the counts in the same change.
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
- Text that came from the network or a device (names, descriptions) is untrusted: strip
  control characters, cap the length, pass it as data.
- nlohmann::json: a function taking `const json &` called with `{x}`, where `x` is already a
  json value, gets a copy of `x`, not a one-element array (nested lists do become arrays). Build
  lists as `std::vector<json>` or `json::array({...})`. It also sorts object keys when dumping.

**The loop**
- Requests chained inside one run shared one budget of rounds and the last was silently
  dropped. A follow-up now gets a fresh budget, and running out is spoken.
- Anything "known" about the world goes stale: the calendar refuses one-off events in the past
  because a date said without a year means its next occurrence.

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
- Smart-home discovery uses only what the devices' own apps use (mDNS, SSDP, the vendor's
  broadcast). No address sweeps, no port scans, opt-in per network with a warning. "This
  network" is the SSID plus the router's hardware address, not the SSID alone.
- Before promising a device can be controlled, check whether it has a local interface at all.
  Several popular products are cloud-only.
- New device code is reviewed before it touches a real device: in this project the first
  independent review of each driver and of the smart-home layer found blockers every time (a
  login that would lock out every plug of a second account, a confirmation the model could skip,
  a discovery port that sent pairing to the wrong service). A second review found only smaller
  problems; two rounds and a final check was a sensible stopping point.

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
- A multi-agent workflow resumed after an interruption reuses finished steps only when it makes
  the same calls in the same order. To continue with a changed plan, save the finished results to
  a file and start a new run from there.

## 9. Adding things

**A tool.** In `src/agent/tools.cpp`: write `t_name(const json &, Context &)` returning text or
JSON for the model (use `fail("...")` for errors, phrased as an instruction about what to do
next); add it to `build()` with a description written for the model and a compact schema; add
it to `kConfirmable` if success is fully known from the arguments. If it needs the screen or
hardware, add a callback to `tools::Context` and an event in `AgentEvents`, set in
`Agent::run_turn` and handled in `App::wire_agent` through `uiq::post`. Honour `dry_run()`.
Add cases to `tests/agent_eval.py`, including a neighbour that must not trigger it. Document
it in `README.md`.

**A setting.** Add the field to `Settings` (`src/core/settings.{h,cpp}`: member, `to_json`,
`from_json`), a row in `open_settings` (and the setup guide if a new user needs it), and a line
in the README's settings table. Fix the screenshot key-press counts.

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
it has all of its room, and beside the countdown it is cut short with dots instead.

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
a later run; honour `Device::port`; deadlines of about 2.5 s for a change and 4 s for listing;
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
