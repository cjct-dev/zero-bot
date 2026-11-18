// Measurement switches for the agent experiments (ZB-033): a trace of every model call and request,
// and probes, each behind a ZB_EXP_* environment variable read once at start.
//
// Compiled in only when the build sets ZB_EXPERIMENTS (the cli build does; the device and simulator
// builds do not), so the app users run has none of it: there every function below is an empty
// inline that the compiler removes. With no ZB_EXP_* variable set, the cli build sends and does
// exactly what it would without this file.
//
// The trace holds no words of the user's or the model's, no tool arguments or results, no keys,
// headers or model ids: counts, sizes, times, tool names and which guard fired. So a trace file is
// as safe to keep as the activity log.
//
//   ZB_EXP_TRACE=FILE      append one JSON line per model call ("call"), per request ("request"),
//                          per speech probe ("tts") and one at start ("start") to FILE
//   ZB_EXP_CACHE_BUST=1    a random line at the top of the system prompt, once per process, so the
//                          service cannot answer from a cached prefix of an earlier run
//   ZB_EXP_TTS_PROBE=1     synthesise (never play) each request's first sentence on a thread of its
//                          own and trace how long that took (needs a speech model in settings)
//   ZB_EXP_TOOLS=V         E2, the tool text sent: full (default, as the app has it), say1 (the
//                          confirmation parameter "say" described once, in the system prompt, and in
//                          one short line per tool), lean (say1 plus shorter tool and parameter
//                          descriptions). Names, types, enums and required fields never change.
//   ZB_EXP_DECIDE=V        E3, how a reply drafted without a tool is judged in fast mode:
//                          second_look (default, as the app has it), parallel (a short check call
//                          started beside the deciding call; when it says no tool is needed the draft
//                          is spoken as it streams, otherwise the second look runs as before)
//   ZB_EXP_CHECK_MODEL=ID  the model the parallel check asks (default: the main model)
#pragma once
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "core/util.h"

#ifndef ZB_EXPERIMENTS
#define ZB_EXPERIMENTS 0
#endif

namespace experiment {

struct Switches {
    std::string trace;          // ZB_EXP_TRACE: the file, or "" for no trace
    bool cache_bust = false;    // ZB_EXP_CACHE_BUST
    bool tts_probe = false;     // ZB_EXP_TTS_PROBE
    std::string tools = "full";             // ZB_EXP_TOOLS: full, say1, lean
    std::string decide = "second_look";     // ZB_EXP_DECIDE: second_look, parallel
    std::string check_model;                // ZB_EXP_CHECK_MODEL ("" = the main model)
};

#if ZB_EXPERIMENTS

const Switches &get();
inline bool tracing() { return !get().trace.empty(); }
void trace(json event);                         // appends one line; adds "ev" if missing
std::string cache_bust_line();                  // "" unless ZB_EXP_CACHE_BUST
long request_ms();                              // ms since the current request began (-1 if none)
void trace_call(const json &call);              // one model call of the current request (numbered here)
json numbers_only(const json &v);               // a copy keeping only numbers, booleans and objects of them
void wait_probes(int max_ms);                   // lets speech probes still running finish (command line)

// E2: the tool list as it is about to be sent (tools::schema), in the variant ZB_EXP_TOOLS asks for.
// Only "description" texts change.
void shape_tools(json &schema);
// E2: the system prompt's opening of its rule about "say", in that variant.
std::string say_rule(const std::string &standard);

// E3 (ZB_EXP_DECIDE=parallel): a short check call beside the deciding call, on its own thread, asking
// whether the request needs a tool at all. It never acts: a tool call it makes only counts as "a tool
// is needed". Destroying it cancels the call and waits for its thread.
inline bool parallel_check() { return get().decide == "parallel"; }
class ParallelCheck {
public:
    enum Verdict { kPending, kNone, kTool, kUnclear, kFailed, kCancelled };
    void start(json messages, const json &tools, const std::string &main_model);
    bool active() const { return started_; }
    bool said_none() const { return verdict_ == kNone; }
    bool answered() const { return verdict_ != kPending; }
    bool wait_none(int max_ms);                 // waits for the verdict up to max_ms; true when it is "none"
    void used(const char *how) { used_ = how; } // what the loop did with it, for the trace
    void stop();                                // cancels the call if it is still running, and waits
    json summary() const;                       // for the request's trace line
    ~ParallelCheck();
private:
    std::thread thread_;
    std::atomic<int> verdict_{kPending};
    std::atomic<long> ms_{-1};
    std::atomic<bool> cancel_{false};
    std::chrono::steady_clock::time_point start_;
    bool started_ = false;
    std::string used_ = "unused";
};

// One request, from the input absorbed to its last word: built up in Agent::run_turn.
class RequestTrace {
public:
    void absorbing();                           // input is about to be absorbed (transcribed if spoken)
    void begin();                               // the request starts: input absorbed (ends one still open)
    void sentence(const std::string &text);    // a sentence handed to speech (spoken or not)
    void tool(const std::string &name, long ms, const std::string &result);
    void guard(const char *which);
    void second_look(const std::string &outcome);
    void parallel_check(const json &summary);
    void steered();
    void end();
    ~RequestTrace() { end(); }
private:
    bool open_ = false, absorb_marked_ = false;
    std::chrono::steady_clock::time_point absorb_start_;
    json ev_;
};

#else

inline const Switches &get() { static const Switches s; return s; }
inline bool tracing() { return false; }
inline void trace(const json &) {}
inline std::string cache_bust_line() { return ""; }
inline long request_ms() { return -1; }
inline void trace_call(const json &) {}
inline json numbers_only(const json &) { return nullptr; }
inline void wait_probes(int) {}
inline void shape_tools(json &) {}
inline std::string say_rule(const std::string &standard) { return standard; }
inline bool parallel_check() { return false; }

class ParallelCheck {
public:
    void start(const json &, const json &, const std::string &) {}
    bool active() const { return false; }
    bool said_none() const { return false; }
    bool answered() const { return false; }
    bool wait_none(int) { return false; }
    void used(const char *) {}
    void stop() {}
    json summary() const { return nullptr; }
};

class RequestTrace {
public:
    void absorbing() {}
    void begin() {}
    void sentence(const std::string &) {}
    void tool(const std::string &, long, const std::string &) {}
    void guard(const char *) {}
    void second_look(const std::string &) {}
    void parallel_check(const json &) {}
    void steered() {}
    void end() {}
};

#endif

}  // namespace experiment
