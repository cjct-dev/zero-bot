#pragma once
#include <string>

namespace prompt {
// Builds the system prompt for the current moment (time, memories, settings).
std::string system_prompt();
// Turns answer text into something a speech synthesizer reads well.
std::string for_speech(const std::string &text);
}
