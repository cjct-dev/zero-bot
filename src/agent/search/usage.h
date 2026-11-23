// Web search's allowance (search.h): for each service set up, the searches it answered this month as counted here,
// what the service itself last said is left, and until when its allowance is used up; and the searches sent today,
// against a daily cap. Kept in ~/.local/share/zero-bot/search_usage.json (0600), so that a later run knows too: counts,
// figures and times only, never a query, a result or a key (a service is known by provider.h's id_of, its kind and a
// print of its key: another key is another allowance). Only the layer (search.cpp) changes it. Definitions in usage.cpp.
//
// The wall clock: a board without a battery-backed clock starts in 1970 and is set by a time sync some while later
// (AGENTS.md section 9, "Wall-clock time"). While it reads a year before 2026 nothing rolls over (the month's and the
// day's counts go on in the period they were counted in), no figure is dated, and a spent allowance whose end the file
// holds stays spent. An end learnt in this run is kept on the monotonic clock, which setting the wall clock does not move.
#pragma once
#include <ctime>
#include <string>

#include "agent/search/provider.h"

namespace search::usage {

// Searches a day, every service together: past it, search answers one ERROR sentence until the next day. A guard
// against a loop spending the month's allowance, well above a person's day (Tavily's free month is 1,000).
constexpr int kDailyCap = 100;

// What the settings page shows of one service.
struct Figures {
    long used = 0;                // searches it answered this month, counted on this device
    long left = -1, limit = -1;   // what it last said is left, of how much (-1: it never said)
    time_t said_at = 0;           // when it said so (0: the clock was not set)
    bool spent = false;           // its allowance is used up: it is not asked
    time_t spent_until = 0;       // until then, on the wall clock (0: not known)
};
Figures of(const std::string &id);
bool spent(const std::string &id);

// What one search's reply said: an answer counts one search (each service's searches cost one unit), a figure the
// service gave is kept with its time, and a spent allowance (or nothing left after this answer) is passed over until
// it comes back: when the service says, else for six hours.
void note(const std::string &id, const Reply &reply);
// What the service said when asked what is left (Provider::allowance): the figure is kept; nothing left marks it
// spent, something left takes a spent mark away.
void said(const std::string &id, const Reply &reply);

int today();   // searches sent today
void sent();   // one more search sent today

// Writes the file when anything changed: the layer calls it once a search is done, and after asking a service what is
// left. A service no longer in the settings' list is left out of it.
void save();

// The layer's clocks: milliseconds on the monotonic clock, and the wall clock (provider.h's testing hooks set both).
unsigned long now_ms();
time_t wall_now();

// The memory of the file as read (testing::forget): it is read again at the next use.
void forget();

}  // namespace search::usage
