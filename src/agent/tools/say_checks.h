// Checks of what a call's "say" claims. weekday_slip and claims_new are made before anything is saved or
// cancelled: each returns "" when the call may go ahead, or the error's words (for fail()) and sets
// Context::say_refused; the small-model controls weekday_check and cancel_claims (controls.h) switch them off.
// claims_start decides only whether a stop's say may end the request (Say::when). Definitions in say_checks.cpp.
#pragma once
#include <ctime>
#include <string>

#include "agent/tools.h"

namespace tools {

// say names one weekday that `when` (the call's `field`) is not.
std::string weekday_slip(const json &a, Context &ctx, time_t when, const char *field);
// A cancel or delete whose say claims something new was set; `done` says what the tool does, `fix` what to do.
std::string claims_new(const json &a, Context &ctx, const std::string &done, const std::string &fix);
// A stop whose say claims that something starts ("Stopping the music and putting Radio 2 on"): the say is not
// spoken as the whole answer, so the model is asked again and makes the step it announced (the music tool's stop).
bool claims_start(const json &a);

}  // namespace tools
