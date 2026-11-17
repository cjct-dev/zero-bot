#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

#include "agent/agent.h"
#include "agent/tools.h"
#include "core/music_lib.h"
#include "core/settings.h"
#include "core/store.h"
#include "net/http.h"
#if !ZB_CLI_ONLY
#include "ui/app.h"
#endif

namespace {

void usage() {
    puts("zero_bot [options]\n"
         "  (no options)          run the app (device screen, or a window in the simulator build)\n"
         "  --ask \"text\"          run one request through the agent and print what happens\n"
         "  --chat                interactive text conversation on the terminal\n"
         "  --steer \"text\"        with --ask/--chat: add this while the first tool of the first request runs\n"
         "  --tool NAME 'JSON'    call one tool directly and print its result\n"
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
int run_cli(const std::string &ask, bool chat, const std::string &steer_text) {
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
    ev.answer_delta = [since](const std::string &) {
        if (!first_text) { first_text = true; printf("[first words +%.1fs]\n", since()); }
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
        const std::string answer = agent.run_blocking(text);
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
        }
        printf("[answer %.1fs] %s\n", total, answer.c_str());
        fflush(stdout);
    };
    if (!ask.empty()) one(ask);
    if (chat) {
        std::string line;
        while (printf("> "), fflush(stdout), std::getline(std::cin, line)) {
            if (line == "/new") { agent.reset_conversation(); continue; }
            if (!util::trim(line).empty()) one(line);
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    std::string ask, shots, steer, tool_name, tool_args = "{}";
    bool chat = false, keydump = false, boot = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--ask" && i + 1 < argc) ask = argv[++i];
        else if (a == "--chat") chat = true;
        else if (a == "--steer" && i + 1 < argc) steer = argv[++i];
        else if (a == "--tool" && i + 1 < argc) { tool_name = argv[++i]; if (i + 1 < argc) tool_args = argv[++i]; }
        else if (a == "--shots" && i + 1 < argc) shots = argv[++i];
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
    Settings::get().load();
    Store::get().load();

    if (!tool_name.empty()) {
        MusicLibrary::get().refresh(Settings::get().eff_music_dir());
        tools::Context ctx;
        puts(tools::run(tool_name, tool_args, ctx).c_str());
        quit(0);
    }
    if (!ask.empty() || chat) {
        MusicLibrary::get().refresh(Settings::get().eff_music_dir());
        quit(run_cli(ask, chat, steer));
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
