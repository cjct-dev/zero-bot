// Client for an OpenAI-compatible service: chat (streaming, tools), speech-to-text,
// text-to-speech, vision and model listing.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "core/util.h"

struct Settings;   // core/settings.h

namespace llm {

// The AI service a request is sent to: its address and key, read together from one snapshot of the settings
// (`of`) and not read again on the way, so a key never goes to an address saved after it was read (fix round
// 4, E6). Compared, it tells an answer from another service's (models::fetch).
struct Service {
    std::string base, key;
    static Service of(const Settings &s);
    bool operator==(const Service &o) const { return base == o.base && key == o.key; }
    bool operator!=(const Service &o) const { return !(*this == o); }
};

struct ToolCall {
    std::string id, name, args;   // args is the raw JSON text from the model
};

struct ChatResult {
    std::string content;
    std::vector<ToolCall> tool_calls;
    std::string error;            // empty on success
    bool cancelled = false;
};

struct ChatOptions {
    std::string model;
    int max_tokens = 1200;
    double temperature = 0.4;
    bool fast = false;                                  // skip the model's reasoning step
    std::atomic<bool> *cancel = nullptr;
    std::function<void(const std::string &)> on_text;   // streamed answer text
    // What the call is for, for the experiments' trace only (agent/experiment.h); changes nothing
    // that is sent. role: "main" if empty, "vision"...; purpose: "decide", "second_look"...
    std::string role, purpose;
};

ChatResult chat(const json &messages, const json &tools, const ChatOptions &opt);

// Returns the transcript, or "" with err set.
std::string transcribe(const std::string &wav, std::string &err, std::atomic<bool> *cancel = nullptr);

// Synthesizes speech as raw 16-bit mono PCM at kTtsRate.
constexpr int kTtsRate = 24000;
bool synthesize(const std::string &text, std::string &pcm, std::string &err, std::atomic<bool> *cancel = nullptr);

// Asks the vision model a question about a JPEG image.
std::string describe_image(const std::string &jpeg, const std::string &question, std::string &err,
                           std::atomic<bool> *cancel = nullptr);

struct ModelInfo {
    std::string id, mode;         // mode: "chat", "audio_transcription", "audio_speech" or ""
};
// The models of `service`, asked of it whatever the settings say by then.
std::vector<ModelInfo> list_models(const Service &service, std::string &err);

}  // namespace llm
