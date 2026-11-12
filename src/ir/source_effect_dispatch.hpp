// IR-001 scoped effect attribution for real synchronous host journals.
// Temporary scope exists ONLY inside an instrumented compiled primitive.
// No pointers to LLVM, fibers, GC values or Thread escape the scope.
#pragma once

#include "commit_once_effects.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace xyo::ir {

// Scheduler/host-journal observation taken at the end of a single
// synchronous primitive, then compared again at its source checkpoint.
// A different epoch means that OTHER effects may have intervened.
struct SourceEffectHostEpoch {
    uint64_t journalSlice = 0;
    uint64_t journalRevision = 0;
    uint64_t immediateBroadcasts = 0;
    uint64_t immediateEffects = 0;
    std::size_t pending = 0;
    std::size_t rooted = 0;
    bool journalActive = false;
    uint64_t gcGeneration = 0;  // completed GC must not intervene
    bool gcCollectionPending = false; // deferred explicit GC not drained

    bool quiescent() const {
        return journalActive && journalSlice != 0 &&
               journalRevision != 0 && gcGeneration != 0 &&
               !gcCollectionPending && pending == 0 && rooted == 0;
    }
    bool operator==(const SourceEffectHostEpoch&) const = default;
};

struct SourceEffectDispatchObservation {
    std::string blockId;
    uint64_t sourcePc = 0;
    uint64_t sourceRevision = 0;
    uint64_t digestLo = 0;
    uint64_t digestHi = 0;
    uint64_t sliceId = 0;
    uint64_t journalRevision = 0;
    ObservableOp operation = ObservableOp::Broadcast;
    uint32_t published = 0;
    uint32_t delivered = 0;
    uint32_t immediate = 0;
    bool claimed = false;
    bool mismatch = false;
    SourceEffectHostEpoch hostEpoch;

    bool exactDelivery() const {
        return claimed && published == 1 && delivered == 1 &&
               immediate == 0 && !mismatch && sourcePc != 0 &&
               sourceRevision != 0 && (digestLo != 0 || digestHi != 0) &&
               !blockId.empty() && sliceId != 0 && journalRevision != 0 &&
               hostEpoch.quiescent() &&
               hostEpoch.journalSlice == sliceId &&
               hostEpoch.journalRevision == journalRevision;
    }
};

inline thread_local SourceEffectDispatchObservation* activeSourceEffect = nullptr;

// The synchronous host delivery can itself execute interpreted Scratch
// receivers (or nested LLVM helpers). That write occurs BEFORE the
// outer compiled primitive captures its global mutation epoch, and
// would otherwise be invisible when the interval begins. Refuse exact
// delivery attribution even when the journal dispatch succeeds.
inline void noteSourceEffectUnjournaledMutation() {
    if (activeSourceEffect) activeSourceEffect->mismatch = true;
}

class ScopedSourceEffectDispatch final {
public:
    explicit ScopedSourceEffectDispatch(
        SourceEffectDispatchObservation& source)
        : previous_(std::exchange(activeSourceEffect, &source)) {
        // A synchronous nested observable inside an existing compiled
        // primitive means that primitive's effects are not isolated.
        // Keep both attribution scopes, but never certify the parent.
        if (previous_) previous_->mismatch = true;
    }
    ScopedSourceEffectDispatch(const ScopedSourceEffectDispatch&) = delete;
    ScopedSourceEffectDispatch& operator=(
        const ScopedSourceEffectDispatch&) = delete;
    ~ScopedSourceEffectDispatch() { activeSourceEffect = previous_; }

private:
    SourceEffectDispatchObservation* previous_;
};

// Each compiled primitive may claim at most one host event. Nested
// uninstrumented helpers cannot be misattributed to the outer source.
inline std::string sourceEffectStageLabel(
    ObservableOp operation, std::string_view fallback) {
    auto* active = activeSourceEffect;
    if (!active) return std::string(fallback);
    if (active->claimed || active->operation != operation) {
        // An uninstrumented nested observable can otherwise claim the
        // wrapper's one allowed source label, generating a false match.
        active->mismatch = true;
        return std::string(fallback);
    }
    active->claimed = true;
    return active->blockId;
}

inline void noteSourceEffectPublished(
    ObservableOp operation, std::string_view blockId,
    uint64_t sliceId, uint64_t journalRevision) {
    auto* active = activeSourceEffect;
    if (!active) return;
    if (!active->claimed || active->operation != operation ||
        blockId != active->blockId || active->published != 0) {
        active->mismatch = true;
        return;
    }
    active->sliceId = sliceId;
    active->journalRevision = journalRevision;
    ++active->published;
}

inline void noteSourceEffectDelivered(
    ObservableOp operation, std::string_view blockId) {
    auto* active = activeSourceEffect;
    if (!active) return;
    if (!active->claimed || active->operation != operation ||
        blockId != active->blockId) {
        active->mismatch = true;
        return;
    }
    ++active->delivered;
    if (active->delivered > 1) active->mismatch = true;
}

inline void noteSourceEffectImmediate(ObservableOp operation) {
    auto* active = activeSourceEffect;
    if (!active) return;
    if (active->claimed || active->operation != operation)
        active->mismatch = true;
    active->claimed = true;
    ++active->immediate;
}

} // namespace xyo::ir
