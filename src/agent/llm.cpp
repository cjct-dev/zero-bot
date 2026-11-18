#include "agent/llm.h"

#include <cstring>

#include <algorithm>
#include <chrono>
#include <unistd.h>

#include "agent/experiment.h"
#include "core/settings.h"
#include "net/http.h"
#include "platform/codec.h"

namespace llm {

namespace {

std::vector<std::string> auth_headers(bool json_body) {
    std::vector<std::string> h = {"Authorization: Bearer " + Settings::get().eff_api_key()};
    if (json_body) h.push_back("Content-Type: application/json");
    return h;
}

std::string endpoint(const char *path) { return Settings::get().eff_api_base() + path; }

std::string api_error(const http::Response &res) {
    if (!res.error.empty()) return res.error;
    const json j = json::parse(res.body, nullptr, false);
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
    if (wav.size() < 46 || wav.compare(0, 4, "RIFF") != 0 || !codec::can_encode_mp3()) return false;
    uint32_t rate = 0;
    memcpy(&rate, wav.data() + 24, 4);
    const size_t data = wav.find("data", 12);
    if (data == std::string::npos || data + 8 >= wav.size() || rate < 8000) return false;
    const size_t count = (wav.size() - data - 8) / 2;
    std::vector<int16_t> pcm(count);
    memcpy(pcm.data(), wav.data() + data + 8, count * 2);
    return codec::mp3_encode(pcm, static_cast<int>(rate), 32, mp3);
}

}  // namespace

ChatResult chat(const json &messages, const json &tools, const ChatOptions &opt) {
    ChatResult out;
    if (!Settings::get().configured()) { out.error = "No AI service is configured. Open settings to add one."; return out; }

    json body = {{"model", opt.model}, {"messages", messages}, {"stream", true},
                 {"max_tokens", opt.max_tokens}, {"temperature", opt.temperature}};
    if (tools.is_array() && !tools.empty()) {
        body["tools"] = tools;
        body["tool_choice"] = "auto";
    }
    // Reasoning models think privately before answering, which costs a second or more per
    // call. Fast mode asks them not to. Servers that reject the field get one retry without it.
    static std::atomic<bool> effort_rejected{false};
    const bool sent_effort = opt.fast && !effort_rejected;
    if (sent_effort) body["reasoning_effort"] = "none";

    // Experiments only (ZB_EXP_TRACE): the call's timings and the token counts the service reports,
    // which it sends only when asked. Never asked for otherwise; dropped for good after a 400.
    const bool tracing = experiment::tracing();
    static std::atomic<bool> usage_rejected{false};
    const bool sent_usage = tracing && !usage_rejected;
    if (sent_usage) body["stream_options"] = {{"include_usage", true}};
    const auto started = std::chrono::steady_clock::now();
    const long t_send = tracing ? experiment::request_ms() : -1;
    long ttfb = -1, first_text = -1, first_call = -1;
    int attempts = 1;
    json usage;
    auto since = [&] {
        return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
    };

    // With reasoning switched off, some models still leak their thinking markers into the
    // answer: a "<think>...</think>" block, or the answer, a stray "</think>", then the answer
    // again. Neither may reach the speaker, so text is passed on only once it is known to be
    // clear of a marker.
    std::string held;
    bool thinking = false, answer_over = false;
    auto emit = [&](const std::string &text) {
        if (text.empty()) return;
        out.content += text;
        if (opt.on_text) opt.on_text(text);
    };
    auto filter_text = [&](const std::string &piece, bool final) {
        static const std::string open = "<think>", close = "</think>";
        if (answer_over) return;
        held += piece;
        for (;;) {
            const size_t c = held.find(close);
            if (thinking) {
                if (c == std::string::npos) { if (held.size() > close.size()) held.erase(0, held.size() - close.size()); break; }
                held.erase(0, c + close.size());
                thinking = false;
                continue;
            }
            const size_t o = held.find(open);
            if (c != std::string::npos && (o == std::string::npos || c < o)) {
                emit(held.substr(0, c));
                held.clear();
                answer_over = true;
                break;
            }
            if (o != std::string::npos) {
                emit(held.substr(0, o));
                held.erase(0, o + open.size());
                thinking = true;
                continue;
            }
            // Keep back a tail that could be the start of a marker split across chunks.
            size_t keep = 0;
            for (size_t n = std::min(held.size(), close.size() - 1); n > 0 && !final; --n)
                if (close.compare(0, n, held, held.size() - n, n) == 0 || open.compare(0, n, held, held.size() - n, n) == 0) { keep = n; break; }
            emit(held.substr(0, held.size() - keep));
            held.erase(0, held.size() - keep);
            break;
        }
        if (final && thinking) held.clear();
    };

    std::string pending;  // partial SSE line carried between network chunks
    auto handle_line = [&](const std::string &line) {
        if (line.compare(0, 5, "data:") != 0) return;
        const std::string payload = util::trim(line.substr(5));
        if (payload.empty() || payload == "[DONE]") return;
        const json ev = json::parse(payload, nullptr, false);
        if (!ev.is_object()) return;
        if (ev.contains("error")) {
            const json &e = ev["error"];
            out.error = e.is_string() ? e.get<std::string>() : util::jget(e, "message", std::string("service error"));
            return;
        }
        // The token counts come in a last event of their own, with an empty list of choices.
        if (tracing && ev.contains("usage") && ev["usage"].is_object()) usage = experiment::numbers_only(ev["usage"]);
        if (!ev.contains("choices") || !ev["choices"].is_array() || ev["choices"].empty()) return;
        const json &choice = ev["choices"][0];
        if (!choice.contains("delta") || !choice["delta"].is_object()) return;
        const json &delta = choice["delta"];
        if (delta.contains("content") && delta["content"].is_string()) {
            const std::string piece = delta["content"].get<std::string>();
            if (!piece.empty()) {
                if (tracing && first_text < 0) first_text = since();
                filter_text(piece, false);
            }
        }
        if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
            if (tracing && first_call < 0) first_call = since();
            for (const json &tc : delta["tool_calls"]) {
                const size_t idx = static_cast<size_t>(util::jget(tc, "index", 0));
                if (out.tool_calls.size() <= idx) out.tool_calls.resize(idx + 1);
                ToolCall &call = out.tool_calls[idx];
                const std::string id = util::jget(tc, "id", std::string());
                if (!id.empty()) call.id = id;
                if (tc.contains("function") && tc["function"].is_object()) {
                    const json &fn = tc["function"];
                    const std::string name = util::jget(fn, "name", std::string());
                    if (!name.empty()) call.name = name;
                    if (fn.contains("arguments") && fn["arguments"].is_string())
                        call.args += fn["arguments"].get<std::string>();
                }
            }
        }
    };

    http::Request req;
    req.method = "POST";
    req.url = endpoint("/chat/completions");
    req.headers = auth_headers(true);
    req.headers.push_back("Accept: text/event-stream");
    req.body = dump(body);
    req.timeout_s = 180;
    req.cancel = opt.cancel;
    req.on_data = [&](const char *data, size_t n) {
        if (tracing && ttfb < 0) ttfb = since();
        pending.append(data, n);
        size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            handle_line(line);
        }
    };
    auto reset = [&] {
        pending.clear();
        held.clear();
        thinking = answer_over = false;
        out = ChatResult();
        ttfb = first_text = first_call = -1;
        usage = nullptr;
    };
    // Upstream hiccups (5xx, dropped connections, a service that says it is overloaded) are common
    // on mobile links. As long as nothing has been passed on yet, the call is tried again, twice at
    // most and all within its own three minutes: a timeout once the request had gone out is not
    // repeated, since that would double the wait.
    http::Retry retry = service_retry(3, req.timeout_s);
    retry.again = [&](const http::Response &r) {
        if (!out.content.empty() || !out.tool_calls.empty()) return false;
        return r.ok() ? !out.error.empty() && busy_message(out.error) : http::transient(r);
    };
    retry.before_resend = [&] { reset(); ++attempts; };
    http::Response res = http::send_retrying(req, retry);
    auto remaining_budget = [&] {
        const long spent_s = static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count());
        return std::max(1L, req.timeout_s - spent_s) * 1000;
    };
    if (sent_usage && res.status == 400 && out.content.empty() && out.tool_calls.empty()) {
        // Experiments only: a service that turns down the request for token counts gets the call
        // without it. If that is refused too, the counts were not the reason; ask for them again.
        // Only an answer without them proves they were (a failure on the way proves nothing).
        body.erase("stream_options");
        req.body = dump(body);
        reset();
        ++attempts;
        retry.budget_ms = remaining_budget();
        res = http::send_retrying(req, retry);
        if (res.ok()) usage_rejected = true;
        else if (res.status == 400) body["stream_options"] = {{"include_usage", true}};
    }
    if (sent_effort && res.status == 400 && out.content.empty() && out.tool_calls.empty()) {
        effort_rejected = true;
        body.erase("reasoning_effort");
        req.body = dump(body);
        reset();
        ++attempts;
        retry.budget_ms = remaining_budget();
        res = http::send_retrying(req, retry);
    }
    if (!pending.empty()) handle_line(pending);
    filter_text("", true);

    if (tracing) {
        // Counts, sizes and times only: never the words sent or received, a key or the model's id.
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
                                {"body_bytes", static_cast<long>(req.body.size())}, {"t_send", t_send}, {"ttfb_ms", ttfb},
                                {"first_text_ms", first_text}, {"first_call_ms", first_call}, {"done_ms", since()},
                                {"attempts", attempts}, {"status", res.status}, {"usage", usage}, {"outcome", outcome},
                                {"text_chars", static_cast<long>(out.content.size())}});
    }

    if (res.error == "cancelled") { out.cancelled = true; return out; }
    if (!res.ok() && out.error.empty()) out.error = api_error(res);
    // Drop empty slots and give every call an id (some servers omit them).
    std::vector<ToolCall> calls;
    for (auto &c : out.tool_calls) {
        if (c.name.empty()) continue;
        if (c.id.empty()) c.id = "call_" + std::to_string(calls.size());
        if (util::trim(c.args).empty()) c.args = "{}";
        calls.push_back(c);
    }
    out.tool_calls.swap(calls);
    return out;
}

std::string transcribe(const std::string &wav, std::string &err, std::atomic<bool> *cancel) {
    const Settings &s = Settings::get();
    if (!s.configured()) { err = "No AI service is configured."; return ""; }
    if (s.stt_model.empty()) { err = "No speech-to-text model is selected in settings."; return ""; }
    http::Request req;
    req.method = "POST";
    req.url = endpoint("/audio/transcriptions");
    req.headers = auth_headers(false);
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
    const json j = json::parse(res.body, nullptr, false);
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
        req.url = endpoint("/audio/speech");
        req.headers = auth_headers(true);
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
        const size_t data = pcm.find("data", 12);
        pcm.erase(0, data == std::string::npos ? 44 : data + 8);
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

std::vector<ModelInfo> list_models(std::string &err) {
    std::vector<ModelInfo> out;
    if (!Settings::get().configured()) { err = "No AI service is configured."; return out; }
    http::Request req;
    req.url = endpoint("/models");
    req.headers = auth_headers(false);
    req.timeout_s = 20;
    const http::Response res = http::send(req);
    if (!res.ok()) { err = api_error(res); return out; }
    const json j = json::parse(res.body, nullptr, false);
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
