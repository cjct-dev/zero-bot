// The AI service's list of models, for the places that read it: the app's own choice at start-up
// (App::auto_select_models), the setup guide's test of the service, the model pages in settings
// (pick_model) and the status page.
#pragma once
#include <algorithm>
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

// A model setting that named a model the service no longer lists, and the one it names now: for the note
// on the chat screen. `use` is the setting's label in settings.
struct Replaced {
    std::string use, was, now;
};

// The choice made without the user (at start-up, and in the setup guide once the service has answered):
// each model setting still empty gets the first model the service labels for it, and the language model
// only one labelled "chat" (ZB-088, the owner's rule: only chat models; an unlabelled model listed first,
// such as one for embeddings, is never taken). A service that labels none of its models gets nothing
// chosen: the user picks in settings. A setting naming a model the service no longer lists counts as
// empty when the service labels one for that use: the owner's service replaced its speech model
// (one speech model by another, 2026-11-24, ZB-119) and every spoken reply asked for the old one. Without
// such a label it stays, since a service may leave a model that works out of its list; the vision model,
// which may be empty (the language model's), is emptied instead. Returns what was replaced. Pure, so it
// can be tested without a screen.
inline std::vector<Replaced> choose(Settings &s, const List &list) {
    struct Use {
        const char *label, *mode;
        std::string Settings::*field;
    };
    static const Use uses[] = {{"Language model", "chat", &Settings::llm_model},
                               {"Speech to text", "audio_transcription", &Settings::stt_model},
                               {"Text to speech", "audio_speech", &Settings::tts_model}};
    const auto listed = [&list](const std::string &id) {
        return std::any_of(list.begin(), list.end(), [&id](const llm::ModelInfo &m) { return m.id == id; });
    };
    std::vector<Replaced> out;
    for (const Use &u : uses) {
        std::string &saved = s.*u.field;
        const auto first = std::find_if(list.begin(), list.end(), [&u](const llm::ModelInfo &m) { return m.mode == u.mode; });
        if (first == list.end() || (!saved.empty() && listed(saved))) continue;
        if (!saved.empty()) out.push_back({u.label, saved, first->id});
        saved = first->id;
    }
    const bool labels = std::any_of(list.begin(), list.end(), [](const llm::ModelInfo &m) { return !m.mode.empty(); });
    if (labels && !s.vision_model.empty() && !listed(s.vision_model)) {
        out.push_back({"Vision model", s.vision_model, "the language model"});
        s.vision_model.clear();
    }
    return out;
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
