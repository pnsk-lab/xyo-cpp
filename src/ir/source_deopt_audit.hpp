// IR-001: diagnostic account of source continuation proof obligations.
// Never treats SSA values or a fingerprint as an effect-commit certificate.
#pragma once

#include "source_resume_phase.hpp"
#include "source_effect_frontier.hpp"
#include "source_typed_ssa_bridge.hpp"

#include <cstdint>
#include <optional>

namespace xyo::ir {

enum class SourceDeoptGap : uint8_t {
    EffectCommit = 1u << 0,
    LiveSsa = 1u << 1,
    ExactStatement = 1u << 2,
    ActiveCallee = 1u << 3,
    InvalidSnapshot = 1u << 4,
    UnresolvedPointers = 1u << 5,
    EffectFrontierMismatch = 1u << 6,
    GcRootContinuity = 1u << 7
};

struct SourceDeoptAudit {
    SourceDeoptBoundary boundary = SourceDeoptBoundary::UnresolvedBody;
    uint8_t gaps = 0;

    bool missing(SourceDeoptGap gap) const {
        return (gaps & uint8_t(gap)) != 0;
    }

    // This is never an execution grant. Even gaps == 0 would still
    // require the executor to authenticate effects and GC roots.
    bool executable() const { return false; }
};

// RT-001 provides only a passive, typed diagnostic. A verified snapshot
// can discharge the *numeric data present* check, but cannot establish
// that LLVM's SSA is reconstructible at a source-level program counter.
// Keep LiveSsa flagged even if numericComplete is true: the expected
// mapping back to source VM locals has not been implemented.
template<class DetachedCapture>
SourceDeoptAudit auditSourceDeopt(
    SourceResumePhase phase,
    const DetachedCapture* detached,
    const SourceTypedSsaManifest& compilerManifest) {
    SourceDeoptAudit audit;
    audit.boundary = sourceDeoptBoundary(phase);
    const auto needs = sourceDeoptRequirements(audit.boundary);
    if (needs.needs(SourceDeoptNeed::EffectContinuity))
        audit.gaps |= uint8_t(SourceDeoptGap::EffectCommit);
    if (needs.needs(SourceDeoptNeed::LiveSsaValues)) {
        // Even an exact match for known generated string handles
        // does not prove every source local and native SSA root.
        audit.gaps |= uint8_t(SourceDeoptGap::LiveSsa) |
                      uint8_t(SourceDeoptGap::GcRootContinuity);
    }
    if (needs.needs(SourceDeoptNeed::ExactStatementPc))
        audit.gaps |= uint8_t(SourceDeoptGap::ExactStatement);
    if (needs.needs(SourceDeoptNeed::ActiveCallee))
        audit.gaps |= uint8_t(SourceDeoptGap::ActiveCallee);

    if (!detached || !verifyDetachedTypedSsa(*detached, compilerManifest)) {
        audit.gaps |= uint8_t(SourceDeoptGap::InvalidSnapshot);
        return audit;
    }
    if (detached->unresolvedPointers != 0)
        audit.gaps |= uint8_t(SourceDeoptGap::UnresolvedPointers);
    return audit;
}

// Correlate the passive effect frontier with SSA source identity and a
// separately observed host-side commit counter. Even a matching frontier
// does not clear EffectCommit, since a tracker cannot prove a real effect
// happened or that replay boundaries are equivalent to Scratch blocks.
template<class DetachedCapture>
SourceDeoptAudit auditSourceDeoptWithEffectFrontier(
    SourceResumePhase phase,
    const DetachedCapture* detached,
    const SourceTypedSsaManifest& compilerManifest,
    const SourceEffectFrontier* observed,
    const SourceEffectFrontier& expected,
    uint64_t hostCommittedCount) {
    auto audit = auditSourceDeopt(phase, detached, compilerManifest);
    if (!observed || !observed->matches(expected, hostCommittedCount) ||
        expected.source.revision != compilerManifest.revision ||
        expected.source.digestLo != compilerManifest.digestLo ||
        expected.source.digestHi != compilerManifest.digestHi ||
        expected.source.procedureId != compilerManifest.procedureId ||
        expected.resumePc != compilerManifest.resumePc ||
        expected.siteId != compilerManifest.siteId ||
        expected.phase != phase) {
        audit.gaps |= uint8_t(SourceDeoptGap::EffectFrontierMismatch);
    }
    return audit;
}

} // namespace xyo::ir
