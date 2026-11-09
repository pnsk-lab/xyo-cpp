// Source-level continuation phase for LLVM's opt-in portable CallFrame.
// This is NOT a machine PC or a general SSA deoptimization contract.
#pragma once

#include <cstdint>

namespace xyo::ir {

// Stored in the final local of every nonroot native procedure frame:
// N loop counters, N ordered loop PCs, path depth, then phase.
// The root has no locals. Every enum value is a fixed-width ABI number.
enum class SourceResumePhase : uint64_t {
    ProcedureEntry = 0,   // callee has not executed its first statement
    InsideLoopBody = 1,   // about to invoke a nested Scratch procedure
    AfterLoopBody = 2,    // native yield after a completed loop iteration
    AwaitingCallee = 3,   // parent has issued a call, child currently active
    AfterCallee = 4,      // child returned, caller's after-call site pending
    UnknownBody = 5,     // entered a procedure; arbitrary SSA/effect PC absent
};

// Describes what a detached source checkpoint proves, not whether its
// machine-independent execution is authorized. Consumers must separately
// verify source revision, effect-commit continuity and any required live SSA.
// UnknownBody cannot be reconstructed from the current source PC alone.
enum class SourceDeoptBoundary : uint8_t {
    ProcedureEntry,
    LoopBody,
    LoopLatch,
    PostCall,
    SuspendedCaller,
    UnresolvedBody,
};

constexpr SourceDeoptBoundary sourceDeoptBoundary(SourceResumePhase phase) {
    switch (phase) {
    case SourceResumePhase::ProcedureEntry:
        return SourceDeoptBoundary::ProcedureEntry;
    case SourceResumePhase::InsideLoopBody:
        return SourceDeoptBoundary::LoopBody;
    case SourceResumePhase::AfterLoopBody:
        return SourceDeoptBoundary::LoopLatch;
    case SourceResumePhase::AfterCallee:
        return SourceDeoptBoundary::PostCall;
    case SourceResumePhase::AwaitingCallee:
        return SourceDeoptBoundary::SuspendedCaller;
    case SourceResumePhase::UnknownBody:
        return SourceDeoptBoundary::UnresolvedBody;
    }
    return SourceDeoptBoundary::UnresolvedBody;
}

// Conservative obligations for the executor receiving a detached frame.
// No bit is a certificate that the obligation has been discharged. In
// particular all boundaries still need an external committed-effect proof.
enum class SourceDeoptNeed : uint8_t {
    EffectContinuity = 1u << 0,
    LiveSsaValues = 1u << 1,
    ExactStatementPc = 1u << 2,
    ActiveCallee = 1u << 3,
};

struct SourceDeoptRequirements {
    uint8_t bits = 0;
    constexpr bool needs(SourceDeoptNeed requirement) const {
        return (bits & static_cast<uint8_t>(requirement)) != 0;
    }
};

constexpr SourceDeoptRequirements sourceDeoptRequirements(
    SourceDeoptBoundary boundary) {
    const uint8_t effects = static_cast<uint8_t>(
        SourceDeoptNeed::EffectContinuity);
    switch (boundary) {
    case SourceDeoptBoundary::ProcedureEntry:
        return {effects};
    // Source-level loop and after-call markers cannot prove that LLVM
    // optimized no live temporaries across this boundary. Require a
    // liveness certificate even when a specific project needs none.
    case SourceDeoptBoundary::LoopBody:
    case SourceDeoptBoundary::LoopLatch:
    case SourceDeoptBoundary::PostCall:
        return {uint8_t(effects |
            uint8_t(SourceDeoptNeed::LiveSsaValues))};
    case SourceDeoptBoundary::SuspendedCaller:
        return {uint8_t(effects |
            uint8_t(SourceDeoptNeed::LiveSsaValues) |
            uint8_t(SourceDeoptNeed::ActiveCallee))};
    case SourceDeoptBoundary::UnresolvedBody:
        return {uint8_t(effects |
            uint8_t(SourceDeoptNeed::LiveSsaValues) |
            uint8_t(SourceDeoptNeed::ExactStatementPc))};
    }
    return {uint8_t(effects |
        uint8_t(SourceDeoptNeed::LiveSsaValues) |
        uint8_t(SourceDeoptNeed::ExactStatementPc))};
}

inline bool validSourceResumePhase(uint64_t phase) {
    return phase <= static_cast<uint64_t>(SourceResumePhase::UnknownBody);
}
} // namespace xyo::ir
