// The look tool: a photo from the camera, described by the vision model.
#include "agent/tools/common.h"

#include "agent/consent.h"
#include "agent/llm.h"
#include "core/settings.h"
#include "core/tether.h"
#include "platform/camera.h"

namespace tools {

namespace {

std::string t_look(const json &a, Context &ctx) {
    std::string question = str_arg(a, "question");
    if (question.empty()) question = "Describe what you see.";
    const std::string path = util::cache_dir() + "/last_photo.jpg";
    util::make_dirs(util::cache_dir());
    // On a phone's Bluetooth link the upload is the slow part, so a smaller photo is taken there:
    // still plenty to read a sign or menu, at about a third of the bytes.
    const bool slow_link = tether::via_phone();
    const bool flip = Settings::get().camera_flip;
    std::string err = slow_link ? camera::capture(path, 960, 720, flip, ctx.cancel) : camera::capture(path, 1280, 960, flip, ctx.cancel);
    if (ctx.cancel && *ctx.cancel) return fail("stopped: the user cancelled the request");
    if (!err.empty()) return fail(err + ". Tell the user; do not describe a photo");
    if (ctx.on_image) ctx.on_image(path);
    std::string jpeg;
    if (!util::read_file(path, jpeg)) return fail("could not read the photo");
    const std::string answer = llm::describe_image(jpeg, question, err, ctx.cancel);
    if (!err.empty()) return fail("the photo was taken but could not be analysed: " + err);
    if (answer.empty()) return fail("the vision model returned nothing");
    consent::saw_untrusted_text();   // a sign in the photo must not switch anything or be remembered by itself
    return answer;
}

}  // namespace

Tool make_look() {
    return {"look", "Looking",
            "Take a photo with the device camera (on the back, pointing away from the user) and answer a question "
            "about it. Use it whenever the user refers to something in front of them: 'what is this', 'read this', "
            "'translate this sign or menu', 'how much is this'. Each call takes a new photo.",
            obj({{"question", S("What to find out from the photo, with any detail the answer needs, e.g. \"Transcribe "
                                "all text on the menu and translate it into English\"")}}, {"question"}), t_look};
}

}  // namespace tools
