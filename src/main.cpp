#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

#include "agent/agent.h"
#include "agent/experiment.h"
#include "agent/llm.h"
#include "agent/prompt.h"
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
    bool version = false;
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
                      "/save saves it as SPACE twice does on the device: diagnostics, temporary)",
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
    {"--talk-over", "MS[:text]", 1, "with --ask: hold the microphone open for MS ms from the start of the request\n"
                                    "(or the moment --talk-at names), then hand over `text` as what was said (or\n"
                                    "nothing, as if cancelled)",
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

// Terminal front end used for testing and tuning the agent without the screen (--ask, --ask-wav,
// --chat). A request is typed (--ask, --chat) or spoken (--ask-wav: a recording); so is what
// --steer/--steer-wav add.
int run_cli(const Args &a) {
    const std::string &ask = a.ask, &ask_wav = a.ask_wav, &steer_text = a.steer, &steer_wav = a.steer_wav;
    const std::string &talk_over = a.talk_over, &follow_ups = a.follow_ups;
    const bool chat = a.chat;
    // --talk-at: when --talk-over's microphone opens. At the first tool it is opened by the tool event, at the
    // answer by the talker's thread once the agent is giving one; either way once a request.
    enum class TalkAt { Start, Tool, Answer };
    static TalkAt talk_at;
    if (a.talk_at == "start") talk_at = TalkAt::Start;
    else if (a.talk_at == "tool") talk_at = TalkAt::Tool;
    else if (a.talk_at == "answer") talk_at = TalkAt::Answer;
    else { fprintf(stderr, "--talk-at takes start, tool or answer, not %s\n", a.talk_at.c_str()); return 2; }
    static bool talking;
    talking = !talk_over.empty();
    static std::atomic<bool> mic_opened{false};
    MusicLibrary::get().refresh(Settings::get().eff_music_dir());
    setenv("ZB_NO_AUDIO", "1", 0);
    std::vector<int16_t> ask_pcm;
    static std::vector<int16_t> steer_pcm;
    if (!ask_wav.empty() && !read_speech(ask_wav, ask_pcm)) return 2;
    if (!steer_wav.empty() && !read_speech(steer_wav, steer_pcm)) return 2;
    AgentEvents ev;
    ev.user_message = [](const std::string &t, bool steering) { printf("%s %s\n", steering ? "[steer]" : "[user]", t.c_str()); };
    static std::chrono::steady_clock::time_point t0;
    static bool first_text;
    static double first_at;
    auto since = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    // With speech allowed (ZB_NO_AUDIO=0, which the tests use with a stand-in player): when each spoken answer
    // has played out, so that it can be set beside when its request was answered.
    if (const char *v = getenv("ZB_NO_AUDIO"); v && strcmp(v, "0") == 0)
        std::thread([since] {
            for (bool was = false;; usleep(10 * 1000)) {
                const bool now = SpeechQueue::get().active();
                if (was && !now) printf("[speech done +%.1fs]\n", since());
                was = now;
            }
        }).detach();
    // Stands in for the user speaking again mid-request, so the steering path can be tested.
    static std::string steer;
    steer = steer_text;
    ev.tool = [since](const std::string &label) {
        printf("[tool +%.1fs] %s\n", since(), label.c_str());
        // --talk-at tool: the talk key, pressed as the request's first tool starts.
        if (talking && talk_at == TalkAt::Tool && !mic_opened.exchange(true)) {
            Agent::get().interrupt_speech();
            Agent::get().set_listening(true);
            printf("[mic open +%.1fs]\n", since());
        }
        if (!steer.empty()) { Agent::get().submit_text(steer); steer.clear(); }
        if (!steer_pcm.empty()) { Agent::get().submit_audio(std::move(steer_pcm)); steer_pcm.clear(); }
    };
    // Follow-ups queued while an answer is arriving are picked up by the same run.
    static std::vector<std::string> chain;
    chain = util::split(follow_ups, '|');
    ev.answer_delta = [since](const std::string &) {
        if (!first_text) {
            first_text = true;
            first_at = since();
            printf("[first words +%.1fs]\n", first_at);
        }
    };
    ev.answer_begin = [] {
        if (!chain.empty()) { Agent::get().submit_text(chain.front()); chain.erase(chain.begin()); }
    };
    ev.answer_discard = [] { first_text = false; puts("[draft withdrawn]"); };
    ev.error = [](const std::string &m) { printf("[error] %s\n", m.c_str()); };
    ev.image = [](const std::string &p) { printf("[photo] %s\n", p.c_str()); };
#if ZB_EXPERIMENTS
    // The evaluator's records (tests/agent_eval.py; cli build only). With ZB_EVAL_RECORDS=FILE they go to
    // that file, a channel of their own, one JSON object a line, so nothing the model says can pass for
    // one: each request's start and end (its number, the times, the answer), every tool call of it as the
    // call finishes, whole (from the request's own events: trimming the history hides none, nothing is
    // cut), errors, steering, the saved state after it, "/new", and the end of the run. Each turn's end lists
    // the calls of every model response in it, from the history, so each call record can be matched to the
    // response that asked for it. Every record carries its time since the run began ("mono") and the wall
    // clock ("wall"), to set it beside a simulated device's log. Without the file the call and state records
    // are printed as "[call] {...}" and "[state] {...}" lines, as before.
    static FILE *records = nullptr;
    static std::mutex records_mtx;
    static int request_no = 0, call_no = 0;
    static const auto run_began = std::chrono::steady_clock::now();
    if (const char *path = getenv("ZB_EVAL_RECORDS"); path && *path && !(records = fopen(path, "we"))) {
        fprintf(stderr, "cannot write the records file named by ZB_EVAL_RECORDS: %s\n", strerror(errno));
        return 2;
    }
    static const auto record = [](json r) {
        r["v"] = 2;
        r["mono"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - run_began).count();
        r["wall"] = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        const std::string line = r.dump(-1, ' ', false, json::error_handler_t::replace);
        std::lock_guard<std::mutex> lk(records_mtx);
        fprintf(records, "%s\n", line.c_str());
        fflush(records);
    };
    ev.error = [](const std::string &m) {
        printf("[error] %s\n", m.c_str());
        if (records) record({{"ev", "error"}, {"turn", request_no}, {"message", m}});
    };
    ev.user_message = [](const std::string &t, bool steering) {
        printf("%s %s\n", steering ? "[steer]" : "[user]", t.c_str());
        if (records && steering) record({{"ev", "steer"}, {"turn", request_no}, {"text", t}});
    };
    ev.tool_done = [](const std::string &id, const std::string &name, const std::string &args, const std::string &result) {
        json rec = {{"id", id}, {"name", name}, {"args", args}, {"result", result}};
        if (records) {
            rec["ev"] = "call";
            rec["turn"] = request_no;
            rec["n"] = ++call_no;
            record(rec);
            return;
        }
        printf("[call] %s\n", rec.dump(-1, ' ', false, json::error_handler_t::replace).c_str());
        fflush(stdout);
    };
    // What the calendar, reminders (timers among them) and memory files hold, and the saved state of every
    // smart-home thing (its name, room and state; nothing else of the file). A file that does not exist is
    // null and named in "absent"; one that cannot be read, or holds something of the wrong shape, is null
    // and named in "unreadable": the scorer tells both from an empty list.
    auto state_record = [] {
        json st = {{"now", static_cast<long long>(time(nullptr))}}, unreadable = json::array(), absent = json::array();
        for (const char *name : {"reminders", "calendar", "memory", "smarthome"}) {
            const std::string path = util::data_dir() + "/" + name + ".json";
            const std::string key = std::string(name) == "smarthome" ? "home" : name;
            std::string text;
            struct stat sb;
            st[key] = json();
            if (stat(path.c_str(), &sb) != 0 && errno == ENOENT) { absent.push_back(key); continue; }
            json j = util::read_file(path, text) ? json::parse(text, nullptr, false) : json(json::value_t::discarded);
            if (key != "home") {
                if (j.is_discarded() || !j.is_array()) unreadable.push_back(key);
                else st[key] = std::move(j);
                continue;
            }
            json things = json::array();
            const json *nets = j.is_object() ? &j["networks"] : nullptr;
            if (!nets || !nets->is_object()) { unreadable.push_back(key); continue; }
            for (const auto &net : nets->items())
                for (const auto &d : util::jget(net.value(), "devices", json::array()))
                    for (const auto &t : util::jget(d, "things", json::array()))
                        things.push_back({{"device", util::jget(d, "id", std::string())}, {"name", util::jget(t, "name", std::string())},
                                          {"room", util::jget(t, "room", std::string())}, {"state", util::jget(t, "state", json::object())}});
            st[key] = std::move(things);
        }
        if (!unreadable.empty()) st["unreadable"] = unreadable;
        if (!absent.empty()) st["absent"] = absent;
        if (records) record({{"ev", "state"}, {"turn", request_no}, {"state", st}});
        else printf("[state] %s\n", st.dump(-1, ' ', false, json::error_handler_t::replace).c_str());
    };
    // What this build can do for the test runners, which refuse a binary without it (tests/agent_eval.py
    // probe_binary): these records, the stand-in web (ZB_EXP_WEB), every tool's web read sent to it ("web_all":
    // search, forecasts, places, exchange rates and prices), and refusing hardware (ZB_EXP_NO_HARDWARE).
    if (records) record({{"ev", "start"}, {"caps", {"records", "web", "web_all", "no_hardware", "net"}}});   // net: ZB_EXP_NET
#endif
    Agent &agent = Agent::get();
    agent.start(ev);
    // A typed request, or a spoken one (pcm; `text` then names its recording in the records).
    auto one = [&](const std::string &text, std::vector<int16_t> pcm = {}) {
        // Where this request's messages will begin, counted over the whole run: the history only grows at
        // its end and loses messages at its front, and the agent counts those it loses.
        size_t gone = 0;
        const size_t held = agent.history_snapshot(&gone).size();   // sets gone: read it only after this statement
        const size_t begins = held + gone;
#if ZB_EXPERIMENTS
        ++request_no;
        call_no = 0;
        if (records) record({{"ev", "turn_start"}, {"turn", request_no}, {"text", text}});
#endif
        t0 = std::chrono::steady_clock::now();
        first_text = false;
        first_at = -1;
        // Stands in for the user pressing the talk key while the request is being worked on (--talk-over, at the
        // moment --talk-at names).
        std::thread talker;
        std::atomic<bool> over{false};   // the request has been answered: a key not pressed yet never is
        if (talking) {
            mic_opened = talk_at == TalkAt::Start;
            if (mic_opened) {
                agent.set_listening(true);
                puts("[mic open +0.0s]");
            }
            // An answer being given: spoken (a spoken request's) or only shown (a typed one's).
            auto giving = [&agent] {
                const AgentState st = agent.state();
                return st == AgentState::Speaking || st == AgentState::Answering;
            };
            talker = std::thread([&, since, giving] {
                if (talk_at == TalkAt::Answer) {
                    while (!over && !giving()) usleep(5 * 1000);
                    if (over) return;
                    const bool spoken = agent.state() == AgentState::Speaking;
                    agent.interrupt_speech();
                    agent.set_listening(true);
                    mic_opened = true;
                    printf("[mic open +%.1fs] (while %s)\n", since(), spoken ? "speaking" : "answering");
                }
                while (!mic_opened && !over) usleep(5 * 1000);
                if (!mic_opened) return;
                const size_t colon = talk_over.find(':');
                const int ms = atoi(talk_over.substr(0, colon).c_str());
                // Nothing new is given (spoken, or shown as an answer) while the microphone is open; an answer
                // already under way when the key was pressed is not news.
                bool told = talk_at == TalkAt::Answer;
                for (int waited = 0; waited < ms; waited += 20) {
                    usleep(20 * 1000);
                    if (!told && giving()) { told = true; printf("[SPEAKING WHILE THE MIC IS OPEN +%.1fs]\n", since()); }
                }
                if (colon != std::string::npos) agent.submit_text(talk_over.substr(colon + 1));
                printf("[mic closed +%.1fs]%s\n", since(), colon != std::string::npos ? " (words handed over)" : " (cancelled)");
                agent.set_listening(false);
            });
        }
        const std::string answer = pcm.empty() ? agent.run_blocking(text) : agent.run_blocking(std::move(pcm));
        over = true;
        if (talker.joinable()) talker.join();
        const double total = since();
        const json hist = agent.history_snapshot(&gone);
        size_t tool_calls = 0;
#if ZB_EXPERIMENTS
        json responses = json::array();   // the calls of each model response in this request, for the records
#endif
        for (size_t i = begins > gone ? begins - gone : 0; i < hist.size(); ++i) {
            const json &m = hist[i];
            if (m.contains("tool_calls")) {
#if ZB_EXPERIMENTS
                json asked = json::array();
#endif
                for (auto &c : m["tool_calls"]) {
                    ++tool_calls;
                    printf("  -> %s %s\n", c["function"]["name"].get<std::string>().c_str(),
                           c["function"]["arguments"].get<std::string>().c_str());
#if ZB_EXPERIMENTS
                    asked.push_back({{"id", util::jget(c, "id", std::string())}, {"name", c["function"]["name"]},
                                     {"args", c["function"]["arguments"]}});
#endif
                }
#if ZB_EXPERIMENTS
                responses.push_back(asked);
#endif
            }
            else if (util::jget(m, "role", std::string()) == "tool")
                printf("  <- %s\n", util::truncate_utf8(m["content"].get<std::string>(), 300).c_str());
            else if (util::jget(m, "role", std::string()) == "user")
                printf("  ? %s\n", util::truncate_utf8(m["content"].get<std::string>(), 120).c_str());
            else if (util::jget(m, "role", std::string()) == "assistant" && i + 1 < hist.size())
                printf("  = %s\n", util::truncate_utf8(util::jget(m, "content", std::string()), 160).c_str());
        }
#if ZB_EXPERIMENTS
        state_record();   // before the answer, whose text runs to the next prompt
        if (records)
            record({{"ev", "turn_end"}, {"turn", request_no}, {"total", total}, {"first", first_at >= 0 ? json(first_at) : json()},
                    {"answer", answer}, {"tool_calls", tool_calls}, {"responses", responses}});
#else
        (void)tool_calls;
#endif
        printf("[answer %.1fs] %s\n", total, answer.c_str());
        fflush(stdout);
    };
    if (!ask.empty()) one(ask);
    if (!ask_wav.empty()) one(ask_wav, std::move(ask_pcm));
    if (chat) {
        std::string line;
        while (printf("> "), fflush(stdout), std::getline(std::cin, line)) {
            if (line == "/new") {
                agent.reset_conversation();
#if ZB_EXPERIMENTS
                if (records) record({{"ev", "new"}});
#endif
                continue;
            }
            // DIAGNOSTICS (temporary, ZB-051): what SPACE twice does on the chat screen.
            if (line == "/save") {
                size_t dropped = 0;
                const json conversation = agent.whole_conversation(&dropped);
                const activity::SaveResult r = activity::save_conversation(conversation, dropped);
                printf("[%s]%s%s\n", activity::saved_toast(r).c_str(), r.path.empty() ? "" : " ", r.path.c_str());
                continue;
            }
            if (!util::trim(line).empty()) one(line);
        }
    }
    experiment::wait_probes(30000);   // speech probes of the experiments' trace (ZB_EXP_TTS_PROBE) still running
    // Spoken answers still being synthesised or played (none with ZB_NO_AUDIO).
    for (int waited = 0; SpeechQueue::get().active() && waited < 30000; waited += 20) usleep(20 * 1000);
#if ZB_EXPERIMENTS
    if (records) record({{"ev", "end"}, {"turns", request_no}});
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
