// See experiment.h. Nothing here is compiled unless the build sets ZB_EXPERIMENTS.
#include "agent/experiment.h"

#if ZB_EXPERIMENTS

#include <arpa/inet.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <thread>

#include "agent/controls.h"
#include "agent/llm.h"
#include "agent/tools.h"

namespace experiment {

namespace {

using Clock = std::chrono::steady_clock;

std::mutex g_mtx;                          // the trace and scan files, the current request
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

// ZB_EXP_WEB's value as the address requests are sent to, or "" when it is not one: "http://", an IPv4
// address in plain decimal (four parts, each 0 to 255 with no leading zero, the first 127), then nothing but
// an optional ":port" (1 to 65535) and a "/path". The address sent is rebuilt from the numbers read, so no
// other reading of the spelling ("0177.0.0.1" as octal, "127.1" as short form) can choose where it goes.
std::string stand_in_base(const std::string &v) {
    if (v.compare(0, 7, "http://") != 0 || v.find_first_of("@?#\\ \t\r\n") != std::string::npos) return "";
    size_t i = 7;
    // Up to max_len digits from i, as a number; -1 when there are none, too many, or a leading zero.
    auto number = [&](size_t max_len) {
        const size_t start = i;
        while (i < v.size() && isdigit(static_cast<unsigned char>(v[i])) && i - start <= max_len) ++i;
        const size_t len = i - start;
        if (len == 0 || len > max_len || (len > 1 && v[start] == '0')) return -1L;
        return std::stol(v.substr(start, len));
    };
    std::string out = "http://";
    for (int k = 0; k < 4; ++k) {
        if (k > 0) {
            if (i >= v.size() || v[i] != '.') return "";
            ++i;
        }
        const long octet = number(3);
        if (octet < 0 || octet > 255 || (k == 0 && octet != 127)) return "";
        out += (k > 0 ? "." : "") + std::to_string(octet);
    }
    if (i < v.size() && v[i] == ':') {
        ++i;
        const long port = number(5);
        if (port < 1 || port > 65535) return "";
        out += ":" + std::to_string(port);
    }
    if (i < v.size() && v[i] != '/') return "";   // after the address only a port and a path
    out += v.substr(i);
    while (out.size() > 7 && out.back() == '/') out.pop_back();
    return out;
}

// Variants dropped in November 2026 (ZB-033, ZB-035; the owner chose the default). A run that still sets
// one would measure the default under the variant's name, so it stops instead, as a ZB_EXP_OFF name this
// build does not know stops it: read as the program starts, whatever its mode, before anything is sent.
void refuse_retired() {
    for (const char *name : {"ZB_EXP_DECIDE", "ZB_EXP_TOOLS", "ZB_EXP_CHECK_MODEL"})
        if (const char *v = getenv(name); v && *v) {
            fprintf(stderr, "%s=%s: that variant was retired, and this build measures only the default; unset %s\n", name, v, name);
            exit(2);
        }
}
[[maybe_unused]] const bool g_retired_refused = (refuse_retired(), true);

}  // namespace

const Switches &get() {
    static const Switches s = [] {
        Switches r;
        if (const char *t = getenv("ZB_EXP_TRACE")) r.trace = t;
        r.cache_bust = env_on("ZB_EXP_CACHE_BUST");
        r.tts_probe = env_on("ZB_EXP_TTS_PROBE");
        if (const char *w = getenv("ZB_EXP_WEB"); w && *w) {
            // Only a stand-in on this computer: the switch must never send a search or a key elsewhere.
            r.web = stand_in_base(w);
            if (r.web.empty()) {
                fprintf(stderr, "ZB_EXP_WEB=%s is not an http://127.x address\n", w);
                exit(2);
            }
        }
        r.no_hardware = env_on("ZB_EXP_NO_HARDWARE");
        if (const char *n = getenv("ZB_EXP_NET"); n && *n) {
            r.net = n;
            const std::string v = n;
            if (v != "loopback" && !((v.compare(0, 8, "https://") == 0 || v.compare(0, 7, "http://") == 0) &&
                                     v.find_first_of("/?#@\\ ", v.find("://") + 3) == std::string::npos && v.size() > v.find("://") + 3)) {
                fprintf(stderr, "ZB_EXP_NET=%s is neither loopback nor one origin (scheme://host[:port])\n", n);
                exit(2);
            }
        }
        if (const char *s = getenv("ZB_EXP_SCAN")) r.scan = s;
        return r;
    }();
    return s;
}

std::string web(const std::string &url) {
    const std::string &base = get().web;
    const size_t scheme = url.find("://");
    if (base.empty() || scheme == std::string::npos) return url;
    return base + "/" + url.substr(scheme + 3);
}

bool stand_in(const std::string &url) {
    const std::string &base = get().web;
    return !base.empty() && url.size() > base.size() && url.compare(0, base.size(), base) == 0 && url[base.size()] == '/';
}

namespace {
// scheme, lower-case host and port of a URL, as libcurl itself reads it (the scheme's own port when none is written;
// a written port, 0 included, as written); "" host when it cannot be read or carries credentials.
struct Origin { std::string scheme, host; long port = -1; };
Origin origin_of(const std::string &url) {
    Origin o;
    CURLU *u = curl_url();
    if (!u) return o;
    char *scheme = nullptr, *host = nullptr, *port = nullptr, *user = nullptr;
    if (curl_url_set(u, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK && curl_url_get(u, CURLUPART_USER, &user, 0) != CURLUE_OK &&
        curl_url_get(u, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK && curl_url_get(u, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
        curl_url_get(u, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT) == CURLUE_OK) {
        const std::string p = port;
        if (!p.empty() && p.size() <= 5 && std::all_of(p.begin(), p.end(), ::isdigit)) {
            o.scheme = util::lower(scheme);
            o.host = util::lower(host);
            o.port = std::stol(p);
        }
    }
    curl_free(scheme);
    curl_free(host);
    curl_free(port);
    curl_free(user);
    curl_url_cleanup(u);
    return o;
}
// A numeric loopback address, checked as an address: IPv4 in 127.0.0.0/8, or ::1. No name (not even localhost).
bool loopback(const std::string &host) {
    in_addr v4;
    if (inet_pton(AF_INET, host.c_str(), &v4) == 1) return (ntohl(v4.s_addr) >> 24) == 127;
    in6_addr v6;
    const std::string bare = host.size() > 2 && host.front() == '[' && host.back() == ']' ? host.substr(1, host.size() - 2) : host;
    return inet_pton(AF_INET6, bare.c_str(), &v6) == 1 && IN6_IS_ADDR_LOOPBACK(&v6);
}
}  // namespace

std::string net_refusal(const std::string &url) {
    const std::string &net = get().net;
    if (net.empty()) return "";
    const Origin o = origin_of(url);
    if (!o.host.empty() && (o.scheme == "http" || o.scheme == "https") && loopback(o.host)) return "";
    if (net != "loopback") {
        const Origin allowed = origin_of(net);
        if (!o.host.empty() && o.scheme == allowed.scheme && o.host == allowed.host && o.port == allowed.port) return "";
    }
    return "refused: a test run sends nothing but to 127.x" + std::string(net == "loopback" ? "" : " and the AI service") +
           " (ZB_EXP_NET), not to " + (o.host.empty() ? std::string("that address") : o.scheme + "://" + o.host + ":" + std::to_string(o.port));
}

std::string no_lan() {
    return get().net.empty() ? "" : "The real network is not looked at in a test run (ZB_EXP_NET).";
}

bool stand_in_scan() {
    const std::string &file = get().scan;
    if (file.empty()) return false;
    std::lock_guard<std::mutex> lk(g_mtx);
    if (FILE *f = fopen(file.c_str(), "a")) {
        fputs("search\n", f);
        fclose(f);
    }
    return true;
}

std::string no_camera() {
    return get().no_hardware ? "the camera is switched off for this test run (ZB_EXP_NO_HARDWARE): set ZB_FAKE_CAMERA to a picture"
                             : "";
}

std::string no_infrared() {
    return get().no_hardware ? "the infrared transmitter is switched off for this test run (ZB_EXP_NO_HARDWARE)" : "";
}

void trace(json event) {
    if (!tracing()) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_started) {
        g_started = true;
        write_line({{"ev", "start"}, {"version", ZB_VERSION}, {"pid", static_cast<long>(getpid())},
                    {"cache_bust", get().cache_bust}, {"tts_probe", get().tts_probe}, {"off", controls::off_names()}});
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
