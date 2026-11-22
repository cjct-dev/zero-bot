// The world_time tool: the time in another place, or a local time there.
#include "agent/tools/common.h"

#include "agent/tools/places.h"

namespace tools {

namespace {

std::string t_world_time(const json &a, Context &ctx) {
    const std::string place = str_arg(a, "place");
    if (place.empty()) return fail("place is required (a city or an IANA zone such as Asia/Tokyo)");
    if (several_zones(place)) return several_zones_error(place);
    std::string zone = place, label = place, err;
    if (!util::timezone_exists(zone)) {
        Place p;
        if (!geocode(place, p, err, ctx)) return fail(err);
        if (p.timezone.empty() || !util::timezone_exists(p.timezone)) return fail("no time zone known for " + place);
        zone = p.timezone;
        label = p.name + (p.country.empty() ? "" : ", " + p.country);
    }
    time_t t = time(nullptr);
    const std::string at = str_arg(a, "local_time");
    if (!at.empty() && !util::parse_local(at, t)) return fail(bad_time("local_time", at));
    return json{{"place", label}, {"timezone", zone},
                {"time_there", util::format_time_in(t, "%A %Y-%m-%d %H:%M (UTC%z)", zone)},
                {"time_here", util::format_time(t, "%A %Y-%m-%d %H:%M (UTC%z)")}}.dump();
}

}  // namespace

Tool make_world_time() {
    return {"world_time", "Checking the time",
            "The current time in another place, or what a given local time corresponds to there. The current local "
            "date and time are already in your instructions; do not call this for the user's own time.",
            obj({{"place", S("City or IANA zone, e.g. \"Tokyo\" or \"America/New_York\"")},
                 {"local_time", S("Optional local time to convert, YYYY-MM-DD HH:MM; omit for now")}}, {"place"}),
            t_world_time};
}

}  // namespace tools
