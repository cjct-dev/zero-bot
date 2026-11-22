// The tv_remote tool: a TV button pressed through the infrared transmitter.
#include "agent/tools/common.h"

#include <algorithm>

#include "core/settings.h"
#include "platform/ir.h"

namespace tools {

namespace {

std::string t_tv_remote(const json &a, Context &ctx) {
    const std::string key = util::lower(str_arg(a, "key"));
    if (key.empty()) return fail("key is required: the one button to press, such as power or volume_up");
    double repeat_d = 1;
    if (given(a, "repeat") && !num_arg(a, "repeat", repeat_d)) return fail("repeat must be a number of presses, such as 5");
    const int repeat = clamp_int(repeat_d, 1, ir::kMaxRepeat);
    std::string brand = util::lower(str_arg(a, "brand"));
    const auto known = ir::brands();
    auto listed = [](const std::vector<std::string> &names) {
        std::string out;
        for (const auto &n : names) out += (out.empty() ? "" : ", ") + n;
        return out;
    };
    if (!brand.empty() && std::find(known.begin(), known.end(), brand) == known.end())
        return fail("unknown TV brand '" + util::truncate_utf8(brand, 30) + "'; use one of " + listed(known) +
                    ", or leave brand out to send every brand's code");
    const std::string saved_brand = Settings::get().tv_brand;
    if (!brand.empty() && brand != saved_brand) Settings::update([&](Settings &s) { s.tv_brand = brand; });   // remember a brand the user mentions
    if (brand.empty()) brand = saved_brand;
    const std::string err = ir::send(brand, key, repeat, ctx.cancel);
    if (!err.empty()) {
        const auto keys = ir::keys();
        if (std::find(keys.begin(), keys.end(), key) == keys.end() && err.compare(0, 11, "no code for") == 0)
            return fail(err + "; the keys are " + listed(keys));
        return fail(err);
    }
    return "sent '" + key + "'" + (repeat > 1 ? " x" + std::to_string(repeat) : "") + " (" +
           (brand.empty() || brand == "auto" ? "all known brands" : brand) +
           "). The device cannot see whether the TV reacted." +
           (repeat_d > ir::kMaxRepeat ? " At most " + std::to_string(ir::kMaxRepeat) + " presses are sent at a time." : "");
}

}  // namespace

Tool make_tv_remote() {
    return {"tv_remote", "Using the remote",
            "Press a button on a TV using the device's infrared transmitter (the user points the top edge of the "
            "device at the TV). Send one key per call; use repeat for several presses, e.g. volume_up with repeat 5. "
            "Works without knowing the brand; pass brand if the user says it. A TV that the smart_home tool lists is "
            "controlled with smart_home instead (its keys, volume and power, no pointing needed); use this one for "
            "other TVs. It sends the codes of the one brand in settings, so if a listed TV does not answer over the "
            "network, say so rather than trying this.",
            obj({{"key", E("Button to press", {"power", "volume_up", "volume_down", "mute", "channel_up", "channel_down",
                                               "input", "up", "down", "left", "right", "ok", "menu", "back", "home",
                                               "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"})},
                 {"repeat", N("How many times to press (default 1)")},
                 {"brand", S("Optional TV brand: samsung, lg, sony, panasonic, philips, toshiba, vizio")}}, {"key"}),
            t_tv_remote};
}

}  // namespace tools
