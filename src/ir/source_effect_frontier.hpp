// IR-001: source effect-frontier diagnostics; not a transaction manager.
// Recording commit() requires the caller to have already completed the
// observable effect. This ledger cannot attest delivery or authorize deopt.
#pragma once

#include "source_resume_phase.hpp"
#include <cstdint>
#include <limits>
#include <optional>

namespace xyo::ir {

struct SourceEffectIdentity {
    uint64_t revision = 0;
    uint64_t digestLo = 0;
    uint64_t digestHi = 0;
    uint64_t procedureId = 0;

    constexpr bool valid() const {
        return revision != 0 && (digestLo != 0 || digestHi != 0);
    }
    constexpr bool operator==(const SourceEffectIdentity&) const = default;
};

struct SourceEffectFrontier {
    SourceEffectIdentity source;
    uint64_t siteId = 0;
    uint64_t resumePc = 0;
    uint64_t committedCount = 0;
    SourceResumePhase phase = SourceResumePhase::UnknownBody;

    constexpr bool valid() const {
        return source.valid() && siteId != 0 && resumePc != 0 &&
               validSourceResumePhase(static_cast<uint64_t>(phase));
    }

    // observedCount must come from a separate host-side effect journal.
    // Equality describes consistent observations, NOT effect delivery proof.
    constexpr bool matches(const SourceEffectFrontier& expected,
                           uint64_t observedCount) const {
        return valid() && expected.valid() &&
               source == expected.source && siteId == expected.siteId &&
               resumePc == expected.resumePc &&
               phase == expected.phase &&
               committedCount == expected.committedCount &&
               committedCount == observedCount;
    }
};

class SourceEffectSequenceTracker {
public:
    explicit SourceEffectSequenceTracker(SourceEffectIdentity source)
        : source_(source) {}

    // Token allocates the next dynamic effect sequence; repeated visits
    // to the same source PC are distinct. No effect is performed here.
    std::optional<uint64_t> begin(uint64_t effectPc) {
        if (!source_.valid() || !effectPc || pending_ ||
            committed_ == std::numeric_limits<uint64_t>::max())
            return std::nullopt;
        pending_ = Pending{effectPc, committed_ + 1};
        return pending_->sequence;
    }

    // Must be called only AFTER the host-side effect has committed.
    // A duplicate, skipped or cross-source commit is never accepted.
    bool commit(uint64_t effectPc, uint64_t sequence) {
        if (!pending_ || effectPc != pending_->effectPc ||
            sequence != pending_->sequence)
            return false;
        committed_ = sequence;
        pending_.reset();
        return true;
    }

    // Only for a definitively UNPUBLISHED effect (e.g. a failed
    // CommitOnceEffects::Slice::commit). Never cancel after publication:
    // delivery may have occurred or a pending rooted event may remain.
    bool cancelUnpublished(uint64_t effectPc, uint64_t sequence) {
        if (!pending_ || pending_->effectPc != effectPc ||
            pending_->sequence != sequence)
            return false;
        pending_.reset();
        return true;
    }

    // No checkpoint may claim an effect frontier mid-primitive.
    std::optional<SourceEffectFrontier> checkpoint(
        uint64_t siteId, uint64_t resumePc, SourceResumePhase phase) const {
        SourceEffectFrontier result{source_, siteId, resumePc, committed_, phase};
        if (pending_ || !result.valid())
            return std::nullopt;
        return result;
    }

    bool hasPendingEffect() const { return pending_.has_value(); }
    uint64_t committedCount() const { return committed_; }

private:
    struct Pending {
        uint64_t effectPc;
        uint64_t sequence;
    };
    SourceEffectIdentity source_;
    uint64_t committed_ = 0;
    std::optional<Pending> pending_;
};

} // namespace xyo::ir
