// Measurement switches for the agent experiments (ZB-033): a trace of every model call and request,
// and probes, each behind a ZB_EXP_* environment variable read once at start.
//
// Compiled in only when the build sets ZB_EXPERIMENTS (the cli build does; the device and simulator
// builds do not), so the app users run has none of it: there every function below is an empty
// inline that the compiler removes. With no ZB_EXP_* variable set, the cli build sends and does
// exactly what it would without this file.
//
// The trace holds no words of the user's or the model's, no tool arguments or results, no keys,
// headers or model ids: counts, sizes, times, tool names, search services' kinds and which guard
// fired. So a trace file is as safe to keep as the activity log.
//
//   ZB_EXP_TRACE=FILE      append one JSON line per model call ("call"), per request ("request"),
//                          per speech probe ("tts"), per service a web search asks and per answer
//                          from its cache ("search") and one at start ("start") to FILE
//   ZB_EXP_CACHE_BUST=1    a random line at the top of the system prompt, once per process, so the
//                          service cannot answer from a cached prefix of an earlier run
//   ZB_EXP_TTS_PROBE=1     synthesise (never play) each request's first sentence on a thread of its
//                          own and trace how long that took (needs a speech model in settings)
//   ZB_EXP_WEB=BASE        every web read a tool makes (search, forecasts, place names, exchange rates,
//                          stock prices) goes to a stand-in at BASE
//                          (tests/mocks/fake_web.py) instead of the internet: https://HOST/PATH?Q is
//                          sent as BASE/HOST/PATH?Q. BASE must be http://127.x...; anything else stops
//                          the run. For the test sets: canned results, and search without a key. A
//                          request to the stand-in is `local` (http.h): no proxy, no redirect followed
//   ZB_EXP_NET=V           the test run's network: HTTP goes only to 127.x and localhost ("loopback") or to those
//                          and exactly one origin ("https://host[:port]", the AI service), never following a
//                          redirect; anything else is refused before it is sent. The real network is never
//                          identified either (no look at the router, no packet to it). Any other value stops
//                          the run. Set by every test runner (tests/netguard.py)
//   ZB_EXP_NO_HARDWARE=1   the camera (unless ZB_FAKE_CAMERA names a picture) and the infrared
//                          transmitter refuse with an error instead of being used: the live test sets'
//                          runners set it, so a test run never sends a real photo to the service or
//                          an infrared code into the room, whatever the model does
//   ZB_EXP_SCAN=FILE       on the stand-in test network, where nothing is ever scanned, the short search for a
//                          device that may have moved (control.cpp relocate) is made all the same: it hears
//                          nothing, and appends a line to FILE. For the end-to-end tests: whether a command
//                          looked for a device, and what its error then says
//   ZB_EXP_DECIDE, ZB_EXP_TOOLS, ZB_EXP_CHECK_MODEL   variants retired in November 2026: set to anything,
//                          they stop the run at start, so that an old command does not measure the
//                          default under their name
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
    std::string web;                        // ZB_EXP_WEB ("" = the real services)
    bool no_hardware = false;               // ZB_EXP_NO_HARDWARE
    std::string net;                        // ZB_EXP_NET: "loopback" or the one origin allowed besides ("" = unrestricted)
    std::string scan;                       // ZB_EXP_SCAN: the file each stand-in search is noted in ("" = none)
};

#if ZB_EXPERIMENTS

const Switches &get();
inline bool tracing() { return !get().trace.empty(); }
void trace(json event);                         // appends one line; adds "ev" if missing
std::string cache_bust_line();                  // "" unless ZB_EXP_CACHE_BUST
long request_ms();                              // ms since the current request began (-1 if none)
void trace_call(const json &call);              // one model call of the current request (numbered here)
void trace_search(const json &search);          // one service a web search asked, or its cache's answer (search.cpp)
json numbers_only(const json &v);               // a copy keeping only numbers, booleans and objects of them
void wait_probes(int max_ms);                   // lets speech probes still running finish (command line)
std::string web(const std::string &url);        // ZB_EXP_WEB: the stand-in's address for url, or url
bool stand_in(const std::string &url);          // url goes to the ZB_EXP_WEB stand-in (sent as `local`)
std::string no_camera();                        // ZB_EXP_NO_HARDWARE: the camera's refusal, or ""
std::string no_infrared();                      // ZB_EXP_NO_HARDWARE: the transmitter's refusal, or ""
std::string net_refusal(const std::string &url);  // ZB_EXP_NET: why a request to url may not be sent, or ""
inline bool net_pinned() { return !get().net.empty(); }   // ZB_EXP_NET: no redirect is followed
std::string no_lan();                           // ZB_EXP_NET: why the real network is not identified, or ""
bool stand_in_scan();                           // ZB_EXP_SCAN: a search on the test network, noted; false when unset

// One request, from the input absorbed to its last word: built up in Agent::Turn.
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
inline void trace_search(const json &) {}
inline json numbers_only(const json &) { return nullptr; }
inline void wait_probes(int) {}
inline std::string web(std::string url) { return url; }
inline bool stand_in(const std::string &) { return false; }
inline std::string no_camera() { return std::string(); }
inline std::string no_infrared() { return std::string(); }
inline std::string net_refusal(const std::string &) { return std::string(); }
inline bool net_pinned() { return false; }
inline std::string no_lan() { return std::string(); }
inline bool stand_in_scan() { return false; }
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
