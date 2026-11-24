// Web search: the one layer between the web_search tool and the services that answer it. Each service is
// a Provider (provider.h), one file in providers/ and one line in providers/registry.cpp; this layer asks
// the services of the user's list (Settings::search) in its order until one answers, and shapes what the
// model reads, the same whichever service answered. Definitions in search.cpp.
//
// Failover, inside one search and never back through the model: a service that cannot be reached, is too
// slow, fails or answers garbled is followed by the next at once; one that found nothing (no summary, and no
// result that says anything) by the next, once. A search takes 8 s at most, the first of several services 5 s
// of them. A key a service refused is not sent again until it is typed again (marked beside it in the
// settings, so a later run does not send it either); a spent allowance is passed over until it comes back
// (when the service says, else for six hours; kept in the allowance file, usage.h, so a later run passes it
// over too); a service that failed twice in a row rests for two minutes (asked only when no other can be).
// The order never changes by itself. Each service is looked at again just before it is sent: one whose key
// was refused, switched off, changed or removed, or whose allowance was found spent, while another was asked
// is not sent. A search counts towards the day's once a service is sent the query. When no service answered,
// the model reads one ERROR sentence; so it does once the day's searches have reached the cap
// (usage::kDailyCap). Every key the settings hold is taken out of what a service answered before anything
// else sees it, and a failure reaches the model as the app's own sentence, never the service's words.
//
// An answer with results is kept for ten minutes, in memory only (32 at most; Brave's terms allow its results
// to be stored only transiently): the same query again, in other capitals or spacing, is answered from it
// without asking a service or counting a search. Failures, answers that found nothing and answers of a
// summary alone are not kept, and any change to the services in the settings (one switched on or off, moved,
// another key, a refusal), even one taken back since, empties it; an answer that came while they changed is
// not kept.
#pragma once
#include <cstdint>
#include <string>

#include "agent/tools.h"
#include "core/settings.h"

namespace search {

// Whether a service is set up: an entry of the user's list that is switched on, of a kind this version
// knows, with a key. The web_search tool is offered, and the prompt says search is available, by this
// alone, never by whether a service has allowance left today or took its key: both stay the same all
// conversation long.
bool configured(const Settings &s);

// The web_search tool's result for `query`: {"summary"?, "results": [{"title", "source", "text"}]}, or one
// "ERROR: ..." when no service answered.
std::string run(const std::string &query, tools::Context &ctx);

// How a service of the user's list stands, for the settings page: switched off, without a key, its key refused,
// its allowance used up, resting after two failures in a row, failed at its last search, answered its last
// search, or none of these yet in this run.
enum class State { Off, NoKey, KeyRefused, Spent, Resting, FailedLast, Working, Ready };
State state_of(const SearchService &service);
// The same in a few words: "working", "key refused: type it again", "allowance used until 1 December", ...
std::string describe(const SearchService &service);
// Counts what this run has learnt of the services: each answer a search had (or did not have) from one, each
// answer to ask_allowance. A page that shows how they stand can build its rows again when it changes, with one
// read. Time passing changes no count (a rest or a spent allowance running out): describe() says that.
uint64_t revision();

// Asks the user's service of this kind what is left of its allowance, in a way that costs none of it
// (Provider::tells_allowance), at most once a minute: what it said is kept in the allowance file (unless a
// search learnt something of the allowance while it was asked: that is newer), a key it refused is marked
// refused as a search's would be. "" when it answered, else why not, in a sentence of the app's own for the
// settings page (never the service's words). Only when the user asks for it there, never at start or for a
// search.
std::string ask_allowance(const std::string &kind, tools::Context &ctx);

}  // namespace search
