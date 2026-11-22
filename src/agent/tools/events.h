// What the calendar tools (calendar_add, calendar_list, calendar_update) share: an event as the model
// sees it, and the arguments add and update read alike. Definitions in events.cpp.
#pragma once
#include <string>

#include "agent/tools.h"

struct CalEvent;

namespace tools {

bool repeat_arg(const json &a, std::string &out, std::string &err);   // "repeat" into out
json event_json(const CalEvent &e);                                   // an event or one occurrence, for the model
bool event_text_args(const json &a, CalEvent &e, std::string &err);   // title, notes and alert lead time
bool in_the_past(const CalEvent &e);                                  // a one-off event that has gone by
std::string past_error();                                             // the error that says so

}  // namespace tools
