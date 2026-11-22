// The small-model controls (ZB-034): the parts of the request loop and the tools that are there
// because the model in use is small and, in fast mode, answers with its reasoning switched off.
// Each has a name, so that what it is worth can be measured by running the test sets without it;
// a newer model may need fewer of them.
//
// In the device and simulator builds every control is always on: on() is a constant true and the
// switch below is not compiled in. In the cli build (ZB_EXPERIMENTS, see experiment.h)
//
//   ZB_EXP_OFF=NAME[,NAME...]   switches the named controls off; "all" switches every one off
//
// and a name the build does not know stops the run at start, before anything is sent (a typing slip
// would otherwise measure the default under another name). The trace's start line (ZB_EXP_TRACE)
// names the controls switched off, as "off". Nothing the model sends can switch a
// control: never make one a tool parameter (AGENTS.md section 8, no way round a guard).
#pragma once
#include <string>

#ifndef ZB_EXPERIMENTS
#define ZB_EXPERIMENTS 0
#endif

namespace controls {

enum Control {
    SecondLook,     // second_look: fast mode holds a draft that calls no tool and checks it once before it is spoken
    DecideNote,     // decide_note: fast mode adds a note to the newest request, to call a tool rather than reply
    ClaimGuards,    // claim_guards: "done" with no tool behind it or after calls that all failed, and "that isn't
                    //   saved" without looking, are withdrawn once and the model told (guards::kClaims)
    Say,            // say: tools take the sentence to speak once they have worked (the "say" parameter, and its
                    //   confirmation used in place of another model call); the system prompt's rule for it
    WeekdayCheck,   // weekday_check: calendar and reminders refuse a say naming a weekday the date is not
    CancelClaims,   // cancel_claims: a cancel or delete whose say claims something new is refused
    DateSpelling,   // date_spelling: numeric dates in what the user says are spelt out before the model sees them
};

#if ZB_EXPERIMENTS
bool on(Control c);
std::string off_names();   // the controls switched off, by name, comma-separated ("" when none is)
#else
constexpr bool on(Control) { return true; }
#endif

}  // namespace controls
