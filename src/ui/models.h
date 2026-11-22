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

// The models of `service` (its address and key, as read once: llm::Service), asked for now (up to 20 s):
// not on the UI thread. `err` says why there are none.
List list(const llm::Service &service, std::string &err);

// Asks the service the settings name now, off the UI thread, then hands `done` the list, or the service's
// error, on the UI thread, unless the service has changed meanwhile. The request and that check use the
// same address and key, read once here: a list is of no use for another service, and the key read with
// one address is never sent to another.
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
// labelled for it when any is; else every model not labelled as another kind; else every model. This
// is not the rule above: here the user is choosing by hand, and the list must not come out empty while
// the service lists models. A service that labels only its speech models leaves its chat model
// unlabelled: the automatic choice rightly skips it, and this list is where the user picks it.
inline std::vector<std::string> offered(const List &list, const std::string &mode) {
    std::vector<std::string> labelled, unlabelled, all;
    for (const llm::ModelInfo &m : list) {
        if (m.mode == mode) labelled.push_back(m.id);
        else if (m.mode.empty()) unlabelled.push_back(m.id);
        all.push_back(m.id);
    }
    if (!labelled.empty()) return labelled;
    return unlabelled.empty() ? all : unlabelled;
}

}  // namespace ui::models
