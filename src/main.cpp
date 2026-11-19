#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

#include "agent/agent.h"
#include "agent/experiment.h"
#include "agent/llm.h"
#include "agent/prompt.h"
#include "agent/tools.h"
#include "core/activity.h"
#include "core/music_lib.h"
#include "core/settings.h"
#include "core/store.h"
#include "net/http.h"
#include "platform/codec.h"
#include "smarthome/discovery.h"
#include "smarthome/home.h"
#if !ZB_CLI_ONLY
#include "ui/app.h"
#endif

namespace {

void usage() {
    puts("zero_bot [options]\n"
         "  (no options)          run the app (device screen, or a window in the simulator build)\n"
         "  --ask \"text\"          run one request through the agent and print what happens\n"
         "  --chat                interactive text conversation on the terminal (/new starts a new one;\n"
         "                         /save saves it as SPACE twice does on the device: diagnostics, temporary)\n"
         "  --steer \"text\"        with --ask/--chat: add this while the first tool of the first request runs\n"
         "  --home-refresh         read names and states from the saved smart-home devices on this network\n"
         "                         (ZB_TEST_NETWORK=1: a stand-in network where only 127.x devices are contacted)\n"
         "  --discover             look for smart-home devices on this network (discovery only) and list them\n"
         "  --talk-over MS[:text]  with --ask: hold the microphone open for MS ms from the start of the request,\n"
         "                         then hand over `text` as what was said (or nothing, as if cancelled)\n"
         "  --follow-ups \"a|b|c\"   with --ask: send each of these as the previous answer begins, so that they\n"
         "                         are all handled inside one run\n"
         "  --tool NAME 'JSON'    call one tool directly and print its result\n"
         "  --prompt              print the system prompt a request would get now\n"
         "  --schema              print the tools a request would be offered now (JSON)\n"
         "  --stt FILE.wav        transcribe a 16-bit mono WAV file and print the text\n"
         "  --tts \"text\" OUT.raw  synthesize speech and write raw 16-bit 24 kHz mono PCM\n"
         "  --shots DIR           render the app's screens to PNG files in DIR and exit\n"
         "  --keydump             log raw key codes to stderr while the app runs\n"
         "  --boot                start-at-power-on mode: runs the app once per boot, then does nothing\n"
         "  --version");
}

// Background threads wait on condition variables for the life of the process, and tearing
// those down through normal static destruction can block, so leave without running it.
[[noreturn]] void quit(int code) {
    fflush(stdout);
    fflush(stderr);
    _exit(code);
}

// Terminal front end used for testing and tuning the agent without the screen.
int run_cli(const std::string &ask, bool chat, const std::string &steer_text, const std::string &talk_over,
            const std::string &follow_ups) {
    setenv("ZB_NO_AUDIO", "1", 0);
    AgentEvents ev;
    ev.user_message = [](const std::string &t, bool steering) { printf("%s %s\n", steering ? "[steer]" : "[user]", t.c_str()); };
    static std::chrono::steady_clock::time_point t0;
    static bool first_text;
    auto since = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    // Stands in for the user speaking again mid-request, so the steering path can be tested.
    static std::string steer;
    steer = steer_text;
    ev.tool = [since](const std::string &label) {
        printf("[tool +%.1fs] %s\n", since(), label.c_str());
        if (!steer.empty()) { Agent::get().submit_text(steer); steer.clear(); }
    };
    // Follow-ups queued while an answer is arriving are picked up by the same run.
    static std::vector<std::string> chain;
    chain = util::split(follow_ups, '|');
    ev.answer_delta = [since](const std::string &) {
        if (!first_text) {
            first_text = true;
            printf("[first words +%.1fs]\n", since());
        }
    };
    ev.answer_begin = [] {
        if (!chain.empty()) { Agent::get().submit_text(chain.front()); chain.erase(chain.begin()); }
    };
    ev.answer_discard = [] { first_text = false; puts("[draft withdrawn]"); };
    ev.error = [](const std::string &m) { printf("[error] %s\n", m.c_str()); };
    ev.image = [](const std::string &p) { printf("[photo] %s\n", p.c_str()); };
    Agent &agent = Agent::get();
    agent.start(ev);
    auto one = [&](const std::string &text) {
        const size_t before = agent.history_size();
        t0 = std::chrono::steady_clock::now();
        first_text = false;
        // Stands in for the user pressing the talk key while the request is being worked on.
        std::thread talker;
        if (!talk_over.empty()) {
            agent.set_listening(true);
            puts("[mic open +0.0s]");
            talker = std::thread([&, since] {
                const size_t colon = talk_over.find(':');
                const int ms = atoi(talk_over.substr(0, colon).c_str());
                bool told = false;
                for (int waited = 0; waited < ms; waited += 20) {
                    usleep(20 * 1000);
                    if (!told && agent.state() == AgentState::Speaking) { told = true; printf("[SPEAKING WHILE THE MIC IS OPEN +%.1fs]\n", since()); }
                }
                if (colon != std::string::npos) agent.submit_text(talk_over.substr(colon + 1));
                printf("[mic closed +%.1fs]%s\n", since(), colon != std::string::npos ? " (words handed over)" : " (cancelled)");
                agent.set_listening(false);
            });
        }
        const std::string answer = agent.run_blocking(text);
        if (talker.joinable()) talker.join();
        const double total = since();
        const json hist = agent.history_snapshot();
        for (size_t i = before; i < hist.size(); ++i) {
            const json &m = hist[i];
            if (m.contains("tool_calls"))
                for (auto &c : m["tool_calls"])
                    printf("  -> %s %s\n", c["function"]["name"].get<std::string>().c_str(),
                           c["function"]["arguments"].get<std::string>().c_str());
            else if (util::jget(m, "role", std::string()) == "tool")
                printf("  <- %s\n", util::truncate_utf8(m["content"].get<std::string>(), 300).c_str());
            else if (util::jget(m, "role", std::string()) == "user")
                printf("  ? %s\n", util::truncate_utf8(m["content"].get<std::string>(), 120).c_str());
            else if (util::jget(m, "role", std::string()) == "assistant" && i + 1 < hist.size())
                printf("  = %s\n", util::truncate_utf8(util::jget(m, "content", std::string()), 160).c_str());
        }
        printf("[answer %.1fs] %s\n", total, answer.c_str());
        fflush(stdout);
    };
    if (!ask.empty()) one(ask);
    if (chat) {
        std::string line;
        while (printf("> "), fflush(stdout), std::getline(std::cin, line)) {
            if (line == "/new") { agent.reset_conversation(); continue; }
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
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    // The C library asks for a name's IPv4 and IPv6 addresses at the same moment over one socket.
    // Some Wi-Fi routers and adapters lose the second reply, and every lookup then waits out a
    // five-second timeout (measured on a CardputerZero: 5.04 s against 0.03 s with this option).
    // Asking one after the other costs a few milliseconds. Must be set before the first lookup.
    setenv("RES_OPTIONS", "single-request", 0);
    std::string ask, shots, steer, talk_over, follow_ups, tool_name, tool_args = "{}";
    std::string stt_file, tts_text, tts_out, bench_wav, bench_opus;
    bool chat = false, keydump = false, boot = false, discover = false, home_refresh = false, show_prompt = false, show_schema = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--ask" && i + 1 < argc) ask = argv[++i];
        else if (a == "--chat") chat = true;
        else if (a == "--steer" && i + 1 < argc) steer = argv[++i];
        else if (a == "--discover") discover = true;
        else if (a == "--home-refresh") home_refresh = true;
        else if (a == "--talk-over" && i + 1 < argc) talk_over = argv[++i];
        else if (a == "--follow-ups" && i + 1 < argc) follow_ups = argv[++i];
        else if (a == "--tool" && i + 1 < argc) { tool_name = argv[++i]; if (i + 1 < argc) tool_args = argv[++i]; }
        else if (a == "--shots" && i + 1 < argc) shots = argv[++i];
        else if (a == "--prompt") show_prompt = true;
        else if (a == "--schema") show_schema = true;
        else if (a == "--codec-bench" && i + 2 < argc) { bench_wav = argv[++i]; bench_opus = argv[++i]; }
        else if (a == "--stt" && i + 1 < argc) stt_file = argv[++i];
        else if (a == "--tts" && i + 2 < argc) { tts_text = argv[++i]; tts_out = argv[++i]; }
        else if (a == "--keydump") keydump = true;
        else if (a == "--boot") boot = true;
        else if (a == "--version") { puts("zero_bot " ZB_VERSION); return 0; }
        else { usage(); return a == "--help" || a == "-h" ? 0 : 2; }
    }

    if (!shots.empty()) {
        // Screenshots use sample data; keep it away from the user's real settings and calendar.
        const std::string state = shots + "/.state";
        setenv("XDG_CONFIG_HOME", (state + "/config").c_str(), 1);
        setenv("XDG_DATA_HOME", (state + "/data").c_str(), 1);
        setenv("XDG_CACHE_HOME", (state + "/cache").c_str(), 1);
    }
    if (boot) {
        // The launcher's service runs this before starting the launcher itself. A marker in the
        // per-boot runtime directory makes sure the app opens once after power-on and that
        // later launcher restarts go straight to the launcher.
        const char *runtime = getenv("XDG_RUNTIME_DIR");
        const std::string marker = std::string(runtime && *runtime ? runtime : "/tmp") + "/zero-bot.booted";
        if (access(marker.c_str(), F_OK) == 0) return 0;
        if (FILE *f = fopen(marker.c_str(), "w")) fclose(f);
    }
    http::global_init();
    Settings::load();
    Store::get().load();

    if (!bench_wav.empty()) {
        // Measures how long this machine takes to compress speech and decompress a reply.
        std::string wav, ogg, mp3;
        if (!util::read_file(bench_wav, wav) || !util::read_file(bench_opus, ogg)) { fprintf(stderr, "cannot read inputs\n"); quit(1); }
        std::vector<int16_t> pcm((wav.size() - 44) / 2), out;
        memcpy(pcm.data(), wav.data() + 44, pcm.size() * 2);
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
        quit(0);
    }
    if (!stt_file.empty() || !tts_text.empty()) {
        // Speech round-trip checks, with the size sent or received so compression can be seen.
        std::string data, err;
        const auto t0 = std::chrono::steady_clock::now();
        auto secs = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
        if (!stt_file.empty()) {
            if (!util::read_file(stt_file, data)) { fprintf(stderr, "cannot read %s\n", stt_file.c_str()); quit(1); }
            const std::string text = llm::transcribe(data, err);
            printf("%s\n[%.1fs]%s%s\n", text.c_str(), secs(), err.empty() ? "" : " error: ", err.c_str());
        } else {
            const bool ok = llm::synthesize(tts_text, data, err);
            if (ok) util::write_file_atomic(tts_out, data, 0644);
            printf("%s %zu bytes of PCM (%.1f s of audio) [%.1fs]%s%s\n", ok ? "wrote" : "failed,", data.size(),
                   static_cast<double>(data.size()) / 2 / llm::kTtsRate, secs(), err.empty() ? "" : " error: ", err.c_str());
        }
        quit(err.empty() ? 0 : 1);
    }
    if (home_refresh) {
        const discovery::Network net = home::network_now(true);
        if (!net.usable) { printf("no usable network: %s\n", net.why_not.c_str()); quit(1); }
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
        quit(0);
    }
    if (discover) {
        if (home::test_network()) { puts("the test network is in use (ZB_TEST_NETWORK), so nothing is scanned"); quit(1); }
        const discovery::Network net = discovery::current_network();
        printf("network: %s (%s), id %s%s\n", net.label().c_str(), net.iface.c_str(), net.id().c_str(),
               net.usable ? "" : (" -- " + net.why_not).c_str());
        std::vector<discovery::Device> found;
        std::string err;
        bool heard = false;
        discovery::Options opt;
        opt.on_device = [](const discovery::Device &d) { printf("  + %s (%s)\n", d.name.c_str(), d.kind.c_str()); fflush(stdout); };
        if (!discovery::scan(opt, found, err, &heard)) { printf("scan failed: %s\n", err.c_str()); quit(1); }
        printf("heard own multicast: %s; %zu device(s)\n", heard ? "yes" : "no", found.size());
        for (const auto &d : found) {
            std::string how, info;
            for (const auto &m : d.found_by) how += (how.empty() ? "" : ",") + m;
            for (const auto &kv : d.info)
                if (kv.first != "owner") info += " " + kv.first + "=" + util::truncate_utf8(kv.second, 50);
            printf("%-11s %-26s %-16s %-15s port %-5d via %-10s%s\n", d.kind.c_str(), d.name.c_str(), d.model.c_str(), d.ip.c_str(),
                   d.port, how.c_str(), info.c_str());
        }
        quit(0);
    }
    if (show_prompt || show_schema) {
        // What the model would be given for a request made now (tools appear and disappear with
        // the user's files: radio stations, the smart home on this network, a search key).
        MusicLibrary::get().refresh(Settings::get().eff_music_dir());
        if (show_prompt) puts(prompt::system_prompt().c_str());
        if (show_schema) puts(tools::schema().dump(2, ' ', false, json::error_handler_t::replace).c_str());
        quit(0);
    }
    if (!tool_name.empty()) {
        MusicLibrary::get().refresh(Settings::get().eff_music_dir());
        tools::Context ctx;
        puts(tools::run(tool_name, tool_args, ctx).c_str());
        quit(0);
    }
    if (!ask.empty() || chat) {
        MusicLibrary::get().refresh(Settings::get().eff_music_dir());
        quit(run_cli(ask, chat, steer, talk_over, follow_ups));
    }
#if ZB_CLI_ONLY
    (void)keydump;
    usage();
    quit(2);
#else
    // Reading tags from a large library takes a moment; do it off the UI thread.
    std::thread([] { MusicLibrary::get().refresh(Settings::get().eff_music_dir()); }).detach();
    quit(ui::run_app(shots, keydump));
#endif
}
