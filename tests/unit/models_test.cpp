// Unit tests for ui/models.h's two rules (ZB-088): what is chosen without the user (only a model
// labelled "chat" becomes the language model) and what the settings page lists for the user to choose
// from (everything when the service labels nothing). Header-only and pure: no screen, nothing linked.
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
    check(ui::models::offered(no_chat, "chat").empty(), "no chat model: settings list none for the language model");

    // A model already chosen is never replaced.
    Settings kept;
    kept.llm_model = "mine";
    kept.tts_model = "my voice";
    ui::models::choose(kept, labelled);
    check(kept.llm_model == "mine" && kept.tts_model == "my voice", "a model already chosen stays");

    printf("models: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
