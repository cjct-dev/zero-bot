#include "ui/models.h"

#include <utility>

#include "ui/settings_internal.h"

namespace ui::models {

List list(std::string &err) { return llm::list_models(err); }

void fetch(std::function<void(const List &list, const std::string &err)> done) {
    background(
        [] {
            std::string err;
            auto found = list(err);
            return std::make_pair(found, err);
        },
        [done](const std::pair<List, std::string> &r) { done(r.first, r.second); });
}

void choose_first(Settings &s, const List &list) {
    for (auto &m : list) {
        if (s.stt_model.empty() && m.mode == "audio_transcription") s.stt_model = m.id;
        if (s.tts_model.empty() && m.mode == "audio_speech") s.tts_model = m.id;
        if (s.eff_llm_model().empty() && m.mode == "chat") s.llm_model = m.id;
    }
}

void choose_for_setup(Settings &st, const List &list) {
    for (auto &model : list) {
        if (st.llm_model.empty() && (model.mode == "chat" || model.mode.empty())) st.llm_model = model.id;
        if (st.stt_model.empty() && model.mode == "audio_transcription") st.stt_model = model.id;
        if (st.tts_model.empty() && model.mode == "audio_speech") st.tts_model = model.id;
    }
}

}  // namespace ui::models
