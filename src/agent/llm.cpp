#include "agent/llm.h"

#include <cstring>

#include <algorithm>
#include <chrono>
#include <unistd.h>

#include "agent/experiment.h"
#include "agent/llm_stream.h"
#include "core/settings.h"
#include "net/http.h"
#include "platform/audio.h"
#include "platform/codec.h"

namespace llm {

Service Service::of(const Settings &s) { return {s.eff_api_base(), s.eff_api_key()}; }

namespace {

// A request to `path` of `to`, with its key: both from the one snapshot (Service).
void address(http::Request &req, const Service &to, const char *path, bool json_body) {
    req.url = to.base + path;
    req.headers = {"Authorization: Bearer " + to.key};
    if (json_body) req.headers.push_back("Content-Type: application/json");
}

std::string api_error(const http::Response &res) {
    if (!res.error.empty()) return res.error;
    const json j = util::parse_untrusted_json(res.body);
    if (j.is_object() && j.contains("error")) {
        const json &e = j["error"];
        if (e.is_string()) return e.get<std::string>();
        if (e.is_object()) return util::jget(e, "message", std::string("service error"));
    }
    // No structured error: say what the status means rather than showing a web page's markup.
    const std::string code = "HTTP " + std::to_string(res.status);
    if (res.status == 401 || res.status == 403) return "The service refused the API key (" + code + ").";
    if (res.status == 404) return "Nothing answers at that address (" + code + "). Check it ends in /v1.";
    if (res.status == 429) return "The service is busy or over its limit (" + code + "). Try again shortly.";
    if (res.status >= 500) {
        // Services behind a proxy report their own outage with a short code in the page.
        std::string detail = code;
        const size_t at = res.body.find("error code: ");
        if (at != std::string::npos) {
            const std::string digits = res.body.substr(at + 12, 4);
            if (digits.find_first_not_of("0123456789") == std::string::npos) detail += ", code " + digits;
        }
        return "The AI service is down or unreachable (" + detail + "). This is on the service's side; try again later.";
    }
    const size_t first = res.body.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && res.body[first] == '<') return "The service sent an unexpected reply (" + code + ").";
    return res.describe();
}

std::string dump(const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

// An error the service sent inside a stream that says it is busy for the moment, rather than
// that the request was wrong.
bool busy_message(const std::string &error) {
    const std::string e = util::lower(error);
    for (const char *w : {"overload", "rate limit", "try again", "temporarily", "unavailable", "capacity", "busy"})
        if (e.find(w) != std::string::npos) return true;
    return false;
}

// Reading from the service is safe to repeat: a request that failed on the way (no connection, a
// dropped one, an overloaded or failing server) is sent again after a short pause, within the
// request's own time limit. `attempts` counts the first send.
http::Retry service_retry(int attempts, long budget_s) {
    http::Retry r;
    r.attempts = attempts;
    r.budget_ms = budget_s * 1000;
    r.min_ms = std::min<long>(15000, r.budget_ms / 4);
    r.pause_ms = 400;
    return r;
}

// Re-encodes a 16-bit mono WAV (as produced by the recorder) to MP3. False if that is not possible.
bool wav_to_mp3(const std::string &wav, std::string &mp3) {
    std::vector<int16_t> pcm;
    int rate = 0;
    if (!codec::can_encode_mp3() || !audio::pcm_from_wav(wav, pcm, rate)) return false;
    return codec::mp3_encode(pcm, rate, 32, mp3);
}

// What the experiments' trace records of one chat call (ZB_EXP_TRACE), gathered as it runs: times in
// ms (-1 until it happens), the sends it took and the token counts the service reported.
struct CallTrace {
    long t_send = -1;                                   // since the request began, when the call was sent
    long ttfb = -1, first_text = -1, first_call = -1;   // since it was sent: the first byte, text, tool call
    int attempts = 1;
    json usage;
};

// The call's line in the trace. Counts, sizes and times only: never the words sent or received, a
// key or the model's id.
void trace_record(const ChatOptions &opt, const json &tools, const json &body, size_t body_bytes, const CallTrace &t, long done_ms,
                  const http::Response &res, const ChatResult &out) {
    json outcome;
    if (res.error == "cancelled") outcome = "cancelled";
    else if (!res.ok() || !out.error.empty()) outcome = "error";
    else if (!out.tool_calls.empty()) {
        outcome = json::array();
        for (const auto &c : out.tool_calls)
            if (!c.name.empty()) outcome.push_back(c.name);
    } else outcome = out.content.empty() ? "empty" : "text";
    experiment::trace_call({{"role", opt.role.empty() ? "main" : opt.role}, {"purpose", opt.purpose},
                            {"tools", tools.is_array() ? static_cast<long>(tools.size()) : 0L},
                            {"tool_choice", body.contains("tool_choice") ? body["tool_choice"] : json(nullptr)},
                            {"fast", body.contains("reasoning_effort")}, {"usage_asked", body.contains("stream_options")},
                            {"body_bytes", static_cast<long>(body_bytes)}, {"t_send", t.t_send}, {"ttfb_ms", t.ttfb},
                            {"first_text_ms", t.first_text}, {"first_call_ms", t.first_call}, {"done_ms", done_ms},
                            {"attempts", t.attempts}, {"status", res.status}, {"usage", t.usage}, {"outcome", outcome},
                            {"text_chars", static_cast<long>(out.content.size())}});
}

// Fields a service has turned down: not sent again while the program runs (ChatCall).
std::atomic<bool> g_effort_rejected{false}, g_usage_rejected{false};

// One streamed chat call (chat): the body sent, the reply read as it streams in (answer text clear of
// thinking markers, tool calls pieced together), the sends it takes, and what the experiments' trace
// records of it.
class ChatCall {
public:
    ChatCall(const json &messages, const json &tools, const ChatOptions &opt);
    ChatCall(const ChatCall &) = delete;   // its callbacks hold `this`
    ChatResult run();

private:
    void make_request();
    http::Response send();
    bool turned_down(const http::Response &res) const;
    http::Response resend(http::Retry &retry);
    void on_line(const std::string &line);
    void on_delta(const json &delta);
    void emit(const std::string &text);
    void reset();
    ChatResult finish(const http::Response &res);
    long since() const;

    const json &tools_;
    const ChatOptions &opt_;
    const bool tracing_ = experiment::tracing();
    json body_;
    bool sent_effort_ = false, sent_usage_ = false;
    std::chrono::steady_clock::time_point started_;
    CallTrace ct_;
    ChatResult out_;
    ThinkFilter think_;
    LineSplitter lines_;   // the event stream's lines (llm_stream.h)
    http::Request req_;
    const std::function<void(const std::string &)> on_line_ = [this](const std::string &line) { on_line(line); };
    const std::function<void(const std::string &)> emit_ = [this](const std::string &text) { emit(text); };
};

ChatCall::ChatCall(const json &messages, const json &tools, const ChatOptions &opt) : tools_(tools), opt_(opt) {
    body_ = {{"model", opt.model}, {"messages", messages}, {"stream", true},
             {"max_tokens", opt.max_tokens}, {"temperature", opt.temperature}};
    if (tools.is_array() && !tools.empty()) {
        body_["tools"] = tools;
        body_["tool_choice"] = "auto";
    }
    // Reasoning models think privately before answering, which costs a second or more per
    // call. Fast mode asks them not to. Servers that reject the field get one retry without it.
    sent_effort_ = opt.fast && !g_effort_rejected;
    if (sent_effort_) body_["reasoning_effort"] = "none";

    // Experiments only (ZB_EXP_TRACE): the call's timings and the token counts the service reports,
    // which it sends only when asked. Never asked for otherwise; dropped for good after a 400.
    sent_usage_ = tracing_ && !g_usage_rejected;
    if (sent_usage_) body_["stream_options"] = {{"include_usage", true}};
    started_ = std::chrono::steady_clock::now();
    ct_.t_send = tracing_ ? experiment::request_ms() : -1;
}

// The call: sent (again without a field the service turned down), the stream read to its end, traced.
ChatResult ChatCall::run() {
    make_request();
    const http::Response res = send();
    lines_.finish(on_line_);
    think_.feed("", true, emit_);
    if (tracing_) trace_record(opt_, tools_, body_, req_.body.size(), ct_, since(), res, out_);
    return finish(res);
}

void ChatCall::make_request() {
    req_.method = "POST";
    address(req_, Service::of(Settings::get()), "/chat/completions", true);
    req_.headers.push_back("Accept: text/event-stream");
    req_.body = dump(body_);
    req_.timeout_s = 180;
    req_.cancel = opt_.cancel;
    req_.on_data = [this](const char *data, size_t n) {
        if (tracing_ && ct_.ttfb < 0) ct_.ttfb = since();
        lines_.feed(data, n, on_line_);
    };
}

// Upstream hiccups (5xx, dropped connections, a service that says it is overloaded) are common
// on mobile links. As long as nothing has been passed on yet, the call is tried again, twice at
// most and all within its own three minutes: a timeout once the request had gone out is not
// repeated, since that would double the wait.
http::Response ChatCall::send() {
    http::Retry retry = service_retry(3, req_.timeout_s);
    retry.again = [this](const http::Response &r) {
        if (!out_.content.empty() || !out_.tool_calls.empty()) return false;
        return r.ok() ? !out_.error.empty() && busy_message(out_.error) : http::transient(r);
    };
    retry.before_resend = [this] { reset(); ++ct_.attempts; };
    http::Response res = http::send_retrying(req_, retry);
    if (sent_usage_ && turned_down(res)) {
        // Experiments only: a service that turns down the request for token counts gets the call
        // without it. If that is refused too, the counts were not the reason; ask for them again.
        // Only an answer without them proves they were (a failure on the way proves nothing).
        body_.erase("stream_options");
        res = resend(retry);
        if (res.ok()) g_usage_rejected = true;
        else if (res.status == 400) body_["stream_options"] = {{"include_usage", true}};
    }
    if (sent_effort_ && turned_down(res)) {
        g_effort_rejected = true;
        body_.erase("reasoning_effort");
        res = resend(retry);
    }
    return res;
}

// Refused as a bad request before anything came back: perhaps for a field this service does not take.
bool ChatCall::turned_down(const http::Response &res) const {
    return res.status == 400 && out_.content.empty() && out_.tool_calls.empty();
}

// The call sent again with the body as it now is, within what is left of its own time limit.
http::Response ChatCall::resend(http::Retry &retry) {
    req_.body = dump(body_);
    reset();
    ++ct_.attempts;
    const long spent_s = static_cast<long>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_).count());
    retry.budget_ms = std::max(1L, req_.timeout_s - spent_s) * 1000;
    return http::send_retrying(req_, retry);
}

// One line of the event stream: an error, the token counts, or a piece of the reply.
void ChatCall::on_line(const std::string &line) {
    if (line.compare(0, 5, "data:") != 0) return;
    const std::string payload = util::trim(line.substr(5));
    if (payload.empty() || payload == "[DONE]") return;
    const json ev = util::parse_untrusted_json(payload);
    if (!ev.is_object()) return;
    if (ev.contains("error")) {
        const json &e = ev["error"];
        out_.error = e.is_string() ? e.get<std::string>() : util::jget(e, "message", std::string("service error"));
        return;
    }
    // The token counts come in a last event of their own, with an empty list of choices.
    if (tracing_ && ev.contains("usage") && ev["usage"].is_object()) ct_.usage = experiment::numbers_only(ev["usage"]);
    if (!ev.contains("choices") || !ev["choices"].is_array() || ev["choices"].empty()) return;
    const json &choice = ev["choices"][0];
    if (!choice.contains("delta") || !choice["delta"].is_object()) return;
    on_delta(choice["delta"]);
}

// A piece of the reply: answer text, or pieces of tool calls.
void ChatCall::on_delta(const json &delta) {
    if (delta.contains("content") && delta["content"].is_string()) {
        const std::string piece = delta["content"].get<std::string>();
        if (!piece.empty()) {
            if (tracing_ && ct_.first_text < 0) ct_.first_text = since();
            think_.feed(piece, false, emit_);
        }
    }
    if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
        if (tracing_ && ct_.first_call < 0) ct_.first_call = since();
        // Each piece to the call its index names (llm_stream.h); an index out of range is a fault in the
        // stream, reported as an error event's message is.
        for (const json &tc : delta["tool_calls"]) {
            std::string bad;
            if (!add_tool_delta(out_.tool_calls, tc, bad)) {
                if (out_.error.empty()) out_.error = bad;
                return;
            }
        }
    }
}

// Answer text clear of thinking markers (llm_stream.h), passed on as it comes.
void ChatCall::emit(const std::string &text) {
    if (text.empty()) return;
    out_.content += text;
    if (opt_.on_text) opt_.on_text(text);
}

// Before the call is sent again: nothing of the last attempt's reply is kept.
void ChatCall::reset() {
    lines_.reset();
    think_.reset();
    out_ = ChatResult();
    ct_.ttfb = ct_.first_text = ct_.first_call = -1;
    ct_.usage = nullptr;
}

// The outcome: cancelled, failed (in the service's own words where it gave any), or the reply.
ChatResult ChatCall::finish(const http::Response &res) {
    if (res.error == "cancelled") { out_.cancelled = true; return std::move(out_); }
    if (!res.ok() && out_.error.empty()) out_.error = api_error(res);
    // Drop empty slots and give every call an id (some servers omit them).
    std::vector<ToolCall> calls;
    for (auto &c : out_.tool_calls) {
        if (c.name.empty()) continue;
        if (c.id.empty()) c.id = "call_" + std::to_string(calls.size());
        if (util::trim(c.args).empty()) c.args = "{}";
        calls.push_back(c);
    }
    out_.tool_calls.swap(calls);
    return std::move(out_);
}

long ChatCall::since() const {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_).count());
}

}  // namespace

ChatResult chat(const json &messages, const json &tools, const ChatOptions &opt) {
    if (!Settings::get().configured()) {
        ChatResult out;
        out.error = "No AI service is configured. Open settings to add one.";
        return out;
    }
    return ChatCall(messages, tools, opt).run();
}

std::string transcribe(const std::string &wav, std::string &err, std::atomic<bool> *cancel) {
    const Settings &s = Settings::get();
    if (!s.configured()) { err = "No AI service is configured."; return ""; }
    if (s.stt_model.empty()) { err = "No speech-to-text model is selected in settings."; return ""; }
    http::Request req;
    req.method = "POST";
    address(req, Service::of(s), "/audio/transcriptions", false);
    // Speech is sent as MP3 when it can be: about a tenth of the size of WAV, which matters far
    // more than the encoding time on a slow or tethered connection. A service that turns MP3
    // down gets the original WAV instead, from then on.
    static std::atomic<bool> mp3_rejected{false};
    std::string mp3;
    const bool use_mp3 = s.compress_audio && !mp3_rejected && wav_to_mp3(wav, mp3);
    auto build = [&](bool compressed) {
        req.multipart = {{"model", "", "", s.stt_model},
                         {"response_format", "", "", "json"},
                         compressed ? http::Part{"file", "speech.mp3", "audio/mpeg", mp3}
                                    : http::Part{"file", "speech.wav", "audio/wav", wav}};
    };
    build(use_mp3);
    req.timeout_s = 60;
    req.cancel = cancel;
    // What the user said is lost if this fails, and transcribing it twice changes nothing.
    http::Response res = http::send_retrying(req, service_retry(3, req.timeout_s));
    if (use_mp3 && (res.status == 400 || res.status == 415 || res.status == 422)) {
        mp3_rejected = true;
        build(false);
        res = http::send_retrying(req, service_retry(2, req.timeout_s));
    }
    if (!res.ok()) { err = api_error(res); return ""; }
    const json j = util::parse_untrusted_json(res.body);
    if (!j.is_object()) { err = "unexpected reply from the speech service"; return ""; }
    return util::trim(util::jget(j, "text", std::string()));
}

bool synthesize(const std::string &text, std::string &pcm, std::string &err, std::atomic<bool> *cancel) {
    const Settings &s = Settings::get();
    if (!s.configured()) { err = "No AI service is configured."; return false; }
    if (s.tts_model.empty()) { err = "No text-to-speech model is selected in settings."; return false; }
    // Opus is several times smaller than raw PCM for the same speech. It is used whenever the
    // decoder is present and the service has not refused it.
    static std::atomic<bool> opus_rejected{false};
    const bool want_opus = s.compress_audio && !opus_rejected && codec::can_decode_opus();
    auto fetch = [&](const char *format) {
        http::Request req;
        req.method = "POST";
        address(req, Service::of(s), "/audio/speech", true);
        req.body = dump({{"model", s.tts_model}, {"input", text}, {"voice", s.tts_voice.empty() ? "alloy" : s.tts_voice},
                         {"response_format", format}});
        req.timeout_s = 60;
        req.cancel = cancel;
        return http::send_retrying(req, service_retry(2, req.timeout_s));
    };
    if (want_opus) {
        const http::Response res = fetch("opus");
        if (res.error == "cancelled") { err = res.error; return false; }
        std::vector<int16_t> decoded;
        if (res.ok() && codec::opus_decode(res.body, decoded)) {
            // The decoder always produces 48 kHz; halve it to the playback rate by averaging pairs.
            pcm.clear();
            pcm.reserve(decoded.size());
            for (size_t i = 0; i + 1 < decoded.size(); i += 2) {
                const int16_t v = static_cast<int16_t>((static_cast<int>(decoded[i]) + decoded[i + 1]) / 2);
                pcm.append(reinterpret_cast<const char *>(&v), 2);
            }
            return true;
        }
        if (res.ok() || res.status == 400 || res.status == 415 || res.status == 422) opus_rejected = true;
        else { err = api_error(res); return false; }
    }
    const http::Response res = fetch("pcm");
    if (!res.ok()) { err = api_error(res); return false; }
    pcm = res.body;
    // Some servers answer with a WAV container even when PCM was requested.
    if (pcm.size() > 44 && pcm.compare(0, 4, "RIFF") == 0) {
        const size_t at = audio::wav_samples_at(pcm);
        pcm.erase(0, at == std::string::npos ? 44 : at);
    }
    return true;
}

std::string describe_image(const std::string &jpeg, const std::string &question, std::string &err,
                           std::atomic<bool> *cancel) {
    const json messages = json::array({
        {{"role", "system"},
         {"content", "You are the eyes of a pocket voice assistant. Answer the question about the photo accurately "
                     "and briefly, in plain text. Transcribe any visible text exactly when it matters. If the photo "
                     "is too dark, blurred or does not show what is asked, say so."}},
        {{"role", "user"},
         {"content", json::array({{{"type", "text"}, {"text", question}},
                                  {{"type", "image_url"},
                                   {"image_url", {{"url", "data:image/jpeg;base64," + util::base64(jpeg)}}}}})}},
    });
    ChatOptions opt;
    opt.model = Settings::get().eff_vision_model();
    opt.max_tokens = 900;
    opt.temperature = 0.2;
    opt.fast = Settings::get().fast_mode;  // reading a sign or a menu needs eyes, not deliberation
    opt.cancel = cancel;
    opt.role = "vision";
    opt.purpose = "look";
    const ChatResult res = chat(messages, json::array(), opt);
    if (!res.error.empty()) { err = res.error; return ""; }
    if (res.cancelled) { err = "cancelled"; return ""; }
    return util::trim(res.content);
}

std::vector<ModelInfo> list_models(const Service &service, std::string &err) {
    std::vector<ModelInfo> out;
    if (service.base.empty() || service.key.empty()) { err = "No AI service is configured."; return out; }
    http::Request req;
    address(req, service, "/models", false);
    req.timeout_s = 20;
    const http::Response res = http::send(req);
    if (!res.ok()) { err = api_error(res); return out; }
    const json j = util::parse_untrusted_json(res.body);
    const json &data = j.is_object() && j.contains("data") ? j["data"] : j;
    if (!data.is_array()) { err = "unexpected model list"; return out; }
    for (const json &m : data) {
        ModelInfo info;
        info.id = util::jget(m, "id", std::string());
        info.mode = util::jget(m, "mode", std::string());
        if (!info.id.empty()) out.push_back(info);
    }
    return out;
}

}  // namespace llm
