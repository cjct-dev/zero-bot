// Client for an OpenAI-compatible service: chat (streaming, tools), speech-to-text,
// text-to-speech, vision and model listing.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "core/util.h"

namespace llm {

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
    std::string effort;                                 // reasoning effort to ask for ("none", "low", ...); overrides fast
    std::atomic<bool> *cancel = nullptr;
    std::function<void(const std::string &)> on_text;   // streamed answer text
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
std::vector<ModelInfo> list_models(std::string &err);

}  // namespace llm
