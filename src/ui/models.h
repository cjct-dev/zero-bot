// The AI service's list of models, for the places that read it: the app's own choice at start-up
// (App::auto_select_models), the setup guide's test of the service, the model pages in settings
// (pick_model) and the status page.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "agent/llm.h"
#include "core/settings.h"

namespace ui::models {

using List = std::vector<llm::ModelInfo>;

// The service's models, asked for now (up to 20 s): not on the UI thread. `err` says why there are none.
List list(std::string &err);

// Asks off the UI thread, then hands `done` the list, or the service's error, on the UI thread.
void fetch(std::function<void(const List &list, const std::string &err)> done);

// The choice made without the user (at start-up, and in the setup guide once the service has answered):
// each model setting still empty gets the first model the service labels for it, and the language model
// only one labelled "chat" (ZB-088, the owner's rule: only chat models; an unlabelled model listed first,
// such as one for embeddings, is never taken). A service that labels none of its models gets nothing
// chosen: the user picks in settings. Pure, so it can be tested without a screen.
inline void choose(Settings &s, const List &list) {
    for (const llm::ModelInfo &m : list) {
        if (s.llm_model.empty() && m.mode == "chat") s.llm_model = m.id;
        if (s.stt_model.empty() && m.mode == "audio_transcription") s.stt_model = m.id;
        if (s.tts_model.empty() && m.mode == "audio_speech") s.tts_model = m.id;
    }
}

// The models the settings page offers for `mode` ("chat", "audio_transcription", "audio_speech"): those
// labelled for it when the service labels any model, and every model when it labels none. That second
// case is not the rule above: here the user is choosing by hand, and the list would otherwise be empty.
inline std::vector<std::string> offered(const List &list, const std::string &mode) {
    bool labelled = false;
    for (const llm::ModelInfo &m : list) labelled = labelled || !m.mode.empty();
    std::vector<std::string> ids;
    for (const llm::ModelInfo &m : list)
        if (!labelled || m.mode == mode) ids.push_back(m.id);
    return ids;
}

}  // namespace ui::models
