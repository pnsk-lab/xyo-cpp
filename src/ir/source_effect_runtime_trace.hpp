// IR-001 scheduler-thread-owned source-effect diagnostics. No GC pointers.
// The actual host journal callback, not global counter deltas, supplies
// delivery provenance. This is deliberately partial diagnostic evidence.
#pragma once

#include "source_effect_dispatch.hpp"
#include "source_effect_frontier.hpp"

#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace xyo::ir {

struct SourceEffectDeliveredSite {
    SourceEffectIdentity identity;
    std::string blockId;
    uint64_t sourcePc = 0;
    uint64_t journalSlice = 0;
    uint64_t journalRevision = 0;
    ObservableOp operation = ObservableOp::Broadcast;
    uint64_t sequence = 0;
};

class NativeSourceEffectTimeline final {
public:
    bool observe(const SourceEffectDispatchObservation& observation,
                 bool sourceMatches) {
        // Unrelated/incomplete/immediate events invalidate any checkpoint.
        lastCheckpoint_.reset();
        if (!sourceMatches || !observation.exactDelivery() ||
            count_ == std::numeric_limits<uint64_t>::max() ||
            (count_ && (identity_.revision != observation.sourceRevision ||
                        identity_.digestLo != observation.digestLo ||
                        identity_.digestHi != observation.digestHi ||
                        // Gaps imply another, potentially untraced,
                        // host event between our attributed effects.
                        observation.sliceId != lastSlice_ + 1 ||
                        observation.journalRevision != lastRevision_ + 1 ||
                        observation.hostEpoch.immediateBroadcasts !=
                            lastHostEpoch_.immediateBroadcasts ||
                        observation.hostEpoch.immediateEffects !=
                            lastHostEpoch_.immediateEffects))) {
            valid_ = false;
            return false;
        }
        if (!valid_ || pendingUnjournaledMutation_) {
            valid_ = false;
            return false;
        }
        checkpointedSinceLastObservation_ = false;
        if (!count_)
            identity_ = {observation.sourceRevision, observation.digestLo,
                         observation.digestHi, 0};
        ++count_;
        lastSlice_ = observation.sliceId;
        lastRevision_ = observation.journalRevision;
        lastHostEpoch_ = observation.hostEpoch;
        lastDelivery_ = SourceEffectDeliveredSite{
            identity_, observation.blockId, observation.sourcePc,
            observation.sliceId, observation.journalRevision,
            observation.operation, count_};
        return true;
    }

    // Only source statements on the same scheduler Thread are
    // instrumented. A mutation AFTER a delivered event but BEFORE the
    // next checkpoint invalidates the current evidence. Writes made
    // after a verified checkpoint and before the next observed event
    // start a fresh diagnostic interval (not a replay proof).
    void noteNonJournalMutation() {
        lastCheckpoint_.reset();
        if (count_ && !checkpointedSinceLastObservation_)
            pendingUnjournaledMutation_ = true;
    }

    // sitePc is the immutable source loop's numeric ID. This does not
    // establish the native machine position or source return semantics.
    bool checkpoint(SourceEffectIdentity source,
                    uint64_t sitePc, SourceResumePhase phase,
                    const SourceEffectHostEpoch& observedHost) {
        lastCheckpoint_.reset();
        if (!valid_ || !count_ || !lastDelivery_ ||
            pendingUnjournaledMutation_ ||
            source != identity_ || sitePc == 0 ||
            phase != SourceResumePhase::AfterLoopBody)
            return false;
        // An uninstrumented observable event or fallback delivery
        // between the last attributed effect and this checkpoint
        // changes the host epoch. Never silently certify stale evidence.
        if (!observedHost.quiescent() || observedHost != lastHostEpoch_) {
            valid_ = false;
            return false;
        }
        SourceEffectFrontier frontier{
            source, sitePc, sitePc, count_, phase};
        if (!frontier.valid()) return false;
        lastCheckpoint_ = frontier;
        checkpointedSinceLastObservation_ = true;
        return true;
    }

    const std::optional<SourceEffectDeliveredSite>& lastDelivery() const {
        return lastDelivery_;
    }
    const std::optional<SourceEffectFrontier>& lastCheckpoint() const {
        return lastCheckpoint_;
    }
    uint64_t count() const { return count_; }
    // Used only while the registry lock is held. A write from another
    // Thread in the same process may invalidate an active interval,
    // but a write after its checkpoint can precede the next interval.
    bool hasUncheckpointedObservation() const {
        return count_ && !checkpointedSinceLastObservation_;
    }
    bool pendingNonJournalMutation() const {
        return pendingUnjournaledMutation_;
    }
    bool valid() const { return valid_; }
    bool executable() const { return false; }

private:
    SourceEffectIdentity identity_;
    std::optional<SourceEffectDeliveredSite> lastDelivery_;
    std::optional<SourceEffectFrontier> lastCheckpoint_;
    uint64_t count_ = 0;
    uint64_t lastSlice_ = 0;
    uint64_t lastRevision_ = 0;
    SourceEffectHostEpoch lastHostEpoch_;
    bool checkpointedSinceLastObservation_ = false;
    bool pendingUnjournaledMutation_ = false;
    bool valid_ = true;
};

// Retains diagnostic Thread-scoped records without changing Thread/JitCtx
// ABI or borrowing LLVM/Fiber state. Pointer keys are erased when Threads
// retire, before their addresses can be reused.
class NativeSourceEffectTimelineRegistry final {
public:
    struct CheckpointObservation {
        bool captured = false;
        bool valid = false;
        uint64_t count = 0;
        bool pendingNonJournalMutation = false;
        std::optional<SourceEffectDeliveredSite> latest;
        std::optional<SourceEffectFrontier> checkpoint;
        bool foreignVmWrite = false;
        bool interpreterVmWrite = false;
    };

    static NativeSourceEffectTimelineRegistry& get() {
        // Thread destructors can run during process shutdown. Maintain
        // registry lifetime to exit; no GC-managed data is retained.
        static auto* registry = new NativeSourceEffectTimelineRegistry;
        return *registry;
    }

    // The snapshot is acquired BEFORE synchronous host delivery. A
    // different OS thread may mutate Scratch during that call; the
    // thread_local source scope alone cannot observe such a write.
    uint64_t mutationEpochSnapshot() {
        const std::lock_guard lock(mutex_);
        return mutationEpoch_;
    }

    bool observe(const void* thread,
                 const SourceEffectDispatchObservation& observation,
                 bool sourceMatches) {
        if (!thread) return false;
        const std::lock_guard lock(mutex_);
        return observeLocked(thread, observation, sourceMatches);
    }

    // Consumes the pre-delivery epoch and registers the observation
    // under ONE registry lock. Completed GC during delivery also
    // invalidates raw native state, even if the post-delivery GC epoch
    // equals the epoch later observed at the checkpoint. Neither
    // check alters the actual host delivery.
    bool observeDuringDelivery(
        const void* thread, SourceEffectDispatchObservation& observation,
        bool sourceMatches, uint64_t beforeMutationEpoch,
        uint64_t beforeGcGeneration) {
        if (!thread) return false;
        const std::lock_guard lock(mutex_);
        if (beforeMutationEpoch == 0 ||
            mutationEpoch_ == std::numeric_limits<uint64_t>::max() ||
            mutationEpoch_ != beforeMutationEpoch ||
            beforeGcGeneration == 0 ||
            beforeGcGeneration != observation.hostEpoch.gcGeneration)
            observation.mismatch = true;
        return observeLocked(thread, observation, sourceMatches);
    }

    CheckpointObservation checkpoint(
        const void* thread, SourceEffectIdentity source,
        uint64_t sitePc, SourceResumePhase phase,
        const SourceEffectHostEpoch& observedHost) {
        const std::lock_guard lock(mutex_);
        const auto it = timelines_.find(thread);
        if (it == timelines_.end()) return {};
        auto& slot = it->second;
        const bool foreignWrite =
            mutationEpoch_ == std::numeric_limits<uint64_t>::max() ||
            slot.mutationEpoch != mutationEpoch_;
        if (foreignWrite) slot.timeline.noteNonJournalMutation();
        const bool interpretedWrite =
            interpreterEpoch_ == std::numeric_limits<uint64_t>::max() ||
            slot.interpreterEpoch != interpreterEpoch_;
        const bool captured = slot.timeline.checkpoint(
            source, sitePc, phase, observedHost);
        return {captured, slot.timeline.valid(), slot.timeline.count(),
                slot.timeline.pendingNonJournalMutation(),
                slot.timeline.lastDelivery(),
                slot.timeline.lastCheckpoint(), foreignWrite,
                interpretedWrite};
    }

    // Avoid creating a timeline for mutations that occurred before
    // any attributed journal event. They cannot be retrospectively
    // claimed as covered by the later diagnostic.
    void noteNonJournalMutation(const void* thread) {
        if (!thread) return;
        const std::lock_guard lock(mutex_);
        recordMutationLocked(thread, false);
    }

    // Interpreter's built-in state-changing primitives use this
    // companion. The global epoch detects a write made on OTHER
    // Scratch Threads even if they have no source-effect timeline.
    void noteInterpreterMutation(const void* thread) {
        if (!thread) return;
        const std::lock_guard lock(mutex_);
        recordMutationLocked(thread, true);
    }

    void erase(const void* thread) {
        const std::lock_guard lock(mutex_);
        timelines_.erase(thread);
    }

    NativeSourceEffectTimelineRegistry(
        const NativeSourceEffectTimelineRegistry&) = delete;
    NativeSourceEffectTimelineRegistry& operator=(
        const NativeSourceEffectTimelineRegistry&) = delete;

private:
    NativeSourceEffectTimelineRegistry() = default;

    struct TimelineSlot {
        NativeSourceEffectTimeline timeline;
        uint64_t mutationEpoch = 0;
        uint64_t interpreterEpoch = 0;
    };

    // Caller holds mutex_ (including observeDuringDelivery).
    bool observeLocked(
        const void* thread,
        const SourceEffectDispatchObservation& observation,
        bool sourceMatches) {
        auto& slot = timelines_[thread];
        if (mutationEpoch_ == std::numeric_limits<uint64_t>::max()) {
            slot.timeline.noteNonJournalMutation();
            return false; // permanently saturated epoch: no ABA proof
        }
        if (slot.timeline.hasUncheckpointedObservation() &&
            slot.mutationEpoch != mutationEpoch_)
            slot.timeline.noteNonJournalMutation();
        const bool recorded =
            slot.timeline.observe(observation, sourceMatches);
        if (recorded) {
            slot.mutationEpoch = mutationEpoch_;
            slot.interpreterEpoch = interpreterEpoch_;
        }
        return recorded;
    }

    void recordMutationLocked(const void* thread, bool interpreted) {
        // A saturating generation never wraps and never becomes a
        // previously accepted diagnostic epoch (no ABA).
        if (mutationEpoch_ != std::numeric_limits<uint64_t>::max())
            ++mutationEpoch_;
        if (interpreted &&
            interpreterEpoch_ != std::numeric_limits<uint64_t>::max())
            ++interpreterEpoch_;
        const auto it = timelines_.find(thread);
        if (it != timelines_.end())
            it->second.timeline.noteNonJournalMutation();
    }

    std::mutex mutex_;
    uint64_t mutationEpoch_ = 1; // guarded by mutex; never resets/ABA
    uint64_t interpreterEpoch_ = 1;
    std::unordered_map<const void*, TimelineSlot> timelines_;
};

} // namespace xyo::ir
