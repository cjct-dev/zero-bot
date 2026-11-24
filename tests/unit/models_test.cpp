// Unit tests for ui/models.h's two rules (ZB-088): what is chosen without the user (only a model
// labelled "chat" becomes the language model) and what the settings page lists for the user to choose
// from (the models labelled for it; else the unlabelled ones; else everything: never nothing), and a saved model
// the service no longer lists (ZB-119). Header-only and pure: no screen, nothing linked.
// Each check prints one line on failure; the exit status is non-zero when any fails.
#include "ui/models.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_passed = 0, g_failed = 0;

void check(bool ok, const std::string &what) {
    if (ok) ++g_passed;
    else { ++g_failed; printf("FAIL %s\n", what.c_str()); }
}

using ui::models::List;
using Ids = std::vector<std::string>;

}  // namespace

int main() {
    // A service that labels its models, with an unlabelled one (an embedding model) listed first.
    const List labelled = {{"embed", ""}, {"chatty", "chat"}, {"ears", "audio_transcription"}, {"voice", "audio_speech"}};
    Settings s;
    ui::models::choose(s, labelled);
    check(s.llm_model == "chatty", "labelled service: the automatic pick takes the chat model, not the unlabelled one first (" + s.llm_model + ")");
    check(s.stt_model == "ears" && s.tts_model == "voice", "labelled service: speech models are picked by their labels");
    check(ui::models::offered(labelled, "chat") == Ids{"chatty"}, "labelled service: settings list only the chat model");

    // A service that labels nothing: no automatic pick, and settings list every model.
    const List unlabelled = {{"one", ""}, {"two", ""}};
    Settings none;
    ui::models::choose(none, unlabelled);
    check(none.llm_model.empty() && none.stt_model.empty() && none.tts_model.empty(),
          "unlabelled service: nothing is picked (the user chooses)");
    check(ui::models::offered(unlabelled, "chat") == Ids{"one", "two"}, "unlabelled service: settings list every model");
    check(ui::models::offered(unlabelled, "audio_speech") == Ids{"one", "two"}, "unlabelled service: for speech too");

    // A service with no chat model: no language model is picked, the rest still are.
    const List no_chat = {{"embed", ""}, {"ears", "audio_transcription"}};
    Settings no_llm;
    ui::models::choose(no_llm, no_chat);
    check(no_llm.llm_model.empty(), "no chat model: no language model is picked (" + no_llm.llm_model + ")");
    check(no_llm.stt_model == "ears", "no chat model: speech to text is still picked");
    check(ui::models::offered(no_chat, "chat") == Ids{"embed"},
          "no chat model: settings list the unlabelled model for the language model, not the speech one");

    // A service that labels only its speech models and leaves its chat model unlabelled (ZB-088's
    // review): nothing is picked for the language model without the user, and the settings page,
    // where the setup guide sends them, lists that model rather than nothing.
    const List speech_only = {{"talker", ""}, {"ears", "audio_transcription"}, {"voice", "audio_speech"}};
    Settings partial;
    ui::models::choose(partial, speech_only);
    check(partial.llm_model.empty() && partial.stt_model == "ears" && partial.tts_model == "voice",
          "speech labelled only: the speech models are picked, the language model is left to the user");
    check(ui::models::offered(speech_only, "chat") == Ids{"talker"}, "speech labelled only: settings list the unlabelled model");
    check(ui::models::offered(speech_only, "audio_speech") == Ids{"voice"}, "speech labelled only: the voice list is still by label");

    // Every model labelled as another kind: the list for the language model is every model, not none.
    const List others = {{"ears", "audio_transcription"}, {"voice", "audio_speech"}};
    check(ui::models::offered(others, "chat") == Ids{"ears", "voice"}, "only other kinds: settings list every model");

    // A model already chosen, that the service still lists, stays, even when it is not the first for its use.
    const List two_chats = {{"chatty", "chat"}, {"other", "chat"}, {"ears", "audio_transcription"}, {"voice", "audio_speech"},
                            {"seer", ""}};
    Settings kept;
    kept.llm_model = "other";
    kept.vision_model = "seer";
    kept.tts_model = "voice";
    check(ui::models::choose(kept, two_chats).empty() && kept.llm_model == "other" && kept.vision_model == "seer" &&
          kept.tts_model == "voice", "a model chosen that is still listed stays, and nothing is reported");
    Settings filled;
    check(ui::models::choose(filled, two_chats).empty(), "an empty setting filled is not reported as replaced");

    // The service replaced its speech model (ZB-119: one speech model by another): a saved model it no longer
    // lists counts as empty when it labels one for that use, and the change is reported for the chat screen.
    const List swapped = {{"chatty", "chat"}, {"ears", "audio_transcription"}, {"new-voice", "audio_speech"}};
    Settings stale;
    stale.llm_model = "chatty";
    stale.stt_model = "ears";
    stale.tts_model = "old-voice";
    const std::vector<ui::models::Replaced> r = ui::models::choose(stale, swapped);
    check(stale.tts_model == "new-voice", "a speech model the service no longer lists is replaced by the labelled one (" + stale.tts_model + ")");
    check(r.size() == 1 && r[0].use == "Text to speech" && r[0].was == "old-voice" && r[0].now == "new-voice",
          "the replacement is reported with the setting's label, the old model and the new");
    check(stale.llm_model == "chatty" && stale.stt_model == "ears", "the models still listed stay");
    Settings gone_chat;
    gone_chat.llm_model = "retired";
    ui::models::choose(gone_chat, swapped);
    check(gone_chat.llm_model == "chatty", "a language model no longer listed is replaced by one labelled chat");
    Settings gone_vision;
    gone_vision.vision_model = "retired";
    const std::vector<ui::models::Replaced> v = ui::models::choose(gone_vision, swapped);
    check(gone_vision.vision_model.empty() && v.size() == 1 && v[0].use == "Vision model",
          "a vision model no longer listed falls back to the language model (empty), and is reported");

    // No model labelled for that use, or a service that labels nothing: a saved model it does not list stays
    // (a service may leave a model that works out of its list).
    Settings unlisted;
    unlisted.llm_model = "mine";
    unlisted.tts_model = "my voice";
    unlisted.vision_model = "my eyes";
    check(ui::models::choose(unlisted, unlabelled).empty() && unlisted.llm_model == "mine" && unlisted.tts_model == "my voice" &&
          unlisted.vision_model == "my eyes", "unlabelled service: models it does not list stay");
    Settings no_label_for_it;
    no_label_for_it.tts_model = "my voice";
    ui::models::choose(no_label_for_it, no_chat);
    check(no_label_for_it.tts_model == "my voice", "no model labelled for speech: the saved speech model stays");

    printf("models: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
