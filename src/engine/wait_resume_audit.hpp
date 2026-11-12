// Diagnostic classification of what happened when a wait-until resumes.
// No execution decisions depend on these values.
#pragma once

#include <cstdint>

namespace xyo {

struct WaitResumeState {
    bool armed = false;
    uintptr_t site = 0;
    uint64_t exitCount = 0;
};

enum class WaitResumeOutcome : uint8_t {
    RewaitSame = 0,    // Same wait still false; no completed wait.
    Exited = 1,        // Native wait completion without rearming.
    ExitedRearmed = 2, // Completed a wait then re-entered a wait in this slice.
    ChangedSite = 3,  // Armed a different site without a completion signal.
    Incomplete = 4,   // Disarmed without completion or identity reset.
    Count = 5,
};

inline WaitResumeOutcome classifyWaitResume(
        WaitResumeState before, WaitResumeState after) noexcept {
    if (after.exitCount < before.exitCount)
        return WaitResumeOutcome::Incomplete;
    if (after.exitCount > before.exitCount)
        return after.armed ? WaitResumeOutcome::ExitedRearmed
                           : WaitResumeOutcome::Exited;
    if (!after.armed)
        return WaitResumeOutcome::Incomplete;
    return after.site == before.site ? WaitResumeOutcome::RewaitSame
                                     : WaitResumeOutcome::ChangedSite;
}

} // namespace xyo
