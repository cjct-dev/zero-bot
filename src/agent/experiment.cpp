// See experiment.h. Nothing here is compiled unless the build sets ZB_EXPERIMENTS.
#include "agent/experiment.h"

#if ZB_EXPERIMENTS

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <thread>

#include "agent/llm.h"
#include "agent/tools.h"

namespace experiment {

namespace {

using Clock = std::chrono::steady_clock;

std::mutex g_mtx;                          // the trace file, the current request
const Clock::time_point g_process_start = Clock::now();
int g_req = 0;                             // requests begun in this process
int g_calls = 0;                           // model calls in the current request
Clock::time_point g_req_start;
bool g_in_request = false;
bool g_started = false;                    // the "start" line has been written
std::atomic<int> g_probes{0};              // speech probes still running

long ms_since(Clock::time_point t) {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count());
}

bool env_on(const char *name) {
    const char *v = getenv(name);
    return v && *v && std::string(v) != "0";
}

void write_line(const json &event) {   // g_mtx held
    FILE *f = fopen(get().trace.c_str(), "a");
    if (!f) return;
    const std::string line = event.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
    fwrite(line.data(), 1, line.size(), f);
    fclose(f);
}

}  // namespace

const Switches &get() {
    static const Switches s = [] {
        Switches r;
        if (const char *t = getenv("ZB_EXP_TRACE")) r.trace = t;
        r.cache_bust = env_on("ZB_EXP_CACHE_BUST");
        r.tts_probe = env_on("ZB_EXP_TTS_PROBE");
        return r;
    }();
    return s;
}

void trace(json event) {
    if (!tracing()) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_started) {
        g_started = true;
        write_line({{"ev", "start"}, {"version", ZB_VERSION}, {"pid", static_cast<long>(getpid())},
                    {"cache_bust", get().cache_bust}, {"tts_probe", get().tts_probe}});
    }
    if (!event.contains("ev")) event["ev"] = "note";
    event["at_ms"] = ms_since(g_process_start);
    write_line(event);
}

std::string cache_bust_line() {
    if (!get().cache_bust) return "";
    static const std::string line = [] {
        std::random_device rd;
        char buf[64];
        snprintf(buf, sizeof buf, "(Session %08x%08x.)\n\n", rd(), rd());
        return std::string(buf);
    }();
    return line;
}

long request_ms() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_in_request ? ms_since(g_req_start) : -1;
}

void trace_call(const json &call) {
    if (!tracing()) return;
    json ev = call;
    ev["ev"] = "call";
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        ev["req"] = g_req;
        ev["call"] = ++g_calls;
    }
    trace(ev);
}

json numbers_only(const json &v) {
    if (v.is_number() || v.is_boolean()) return v;
    if (v.is_object()) {
        json out = json::object();
        for (auto it = v.begin(); it != v.end(); ++it) {
            json kept = numbers_only(it.value());
            if (!kept.is_null()) out[it.key()] = kept;
        }
        return out;
    }
    return nullptr;
}

void wait_probes(int max_ms) {
    for (int waited = 0; g_probes > 0 && waited < max_ms; waited += 20) usleep(20 * 1000);
}

// ---------------------------------------------------------------- one request

void RequestTrace::absorbing() {
    if (!tracing()) return;
    absorb_start_ = Clock::now();
    absorb_marked_ = true;
}

void RequestTrace::begin() {
    end();
    if (!tracing()) return;
    const long absorb = absorb_marked_ ? ms_since(absorb_start_) : -1;   // -1: absorbed while answering
    absorb_marked_ = false;
    int n;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        n = ++g_req;
        g_calls = 0;
        g_req_start = Clock::now();
        g_in_request = true;
    }
    ev_ = {{"ev", "request"}, {"req", n}, {"absorb_ms", absorb}, {"first_sentence_ms", -1}, {"sentences", 0},
           {"tools", json::array()}, {"guards", json::array()}, {"second_look", nullptr}, {"steered", 0}};
    open_ = true;
}

void RequestTrace::sentence(const std::string &text) {
    if (!open_) return;
    ev_["sentences"] = ev_["sentences"].get<int>() + 1;
    if (ev_["first_sentence_ms"].get<long>() >= 0) return;
    ev_["first_sentence_ms"] = request_ms();
    if (!get().tts_probe) return;
    // How long the first sentence takes to synthesise, measured beside the request rather than in
    // it: the result is discarded, nothing is played.
    const int req = ev_["req"].get<int>();
    const long at = ev_["first_sentence_ms"].get<long>();
    ++g_probes;
    std::thread([text, req, at] {
        const auto t = Clock::now();
        std::string pcm, err;
        const bool ok = llm::synthesize(text, pcm, err);
        trace({{"ev", "tts"}, {"req", req}, {"after_ms", at}, {"tts_first_ms", ms_since(t)}, {"ok", ok},
               {"pcm_bytes", static_cast<long>(pcm.size())}});
        --g_probes;
    }).detach();
}

void RequestTrace::tool(const std::string &name, long ms, const std::string &result) {
    if (!open_) return;
    const char *outcome = tools::failed(result) ? "error" : result.compare(0, 7, "CONFIRM") == 0 ? "confirm" : "ok";
    ev_["tools"].push_back({{"name", name}, {"ms", ms}, {"outcome", outcome}, {"result_bytes", static_cast<long>(result.size())},
                            {"at_ms", request_ms()}});
}

void RequestTrace::guard(const char *which) {
    if (open_) ev_["guards"].push_back({{"guard", which}, {"at_ms", request_ms()}});
}

void RequestTrace::second_look(const std::string &outcome) {
    if (open_) ev_["second_look"] = outcome;
}

void RequestTrace::steered() {
    if (open_) ev_["steered"] = ev_["steered"].get<int>() + 1;
}

void RequestTrace::end() {
    if (!open_) return;
    open_ = false;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        ev_["done_ms"] = ms_since(g_req_start);
        ev_["calls"] = g_calls;
        g_in_request = false;
    }
    trace(ev_);
}

}  // namespace experiment

#endif  // ZB_EXPERIMENTS
