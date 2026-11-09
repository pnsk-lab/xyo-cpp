// Scheduler-owned, commit-once journal for observable Scratch host effects.
//
// Generated LLVM/Binaryen code must not directly emit an observable action
// during a speculative slice. The action is staged with GC-rooted payload,
// validated against its scheduler revision, and dispatched only *after*
// publication of the corresponding Scratch state/continuation.
//
// This is an infrastructure ABI, not automatic lowering of effects into
// either backend. The scheduler must supply monotonically increasing slice
// IDs and commit state atomically at a quiescent point. Dispatchers are
// synchronous, non-yielding and noexcept: at-most-once delivery does not
// imply recovery after a process crash or a failing external device.
#pragma once

#include "gc_value_handles.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace xyo::ir {

enum class ObservableOp : uint8_t {
    PenStamp = 1,
    Broadcast = 2,
    CreateClone = 3,
    SoundStart = 4,
    Say = 5,
};

constexpr bool supportsDeferredEffect(ObservableOp op) {
    switch (op) {
    case ObservableOp::PenStamp:
    case ObservableOp::Broadcast:
    case ObservableOp::CreateClone:
    case ObservableOp::SoundStart:
    case ObservableOp::Say:
        return true;
    }
    return false;
}

class CommitOnceEffects final {
public:
    static constexpr std::size_t kMaxPerSlice = 256;
    static constexpr std::size_t kMaxPending = 4096;

    struct DrainResult {
        std::size_t delivered = 0;
        std::size_t invalid = 0;
    };

private:
    struct Pending {
        ObservableOp op;
        std::string blockId;
        RootedValueHandle payload;
    };

public:
    class Slice final {
    public:
        Slice(const Slice&) = delete;
        Slice& operator=(const Slice&) = delete;

        Slice(Slice&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)),
              sliceId_(other.sliceId_),
              baseRevision_(other.baseRevision_),
              staged_(std::move(other.staged_)) {}

        Slice& operator=(Slice&&) = delete;
        ~Slice() { discard(); }

        // Value is borrowed from the current interpreter/compiled stack.
        // Root it immediately. A failed stage leaves the journal unchanged.
        bool stage(ObservableOp op, std::string blockId, Value payload) {
            if (!owner_ || !sliceId_ || blockId.empty() ||
                !supportsDeferredEffect(op) ||
                staged_.size() >= kMaxPerSlice)
                return false;
            const auto handle = owner_->roots_.retain(payload);
            if (!handle) return false;
            staged_.push_back({op, std::move(blockId), *handle});
            return true;
        }

        // commitState must first validate caller-owned slots/CallFrame and
        // publish them as one scheduler-owned transition; it must never
        // dispatch any external effects. A false result keeps observables
        // invisible and drops all speculative roots.
        template <typename CommitState>
        bool commit(CommitState&& commitState) {
            if (!owner_ || !sliceId_ ||
                baseRevision_ != owner_->revision_ ||
                owner_->committing_ || owner_->draining_ ||
                sliceId_ <= owner_->lastPublishedSlice_ ||
                owner_->revision_ == std::numeric_limits<uint64_t>::max() ||
                staged_.size() > kMaxPending - owner_->pending_.size()) {
                discard();
                return false;
            }
            // Prevent reentrant publication while commitState executes.
            // Without this guard a nested commit could advance revision and
            // lastPublishedSlice, then the outer commit could incorrectly
            // overwrite them with an earlier slice ID.
            owner_->committing_ = true;
            const bool published =
                std::invoke(std::forward<CommitState>(commitState));
            owner_->committing_ = false;
            if (!published) {
                discard();
                return false;
            }
            for (auto& record : staged_)
                owner_->pending_.push_back(std::move(record));
            staged_.clear(); // root ownership transferred to pending_
            ++owner_->revision_;
            owner_->lastPublishedSlice_ = sliceId_;
            owner_ = nullptr;
            return true;
        }

        void discard() {
            if (!owner_) return;
            for (const auto& record : staged_)
                owner_->roots_.release(record.payload);
            staged_.clear();
            owner_ = nullptr;
        }

        std::size_t stagedCount() const { return staged_.size(); }

    private:
        friend class CommitOnceEffects;

        Slice(CommitOnceEffects& owner, uint64_t sliceId)
            : owner_(&owner), sliceId_(sliceId),
              baseRevision_(owner.revision_) {}

        CommitOnceEffects* owner_;
        uint64_t sliceId_;
        uint64_t baseRevision_;
        std::vector<Pending> staged_;
    };

    explicit CommitOnceEffects(Gc& gc = Gc::get()) : roots_(gc) {}
    CommitOnceEffects(const CommitOnceEffects&) = delete;
    CommitOnceEffects& operator=(const CommitOnceEffects&) = delete;

    ~CommitOnceEffects() {
        // After scheduler shutdown, no queued external event may fire.
        while (!pending_.empty()) {
            roots_.release(pending_.front().payload);
            pending_.pop_front();
        }
    }

    Slice begin(uint64_t schedulerSliceId) {
        return Slice(*this, schedulerSliceId);
    }

    // Remove the event from the queue BEFORE dispatch. This ensures that
    // reentrant dispatch/drain cannot double-emit an action.
    template <typename Dispatcher>
    DrainResult drain(Dispatcher&& dispatcher) {
        static_assert(std::is_nothrow_invocable_v<
            Dispatcher&, ObservableOp, const std::string&, Value>,
            "Observable effect dispatcher must be non-yielding and noexcept");
        DrainResult result;
        // A dispatcher can indirectly invoke drain() again. Let the outer
        // drain finish in FIFO order, and never dispatch before the current
        // scheduler commitState callback has returned.
        if (draining_ || committing_) return result;
        draining_ = true;
        while (!pending_.empty()) {
            Pending event = std::move(pending_.front());
            pending_.pop_front();
            const auto payload = roots_.resolve(event.payload);
            if (payload) {
                std::invoke(dispatcher, event.op, event.blockId, *payload);
                ++result.delivered;
            } else {
                // A broken token is a surfaced error, never a fallback
                // that would re-execute a possibly committed Scratch block.
                ++result.invalid;
            }
            roots_.release(event.payload);
        }
        draining_ = false;
        return result;
    }

    bool committing() const { return committing_; }
    bool draining() const { return draining_; }
    uint64_t revision() const { return revision_; }
    uint64_t lastPublishedSlice() const { return lastPublishedSlice_; }
    std::size_t pendingCount() const { return pending_.size(); }
    std::size_t rootedPayloads() const { return roots_.size(); }

private:
    GcValueHandles roots_;
    std::deque<Pending> pending_;
    uint64_t revision_ = 0;
    uint64_t lastPublishedSlice_ = 0;
    bool committing_ = false;
    bool draining_ = false;
};

} // namespace xyo::ir
