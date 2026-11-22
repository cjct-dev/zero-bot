// The set_timezone tool: the device's time zone changed.
#include "agent/tools/common.h"

#include "agent/tools/places.h"
#include "core/settings.h"

namespace tools {

namespace {

std::string t_set_timezone(const json &a, Context &ctx) {
    const std::string place = str_arg(a, "place");
    if (place.empty()) return fail("place is required (a city or an IANA zone such as Europe/Lisbon)");
    if (several_zones(place)) return several_zones_error(place);
    std::string zone = place, err;
    if (!util::timezone_exists(zone)) {
        Place p;
        if (!geocode(place, p, err, ctx)) return fail(err);
        if (p.timezone.empty() || !util::timezone_exists(p.timezone)) return fail("no time zone known for " + place);
        zone = p.timezone;
    }
    Settings::update([&](Settings &s) { s.timezone = zone; });
    util::apply_timezone(zone);
    return json{{"timezone", zone}, {"local_time_now", util::format_time(time(nullptr), "%A %Y-%m-%d %H:%M")},
                {"note", "Calendar entries keep their absolute moment, so they now show in the new zone."}}.dump();
}

}  // namespace

Tool make_set_timezone() {
    return {"set_timezone", "Changing time zone",
            "Change the time zone this device uses, for when the user has travelled or asks to switch zone.",
            obj({{"place", S("City or IANA zone the user is now in")}}, {"place"}), t_set_timezone};
}

}  // namespace tools
