#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

#include "agent/agent.h"
#include "agent/experiment.h"
#include "agent/llm.h"
#include "agent/prompt.h"
#include "agent/search/search.h"
#include "agent/search/usage.h"
#include "agent/speech.h"
#include "agent/tools.h"
#include "core/activity.h"
#include "core/music_lib.h"
#include "core/settings.h"
#include "core/store.h"
#include "core/upgrade.h"
#include "net/http.h"
#include "platform/audio.h"
#include "platform/codec.h"
#include "smarthome/discovery.h"
#include "smarthome/home.h"
#if !ZB_CLI_ONLY
#include "ui/app.h"
#endif

namespace {

// What the command line asked for.
struct Args {
    std::string ask, ask_wav, steer, steer_wav, talk_over, talk_at = "start", follow_ups;
    std::string tool_name, tool_args = "{}";
    std::string stt_file, tts_text, tts_out, bench_wav, bench_opus, shots;
    bool chat = false, keydump = false, discover = false, home_refresh = false, show_prompt = false, show_schema = false;
    bool search_status = false, version = false;
};

// One option: its flag, its arguments as the usage line names them and how many must follow, what it
// does (lines after the first are indented under it), and how it is recorded (argv[i] is the flag; take
// moves i past what it uses).
struct Option {
    const char *flag, *args;
    int count;
    const char *what;
    void (*take)(Args &a, char **argv, int &i, int argc);
};

const Option kOptions[] = {
    {"--ask", "\"text\"", 1, "run one request through the agent and print what happens",
     [](Args &a, char **v, int &i, int) { a.ask = v[++i]; }},
    {"--chat", "", 0, "interactive text conversation on the terminal (/new starts a new one;\n"
                      "/wav FILE.wav says a request aloud, as --ask-wav does; /save saves it\n"
                      "as SPACE twice does on the device: diagnostics, temporary)",
     [](Args &a, char **, int &, int) { a.chat = true; }},
    {"--ask-wav", "FILE.wav", 1, "run one spoken request: the recording (16-bit mono, 16 kHz) is transcribed by\n"
                                 "the speech-to-text service, and the answer spoken as on the device (not with\n"
                                 "ZB_NO_AUDIO, which this mode sets unless it is set; ZB_NO_AUDIO=0 speaks)",
     [](Args &a, char **v, int &i, int) { a.ask_wav = v[++i]; }},
    {"--steer", "\"text\"", 1, "with --ask/--ask-wav/--chat: add this while the first tool of the first request runs",
     [](Args &a, char **v, int &i, int) { a.steer = v[++i]; }},
    {"--steer-wav", "FILE.wav", 1, "the same, said aloud: the recording is handed over as speech",
     [](Args &a, char **v, int &i, int) { a.steer_wav = v[++i]; }},
    {"--home-refresh", "", 0, "read names and states from the saved smart-home devices on this network\n"
                              "(ZB_TEST_NETWORK=1: a stand-in network where only 127.x devices are contacted)",
     [](Args &a, char **, int &, int) { a.home_refresh = true; }},
    {"--discover", "", 0, "look for smart-home devices on this network (discovery only) and list them",
     [](Args &a, char **, int &, int) { a.discover = true; }},
    {"--talk-over", "MS[+LATE][:text]", 1, "with --ask: hold the microphone open for MS ms from the start of the request\n"
                                           "(or the moment --talk-at names), then hand over `text` as what was said (or\n"
                                           "nothing, as if cancelled); with +LATE, LATE ms after the microphone closes, as\n"
                                           "the app does when it first connects to the phone",
     [](Args &a, char **v, int &i, int) { a.talk_over = v[++i]; }},
    {"--talk-at", "start|tool|answer", 1, "with --talk-over: open the microphone at the start of the request (the default),\n"
                                          "as its first tool starts, or once its answer has begun; the last two press\n"
                                          "the talk key as the device does, stopping any speech first",
     [](Args &a, char **v, int &i, int) { a.talk_at = v[++i]; }},
    {"--follow-ups", "\"a|b|c\"", 1, "with --ask: send each of these as the previous answer begins, so that they\n"
                                     "are all handled inside one run",
     [](Args &a, char **v, int &i, int) { a.follow_ups = v[++i]; }},
    {"--tool", "NAME 'JSON'", 1, "call one tool directly and print its result",
     [](Args &a, char **v, int &i, int argc) {
         a.tool_name = v[++i];
         if (i + 1 < argc) a.tool_args = v[++i];
     }},
    {"--prompt", "", 0, "print the system prompt a request would get now", [](Args &a, char **, int &, int) { a.show_prompt = true; }},
    {"--schema", "", 0, "print the tools a request would be offered now (JSON)", [](Args &a, char **, int &, int) { a.show_schema = true; }},
    {"--search-status", "", 0, "print how each web search service set up stands and its use this month, as the\n"
                               "settings page shows them, asking those that can say for free what is left of\n"
                               "their allowance (Tavily), as the page's button does",
     [](Args &a, char **, int &, int) { a.search_status = true; }},
    {"--codec-bench", "FILE.wav FILE.opus", 2, "time compressing a recording to MP3 and decompressing an Opus reply\n"
                                               "on this machine (three runs)",
     [](Args &a, char **v, int &i, int) { a.bench_wav = v[++i]; a.bench_opus = v[++i]; }},
    {"--stt", "FILE.wav", 1, "transcribe a 16-bit mono WAV file and print the text", [](Args &a, char **v, int &i, int) { a.stt_file = v[++i]; }},
    {"--tts", "\"text\" OUT.raw", 2, "synthesize speech and write raw 16-bit 24 kHz mono PCM",
     [](Args &a, char **v, int &i, int) { a.tts_text = v[++i]; a.tts_out = v[++i]; }},
    {"--shots", "DIR", 1, "render the app's screens to PNG files in DIR and exit", [](Args &a, char **v, int &i, int) { a.shots = v[++i]; }},
    {"--keydump", "", 0, "log raw key codes to stderr while the app runs", [](Args &a, char **, int &, int) { a.keydump = true; }},
    {"--version", "", 0, "", [](Args &a, char **, int &, int) { a.version = true; }},
};

void usage() {
    puts("zero_bot [options]");
    printf("  %-22s%s\n", "(no options)", "run the app (device screen, or a window in the simulator build)");
    for (const Option &o : kOptions) {
        const std::string left = std::string(o.flag) + (*o.args ? " " : "") + o.args;
        if (!*o.what) { printf("  %s\n", left.c_str()); continue; }
        std::string what = o.what;
        for (size_t at = 0; (at = what.find('\n', at)) != std::string::npos; at += 26) what.insert(at + 1, 25, ' ');
        if (left.size() > 21) printf("  %s\n%25s%s\n", left.c_str(), "", what.c_str());   // too long to share its line
        else printf("  %-21s %s\n", left.c_str(), what.c_str());
    }
}

// Background threads wait on condition variables for the life of the process, and tearing
// those down through normal static destruction can block, so leave without running it.
[[noreturn]] void quit(int code) {
    fflush(stdout);
    fflush(stderr);
    _exit(code);
}

// A recording to hand over as speech (--ask-wav, --steer-wav): 16-bit, 16 kHz, as the recorder makes.
bool read_speech(const std::string &path, std::vector<int16_t> &pcm) {
    std::string wav;
    int rate = 0;
    if (util::read_file(path, wav) && audio::pcm_from_wav(wav, pcm, rate) && rate == audio::kMicRate) return true;
    fprintf(stderr, "%s is not a 16-bit WAV recorded at %d Hz\n", path.c_str(), audio::kMicRate);
    return false;
}

// ---------------------------------------------------------------- what the command line times
using Clock = std::chrono::steady_clock;

// When the request under way began, set as each one begins (run_cli). A thread that outlives a request
// takes it once, for what it times from that request: the next one may begin meanwhile.
std::atomic<Clock::rep> g_began{0};

double seconds_since(Clock::rep began) {
    return std::chrono::duration<double>(Clock::now() - Clock::time_point(Clock::duration(began))).count();
}

// With speech allowed (ZB_NO_AUDIO=0, which the tests use with a stand-in player), the command line says
// when each spoken answer has played out, and when music held for a spoken request comes back, each timed
// from its own request, to set beside when that one was answered.
bool audio_observed() {
    static const bool on = [] {
        const char *v = getenv("ZB_NO_AUDIO");
        return v && strcmp(v, "0") == 0;
    }();
    return on;
}

std::atomic<Clock::rep> g_music_held_for{0};   // the request music was held for (hold_music); 0 for none

// A recording handed over as the app hands one over (App::finish_recording): with spoken replies on, music
// already playing is held until the agent knows whether the answer will be heard.
void hold_music() {
    if (!Agent::replies_spoken()) return;
    audio::Music::get().hold(audio::Music::kExchange, true);
    if (!audio_observed()) return;
    g_music_held_for = g_began.load();
    printf("[music held +%.1fs]\n", seconds_since(g_began));
}

// For the rest of the run: each spoken answer is tied to the request under way as its first sentence is
// queued (on the agent's thread, inside that request), and said to be done once it has played out, timed
// from that request; the music is looked at every 10 ms.
void observe_audio() {
    static std::mutex mtx;
    static std::map<int, Clock::rep> began;   // answer -> its request's start
    SpeechQueue::get().set_observer([](int answer, bool over) {
        std::lock_guard<std::mutex> lk(mtx);
        if (!over) { began[answer] = g_began; return; }
        const auto it = began.find(answer);
        if (it == began.end()) return;
        printf("[speech done +%.1fs]\n", seconds_since(it->second));
        began.erase(it);
    });
    std::thread([] {
        for (;; usleep(10 * 1000)) {
            Clock::rep held = g_music_held_for;
            if (held && !audio::Music::get().holding(audio::Music::kExchange) && g_music_held_for.compare_exchange_strong(held, 0))
                printf("[music free +%.1fs]\n", seconds_since(held));
        }
    }).detach();
}

// "/wav FILE.wav" in --chat: a request said aloud, as --ask-wav makes one, which the records name by its
// recording. False for any other line.
bool said_aloud(const std::string &line, const std::function<void(const std::string &, std::vector<int16_t>)> &ask) {
    if (line.compare(0, 5, "/wav ") != 0) return false;
    const std::string wav = util::trim(line.substr(5));
    std::vector<int16_t> pcm;
    if (read_speech(wav, pcm)) ask(wav, std::move(pcm));
    return true;
}

// --talk-over's microphone closes. What was said (none: as if cancelled) is handed over at once, or `late`
// ms after, as App::when_online does while it connects to the phone.
void close_microphone(Agent &agent, const std::optional<std::string> &words, int late) {
    if (words && late > 0) {
        agent.set_input_coming(true);
        printf("[mic closed +%.1fs] (words on their way)\n", seconds_since(g_began));
        agent.set_listening(false);
        usleep(late * 1000);
        agent.submit_text(*words);
        printf("[words handed over +%.1fs]\n", seconds_since(g_began));
        agent.set_input_coming(false);
        return;
    }
    if (words) agent.submit_text(*words);
    printf("[mic closed +%.1fs]%s\n", seconds_since(g_began), words ? " (words handed over)" : " (cancelled)");
    agent.set_listening(false);
}

// ---------------------------------------------------------------- the terminal front end (run_cli)
// --talk-at: when --talk-over's microphone opens. At the first tool it is opened by the tool event, at the
// answer by the talker's thread once the agent is giving one; either way once a request.
enum class TalkAt { Start, Tool, Answer };

// What the run was asked to do beside its requests, set before the first one and read from the agent's
// events and the talker's thread.
struct CliRun {
    std::string talk_over;            // --talk-over, as given (empty: the microphone is never opened)
    TalkAt talk_at = TalkAt::Start;
    std::atomic<bool> mic_opened{false};
    // Stands in for the user speaking again mid-request, so the steering path can be tested.
    std::string steer;
    std::vector<int16_t> steer_pcm;
    std::vector<std::string> chain;   // --follow-ups not sent yet: picked up by the same run
    bool first_text = false;          // the answer under way has shown words
    double first_at = -1;             // when they came, since the request began; -1 for none yet

    bool talking() const { return !talk_over.empty(); }
} g_cli;

// What the run prints as a request goes. A tool's start also stands in for the user acting while it runs
// (--talk-at tool, --steer, --steer-wav), and an answer's start sends the next of --follow-ups.
AgentEvents cli_events() {
    AgentEvents ev;
    ev.user_message = [](const std::string &t, bool steering) { printf("%s %s\n", steering ? "[steer]" : "[user]", t.c_str()); };
    ev.tool = [](const std::string &label) {
        printf("[tool +%.1fs] %s\n", seconds_since(g_began), label.c_str());
        // --talk-at tool: the talk key, pressed as the request's first tool starts.
        if (g_cli.talking() && g_cli.talk_at == TalkAt::Tool && !g_cli.mic_opened.exchange(true)) {
            Agent::get().interrupt_speech();
            Agent::get().set_listening(true);
            printf("[mic open +%.1fs]\n", seconds_since(g_began));
        }
        if (!g_cli.steer.empty()) { Agent::get().submit_text(g_cli.steer); g_cli.steer.clear(); }
        if (!g_cli.steer_pcm.empty()) {
            hold_music();
            Agent::get().submit_audio(std::move(g_cli.steer_pcm));
            g_cli.steer_pcm.clear();
        }
    };
    ev.answer_delta = [](const std::string &) {
        if (!g_cli.first_text) {
            g_cli.first_text = true;
            g_cli.first_at = seconds_since(g_began);
            printf("[first words +%.1fs]\n", g_cli.first_at);
        }
    };
    ev.answer_begin = [] {
        if (!g_cli.chain.empty()) { Agent::get().submit_text(g_cli.chain.front()); g_cli.chain.erase(g_cli.chain.begin()); }
    };
    ev.answer_discard = [] { g_cli.first_text = false; puts("[draft withdrawn]"); };
    ev.error = [](const std::string &m) { printf("[error] %s\n", m.c_str()); };
    ev.image = [](const std::string &p) { printf("[photo] %s\n", p.c_str()); };
    return ev;
}

#if ZB_EXPERIMENTS
// The evaluator's records (tests/agent/agent_eval.py; cli build only). With ZB_EVAL_RECORDS=FILE they go to
// that file, a channel of their own, one JSON object a line, so nothing the model says can pass for
// one: each request's start and end (its number, the times, the answer), every tool call of it as the
// call finishes, whole (from the request's own events: trimming the history hides none, nothing is
// cut), errors, steering, the saved state after it, "/new", and the end of the run. Each turn's end lists
// the calls of every model response in it, from the history, so each call record can be matched to the
// response that asked for it. Every record carries its time since the run began ("mono") and the wall
// clock ("wall"), to set it beside a simulated device's log. Without the file the call and state records
// are printed as "[call] {...}" and "[state] {...}" lines, as before.
struct Records {
    FILE *file = nullptr;
    std::mutex mtx;
    int request = 0, call = 0;                       // the request under way (from 1) and its calls so far
    std::chrono::steady_clock::time_point began;     // when the run began
} g_records;

void record(json r) {
    r["v"] = 2;
    r["mono"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_records.began).count();
    r["wall"] = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string line = r.dump(-1, ' ', false, json::error_handler_t::replace);
    std::lock_guard<std::mutex> lk(g_records.mtx);
    fprintf(g_records.file, "%s\n", line.c_str());
    fflush(g_records.file);
}

// Opens the file ZB_EVAL_RECORDS names, if it names one, and starts it with what this build can do for
// the test runners, which refuse a binary without it (tests/agent/agent_eval.py probe_binary): these
// records, the stand-in web (ZB_EXP_WEB), every tool's web read sent to it ("web_all": search, forecasts,
// places, exchange rates and prices), and refusing hardware (ZB_EXP_NO_HARDWARE). False when it cannot
// be written.
bool open_records() {
    g_records.began = std::chrono::steady_clock::now();
    if (const char *path = getenv("ZB_EVAL_RECORDS"); path && *path && !(g_records.file = fopen(path, "we"))) {
        fprintf(stderr, "cannot write the records file named by ZB_EVAL_RECORDS: %s\n", strerror(errno));
        return false;
    }
    if (g_records.file) record({{"ev", "start"}, {"caps", {"records", "web", "web_all", "no_hardware", "net"}}});   // net: ZB_EXP_NET
    return true;
}

// The records' events over the plain ones: errors and steering recorded too, and every tool call as it
// finishes.
void record_events(AgentEvents &ev) {
    ev.error = [](const std::string &m) {
        printf("[error] %s\n", m.c_str());
        if (g_records.file) record({{"ev", "error"}, {"turn", g_records.request}, {"message", m}});
    };
    ev.user_message = [](const std::string &t, bool steering) {
        printf("%s %s\n", steering ? "[steer]" : "[user]", t.c_str());
        if (g_records.file && steering) record({{"ev", "steer"}, {"turn", g_records.request}, {"text", t}});
    };
    ev.tool_done = [](const std::string &id, const std::string &name, const std::string &args, const std::string &result) {
        json rec = {{"id", id}, {"name", name}, {"args", args}, {"result", result}};
        if (g_records.file) {
            rec["ev"] = "call";
            rec["turn"] = g_records.request;
            rec["n"] = ++g_records.call;
            record(rec);
            return;
        }
        printf("[call] %s\n", rec.dump(-1, ' ', false, json::error_handler_t::replace).c_str());
        fflush(stdout);
    };
}

// The saved state of every smart-home thing in the file's networks: its device, name, room and state
// (nothing else of the file). Null when the file has no networks to read.
json home_things(json &file) {
    const json *nets = file.is_object() ? &file["networks"] : nullptr;
    if (!nets || !nets->is_object()) return json();
    json things = json::array();
    for (const auto &net : nets->items())
        for (const auto &d : util::jget(net.value(), "devices", json::array()))
            for (const auto &t : util::jget(d, "things", json::array()))
                things.push_back({{"device", util::jget(d, "id", std::string())}, {"name", util::jget(t, "name", std::string())},
                                  {"room", util::jget(t, "room", std::string())}, {"state", util::jget(t, "state", json::object())}});
    return things;
}

// What the calendar, reminders (timers among them) and memory files hold, and the saved state of every
// smart-home thing. A file that does not exist is null and named in "absent"; one that cannot be read, or
// holds something of the wrong shape, is null and named in "unreadable": the scorer tells both from an
// empty list.
void record_state() {
    json st = {{"now", static_cast<long long>(time(nullptr))}}, unreadable = json::array(), absent = json::array();
    for (const char *name : {"reminders", "calendar", "memory", "smarthome"}) {
        const std::string path = util::data_dir() + "/" + name + ".json";
        const std::string key = std::string(name) == "smarthome" ? "home" : name;
        std::string text;
        struct stat sb;
        st[key] = json();
        if (stat(path.c_str(), &sb) != 0 && errno == ENOENT) { absent.push_back(key); continue; }
        json j = util::read_file(path, text) ? json::parse(text, nullptr, false) : json(json::value_t::discarded);
        if (key == "home") j = home_things(j);
        if (j.is_array()) st[key] = std::move(j);
        else unreadable.push_back(key);
    }
    if (!unreadable.empty()) st["unreadable"] = unreadable;
    if (!absent.empty()) st["absent"] = absent;
    if (g_records.file) record({{"ev", "state"}, {"turn", g_records.request}, {"state", st}});
    else printf("[state] %s\n", st.dump(-1, ' ', false, json::error_handler_t::replace).c_str());
}

// The calls of each model response in the history from `from` on, for the turn's end record.
json responses_of(const json &hist, size_t from) {
    json responses = json::array();
    for (size_t i = from; i < hist.size(); ++i) {
        if (!hist[i].contains("tool_calls")) continue;
        json asked = json::array();
        for (auto &c : hist[i]["tool_calls"])
            asked.push_back({{"id", util::jget(c, "id", std::string())}, {"name", c["function"]["name"]},
                             {"args", c["function"]["arguments"]}});
        responses.push_back(asked);
    }
    return responses;
}
#endif

// An answer being given: spoken (a spoken request's) or only shown (a typed one's).
bool giving_answer() {
    const AgentState st = Agent::get().state();
    return st == AgentState::Speaking || st == AgentState::Answering;
}

// The talker's thread (--talk-over): the talk key pressed at the answer, if --talk-at says so, then the
// microphone held open for the time given and closed. `over` is set once the request has been answered:
// a key not pressed by then never is.
void talk(const std::atomic<bool> &over) {
    Agent &agent = Agent::get();
    if (g_cli.talk_at == TalkAt::Answer) {
        while (!over && !giving_answer()) usleep(5 * 1000);
        if (over) return;
        const bool spoken = agent.state() == AgentState::Speaking;
        agent.interrupt_speech();
        agent.set_listening(true);
        g_cli.mic_opened = true;
        printf("[mic open +%.1fs] (while %s)\n", seconds_since(g_began), spoken ? "speaking" : "answering");
    }
    while (!g_cli.mic_opened && !over) usleep(5 * 1000);
    if (!g_cli.mic_opened) return;
    const std::string &talk_over = g_cli.talk_over;
    const size_t colon = talk_over.find(':'), plus = talk_over.substr(0, colon).find('+');
    const int ms = atoi(talk_over.substr(0, colon).c_str());
    // Nothing new is given (spoken, or shown as an answer) while the microphone is open; an answer
    // already under way when the key was pressed is not news.
    bool told = g_cli.talk_at == TalkAt::Answer;
    for (int waited = 0; waited < ms; waited += 20) {
        usleep(20 * 1000);
        if (!told && giving_answer()) { told = true; printf("[SPEAKING WHILE THE MIC IS OPEN +%.1fs]\n", seconds_since(g_began)); }
    }
    close_microphone(agent, colon == std::string::npos ? std::nullopt : std::optional(talk_over.substr(colon + 1)),
                     plus == std::string::npos ? 0 : atoi(talk_over.substr(plus + 1).c_str()));
}

// Stands in for the user pressing the talk key while the request is being worked on (--talk-over, at the
// moment --talk-at names): at once for --talk-at start, else from the tool event or the talker's thread.
std::thread start_talker(const std::atomic<bool> &over) {
    if (!g_cli.talking()) return std::thread();
    g_cli.mic_opened = g_cli.talk_at == TalkAt::Start;
    if (g_cli.mic_opened) {
        Agent::get().set_listening(true);
        puts("[mic open +0.0s]");
    }
    return std::thread(talk, std::cref(over));
}

// The request's part of the history, from `from` on, printed: each tool call and result, what the user
// said, and what the assistant said before its answer. How many tool calls it made.
size_t print_request(const json &hist, size_t from) {
    size_t tool_calls = 0;
    for (size_t i = from; i < hist.size(); ++i) {
        const json &m = hist[i];
        if (m.contains("tool_calls")) {
            for (auto &c : m["tool_calls"]) {
                ++tool_calls;
                printf("  -> %s %s\n", c["function"]["name"].get<std::string>().c_str(),
                       c["function"]["arguments"].get<std::string>().c_str());
            }
        }
        else if (util::jget(m, "role", std::string()) == "tool")
            printf("  <- %s\n", util::truncate_utf8(m["content"].get<std::string>(), 300).c_str());
        else if (util::jget(m, "role", std::string()) == "user")
            printf("  ? %s\n", util::truncate_utf8(m["content"].get<std::string>(), 120).c_str());
        else if (util::jget(m, "role", std::string()) == "assistant" && i + 1 < hist.size())
            printf("  = %s\n", util::truncate_utf8(util::jget(m, "content", std::string()), 160).c_str());
    }
    return tool_calls;
}

// One request, typed, or spoken (pcm; `text` then names its recording in the records), run to its answer:
// its steps printed, then the answer with its time.
void run_request(const std::string &text, std::vector<int16_t> pcm = {}) {
    Agent &agent = Agent::get();
    // Where this request's messages will begin, counted over the whole run: the history only grows at
    // its end and loses messages at its front, and the agent counts those it loses.
    size_t gone = 0;
    const size_t held = agent.history_snapshot(&gone).size();   // sets gone: read it only after this statement
    const size_t begins = held + gone;
#if ZB_EXPERIMENTS
    ++g_records.request;
    g_records.call = 0;
    if (g_records.file) record({{"ev", "turn_start"}, {"turn", g_records.request}, {"text", text}});
#endif
    g_began = Clock::now().time_since_epoch().count();
    g_cli.first_text = false;
    g_cli.first_at = -1;
    std::atomic<bool> over{false};   // the request has been answered: a key not pressed yet never is
    std::thread talker = start_talker(over);
    if (!pcm.empty()) hold_music();
    const std::string answer = pcm.empty() ? agent.run_blocking(text) : agent.run_blocking(std::move(pcm));
    over = true;
    if (talker.joinable()) talker.join();
    const double total = seconds_since(g_began);
    const json hist = agent.history_snapshot(&gone);
    const size_t from = begins > gone ? begins - gone : 0;
    const size_t tool_calls = print_request(hist, from);
#if ZB_EXPERIMENTS
    record_state();   // before the answer, whose text runs to the next prompt
    if (g_records.file)
        record({{"ev", "turn_end"}, {"turn", g_records.request}, {"total", total},
                {"first", g_cli.first_at >= 0 ? json(g_cli.first_at) : json()}, {"answer", answer},
                {"tool_calls", tool_calls}, {"responses", responses_of(hist, from)}});
#else
    (void)tool_calls;
#endif
    printf("[answer %.1fs] %s\n", total, answer.c_str());
    fflush(stdout);
}

// --chat: a request a line, until the input ends. "/new" starts a new conversation, "/save" saves it and
// "/wav FILE.wav" says a request aloud.
void chat_loop() {
    std::string line;
    while (printf("> "), fflush(stdout), std::getline(std::cin, line)) {
        if (line == "/new") {
            Agent::get().reset_conversation();
#if ZB_EXPERIMENTS
            if (g_records.file) record({{"ev", "new"}});
#endif
            continue;
        }
        // DIAGNOSTICS (temporary, ZB-051): what SPACE twice does on the chat screen.
        if (line == "/save") {
            size_t dropped = 0;
            const json conversation = Agent::get().whole_conversation(&dropped);
            const activity::SaveResult r = activity::save_conversation(conversation, dropped);
            printf("[%s]%s%s\n", activity::saved_toast(r).c_str(), r.path.empty() ? "" : " ", r.path.c_str());
            continue;
        }
        if (said_aloud(line, run_request)) continue;
        if (!util::trim(line).empty()) run_request(line);
    }
}

// Terminal front end used for testing and tuning the agent without the screen (--ask, --ask-wav,
// --chat). A request is typed (--ask, --chat) or spoken (--ask-wav: a recording); so is what
// --steer/--steer-wav add.
int run_cli(const Args &a) {
    if (a.talk_at == "start") g_cli.talk_at = TalkAt::Start;
    else if (a.talk_at == "tool") g_cli.talk_at = TalkAt::Tool;
    else if (a.talk_at == "answer") g_cli.talk_at = TalkAt::Answer;
    else { fprintf(stderr, "--talk-at takes start, tool or answer, not %s\n", a.talk_at.c_str()); return 2; }
    g_cli.talk_over = a.talk_over;
    MusicLibrary::get().refresh(Settings::get().eff_music_dir());
    setenv("ZB_NO_AUDIO", "1", 0);
    std::vector<int16_t> ask_pcm;
    if (!a.ask_wav.empty() && !read_speech(a.ask_wav, ask_pcm)) return 2;
    if (!a.steer_wav.empty() && !read_speech(a.steer_wav, g_cli.steer_pcm)) return 2;
    if (audio_observed()) observe_audio();
    g_cli.steer = a.steer;
    g_cli.chain = util::split(a.follow_ups, '|');
    AgentEvents ev = cli_events();
#if ZB_EXPERIMENTS
    if (!open_records()) return 2;
    record_events(ev);
#endif
    Agent::get().start(ev);
    if (!a.ask.empty()) run_request(a.ask);
    if (!a.ask_wav.empty()) run_request(a.ask_wav, std::move(ask_pcm));
    if (a.chat) chat_loop();
    experiment::wait_probes(30000);   // speech probes of the experiments' trace (ZB_EXP_TTS_PROBE) still running
    // Spoken answers still being synthesised or played (none with ZB_NO_AUDIO).
    for (int waited = 0; SpeechQueue::get().active() && waited < 30000; waited += 20) usleep(20 * 1000);
#if ZB_EXPERIMENTS
    if (g_records.file) record({{"ev", "end"}, {"turns", g_records.request}});
#endif
    return 0;
}

// Measures how long this machine takes to compress speech and decompress a reply (--codec-bench).
int codec_bench(const Args &a) {
    std::string wav, ogg, mp3;
    std::vector<int16_t> pcm, out;
    int rate = 0;
    if (!util::read_file(a.bench_wav, wav) || !util::read_file(a.bench_opus, ogg) || !audio::pcm_from_wav(wav, pcm, rate)) {
        fprintf(stderr, "cannot read inputs\n");
        return 1;
    }
    auto ms = [](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
    };
    for (int run = 0; run < 3; ++run) {
        auto t = std::chrono::steady_clock::now();
        const bool ok = codec::mp3_encode(pcm, 16000, 32, mp3);
        const double enc = ms(t);
        t = std::chrono::steady_clock::now();
        const bool ok2 = codec::opus_decode(ogg, out);
        printf("run %d: encode %.1f s of speech -> %zu bytes MP3 in %.0f ms (%s); decode %.1f s of Opus in %.0f ms (%s)\n",
               run + 1, pcm.size() / 16000.0, mp3.size(), enc, ok ? "ok" : "FAILED", out.size() / 48000.0, ms(t),
               ok2 ? "ok" : "FAILED");
    }
    return 0;
}

// Speech round-trip checks (--stt, --tts), with the size sent or received so compression can be seen.
int speech_check(const Args &a) {
    std::string data, err;
    const auto t0 = std::chrono::steady_clock::now();
    auto secs = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    if (!a.stt_file.empty()) {
        if (!util::read_file(a.stt_file, data)) { fprintf(stderr, "cannot read %s\n", a.stt_file.c_str()); return 1; }
        const std::string text = llm::transcribe(data, err);
        printf("%s\n[%.1fs]%s%s\n", text.c_str(), secs(), err.empty() ? "" : " error: ", err.c_str());
    } else {
        const bool ok = llm::synthesize(a.tts_text, data, err);
        if (ok) util::write_file_atomic(a.tts_out, data, 0644);
        printf("%s %zu bytes of PCM (%.1f s of audio) [%.1fs]%s%s\n", ok ? "wrote" : "failed,", data.size(),
               static_cast<double>(data.size()) / 2 / llm::kTtsRate, secs(), err.empty() ? "" : " error: ", err.c_str());
    }
    return err.empty() ? 0 : 1;
}

// Reads names and states from the saved smart-home devices on this network (--home-refresh).
int home_refresh(const Args &) {
    const discovery::Network net = home::network_now(true);
    if (!net.usable) { printf("no usable network: %s\n", net.why_not.c_str()); return 1; }
    printf("network: %s, id %s\n", net.label().c_str(), net.id().c_str());
    printf("%s\n", home::refresh(net.id()).c_str());
    for (const auto &d : home::entry(net.id()).devices) {
        printf("%-10s %-24s %s\n", d.kind.c_str(), d.title().c_str(), d.problem.empty() ? "" : ("-- " + d.problem).c_str());
        for (const auto &t : d.things) {
            if (!d.listed(t)) continue;
            const auto on = t.state.find("on");
            const std::string state = on != t.state.end() && on->is_boolean() ? (on->get<bool>() ? "on" : "off") : "-";
            printf("             %-24s %-8s %-4s%s\n", home::spoken(t).c_str(), t.type.c_str(), state.c_str(),
                   t.room.empty() ? "" : ("  room: " + t.room).c_str());
        }
    }
    return 0;
}

// Looks for smart-home devices on this network and lists them (--discover).
int discover(const Args &) {
    if (home::test_network()) { puts("the test network is in use (ZB_TEST_NETWORK), so nothing is scanned"); return 1; }
    const discovery::Network net = discovery::current_network();
    printf("network: %s (%s), id %s%s\n", net.label().c_str(), net.iface.c_str(), net.id().c_str(),
           net.usable ? "" : (" -- " + net.why_not).c_str());
    std::vector<discovery::Device> found;
    std::string err;
    bool heard = false;
    discovery::Options opt;
    opt.on_device = [](const discovery::Device &d) { printf("  + %s (%s)\n", d.name.c_str(), d.kind.c_str()); fflush(stdout); };
    if (!discovery::scan(opt, found, err, &heard)) { printf("scan failed: %s\n", err.c_str()); return 1; }
    printf("heard own multicast: %s; %zu device(s)\n", heard ? "yes" : "no", found.size());
    for (const auto &d : found) {
        std::string how, info;
        for (const auto &m : d.found_by) how += (how.empty() ? "" : ",") + m;
        for (const auto &kv : d.info)
            if (kv.first != "owner") info += " " + kv.first + "=" + util::truncate_utf8(kv.second, 50);
        printf("%-11s %-26s %-16s %-15s port %-5d via %-10s%s\n", d.kind.c_str(), d.name.c_str(), d.model.c_str(), d.ip.c_str(),
               d.port, how.c_str(), info.c_str());
    }
    return 0;
}

// What the model would be given for a request made now (--prompt, --schema): tools appear and disappear
// with the user's files (radio stations, the smart home on this network, a search key).
int show(const Args &a) {
    MusicLibrary::get().refresh(Settings::get().eff_music_dir());
    if (a.show_prompt) puts(prompt::system_prompt().c_str());
    if (a.show_schema) puts(tools::schema().dump(2, ' ', false, json::error_handler_t::replace).c_str());
    return 0;
}

// How each web search service of the settings' list stands (--search-status): one line each, then today's searches.
int search_status(const Args &) {
    const Settings before = Settings::get();
    for (const SearchService &listed : before.search) {
        const search::Provider *p = search::provider_for(listed.kind);
        if (!p) continue;
        tools::Context ctx;
        const std::string asked = p->tells_allowance() && listed.enabled && !listed.key.empty() ? search::ask_allowance(listed.kind, ctx) : "";
        const std::string key = listed.key;
        SearchService e = listed;   // as it is now: asking may have marked its key refused
        const Settings after = Settings::get();
        for (const SearchService &now : after.search)
            if (now.kind == listed.kind && now.key == key) e = now;
        const search::usage::Figures f = search::usage::of(search::id_of(e));
        printf("%s: %s; %ld this month", e.kind.c_str(), search::describe(e).c_str(), f.used);
        if (f.left >= 0) printf("; it said %ld of %ld left", f.left, f.limit);
        printf("%s%s\n", asked.empty() ? "" : "; ", asked.c_str());
    }
    printf("today: %d of %d searches\n", search::usage::today(), search::usage::kDailyCap);
    return 0;
}

// Calls one tool directly and prints its result (--tool).
int tool(const Args &a) {
    MusicLibrary::get().refresh(Settings::get().eff_music_dir());
    tools::Context ctx;
    puts(tools::run(a.tool_name, a.tool_args, ctx).c_str());
    return 0;
}

// The sub-commands, in the order they are tried: the first one asked for runs. With none, the app runs.
const struct {
    bool (*asked)(const Args &);
    int (*run)(const Args &);
} kModes[] = {
    {[](const Args &a) { return !a.bench_wav.empty(); }, codec_bench},
    {[](const Args &a) { return !a.stt_file.empty() || !a.tts_text.empty(); }, speech_check},
    {[](const Args &a) { return a.home_refresh; }, home_refresh},
    {[](const Args &a) { return a.discover; }, discover},
    {[](const Args &a) { return a.show_prompt || a.show_schema; }, show},
    {[](const Args &a) { return a.search_status; }, search_status},
    {[](const Args &a) { return !a.tool_name.empty(); }, tool},
    {[](const Args &a) { return !a.ask.empty() || !a.ask_wav.empty() || a.chat; }, run_cli},
};

}  // namespace

int main(int argc, char **argv) {
    // The C library asks for a name's IPv4 and IPv6 addresses at the same moment over one socket.
    // Some Wi-Fi routers and adapters lose the second reply, and every lookup then waits out a
    // five-second timeout (measured on a CardputerZero: 5.04 s against 0.03 s with this option).
    // Asking one after the other costs a few milliseconds. Must be set before the first lookup.
    setenv("RES_OPTIONS", "single-request", 0);
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const Option *o = nullptr;
        for (const Option &k : kOptions)
            if (a == k.flag && i + k.count < argc) o = &k;
        if (!o) { usage(); return a == "--help" || a == "-h" ? 0 : 2; }
        o->take(args, argv, i, argc);
        if (args.version) { puts("zero_bot " ZB_VERSION); return 0; }
    }

    if (!args.shots.empty()) {
        // Screenshots use sample data; keep it away from the user's real settings and calendar.
        const std::string state = args.shots + "/.state";
        setenv("XDG_CONFIG_HOME", (state + "/config").c_str(), 1);
        setenv("XDG_DATA_HOME", (state + "/data").c_str(), 1);
        setenv("XDG_CACHE_HOME", (state + "/cache").c_str(), 1);
    }
    http::global_init();
    Settings::load();
    Store::get().load();

    for (const auto &mode : kModes)
        if (mode.asked(args)) quit(mode.run(args));
#if ZB_CLI_ONLY
    usage();
    quit(2);
#else
    // Reading tags from a large library takes a moment; do it off the UI thread.
    std::thread([] { MusicLibrary::get().refresh(Settings::get().eff_music_dir()); }).detach();
    if (args.shots.empty()) upgrade::tidy_at_start();
    quit(ui::run_app(args.shots, args.keydump));
#endif
}
