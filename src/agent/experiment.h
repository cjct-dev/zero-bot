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
#pragma once
#include <chrono>
#include <string>

#include "core/util.h"

#ifndef ZB_EXPERIMENTS
#define ZB_EXPERIMENTS 0
#endif

namespace experiment {

struct Switches {
    std::string trace;          // ZB_EXP_TRACE: the file, or "" for no trace
    bool cache_bust = false;    // ZB_EXP_CACHE_BUST
    bool tts_probe = false;     // ZB_EXP_TTS_PROBE
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

// One request, from the input absorbed to its last word: built up in Agent::run_turn.
class RequestTrace {
public:
    void absorbing();                           // input is about to be absorbed (transcribed if spoken)
    void begin();                               // the request starts: input absorbed (ends one still open)
    void sentence(const std::string &text);    // a sentence handed to speech (spoken or not)
    void tool(const std::string &name, long ms, const std::string &result);
    void guard(const char *which);
    void second_look(const std::string &outcome);
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

class RequestTrace {
public:
    void absorbing() {}
    void begin() {}
    void sentence(const std::string &) {}
    void tool(const std::string &, long, const std::string &) {}
    void guard(const char *) {}
    void second_look(const std::string &) {}
    void steered() {}
    void end() {}
};

#endif

}  // namespace experiment
