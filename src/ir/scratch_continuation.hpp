// Transactional, caller-scheduled continuation for numeric Scratch CFGs.
//
// Each generated backend slice receives (staged scalar slots*, staged
// continuation frame*). A yield result is published only if all scalar
// guards pass; failed execution or a replaced runtime variable leaves the
// original frame and live Scratch state untouched.
//
// ABI: frame[0] = next CFG block (0 for new execution; UINT64_MAX = done).
//      frame[1] = final raw Scratch Value, set only on completion.
//      frame[2 + ValueId] = raw i64/f64/i1 SSA value bits.
//
// This is NOT itself a scheduler. The caller decides whether a yielded
// thread runs next tick (nonwarp) or is resumed immediately (warp when safe).
// No live Variable* is passed to generated code.
#pragma once

#include "scratch_variable_slots.hpp"
#include "execution_abi.hpp"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <functional>
#include <utility>
#include <vector>

namespace xyo::ir {

enum class NumericSliceResult : uint8_t {
    Failed, Yielded, Completed
};

// Scheduler-visible transaction record. A yield only becomes observable
// after the scalar writes AND the continuation PC have been published.
// An aborted slice never advances the committed frame.
enum class NumericSliceTransition : uint8_t {
    None, YieldCommitted, Completed, RolledBack
};

enum class NumericRollbackReason : uint8_t {
    None, InvalidState, InvalidBindings, BackendFailure,
    InvalidContinuation, CommitConflict, SourceChanged
};

inline constexpr const char* numericRollbackReasonName(
        NumericRollbackReason reason) {
    switch (reason) {
    case NumericRollbackReason::None: return "none";
    case NumericRollbackReason::InvalidState: return "invalid-state";
    case NumericRollbackReason::InvalidBindings: return "invalid-bindings";
    case NumericRollbackReason::BackendFailure: return "backend-failure";
    case NumericRollbackReason::InvalidContinuation: return "invalid-continuation";
    case NumericRollbackReason::CommitConflict: return "commit-conflict";
    case NumericRollbackReason::SourceChanged: return "source-changed";
    }
    return "unknown";
}

class NumericContinuation {
public:
    explicit NumericContinuation(const ScratchNumericKernel& kernel)
        : frame_(NumericExecutionAbi::frameWords(kernel.executable.ssa.definitions.size()), 0),
          stagedFrame_(frame_.size(), 0),
          program_(&kernel.executable),
          verified_(bool(verifyExecutable(kernel.executable))),
          resumeHeaders_(kernel.executable.ssa.cfg.blocks().size(), 0) {
        if (!verified_) return;
        for (const auto& boundary : kernel.executable.yieldBoundaries)
            resumeHeaders_[boundary.header] = 1;
    }

    bool completed() const { return completed_; }
    bool hasCommittedSlice() const { return started_; }
    const std::vector<uint64_t>& frame() const { return frame_; }
    uint64_t resultBits() const { return frame_[NumericExecutionAbi::kResultWord]; }
    NumericSliceTransition lastTransition() const { return lastTransition_; }
    NumericRollbackReason rollbackReason() const { return rollbackReason_; }
    uint64_t committedSlices() const { return committedSlices_; }
    uint64_t rolledBackSlices() const { return rolledBackSlices_; }

    // Scheduler may reject the next slice before invoking generated code,
    // for example after the Scratch block structure changes at a yield.
    // The last committed PC and scalar state remain intact.
    void rejectUnrunSlice(NumericRollbackReason reason) {
        if (reason == NumericRollbackReason::None) return;
        lastTransition_ = NumericSliceTransition::RolledBack;
        rollbackReason_ = reason;
        ++rolledBackSlices_;
    }

private:
    template <typename Runner>
    friend NumericSliceResult resumeNumericKernel(
        Target&, const ScratchNumericKernel&, NumericContinuation&, Runner&&);

    std::vector<uint64_t> frame_;
    // Allocated once per thread. The staged frame is never exposed on a
    // failed slice, and swap() publishes it without an allocation.
    std::vector<uint64_t> stagedFrame_;
    std::unique_ptr<ScratchNumericSlots> slots_;
    Target* owner_ = nullptr;
    const Executable* program_ = nullptr;
    bool verified_ = false;
    std::vector<uint8_t> resumeHeaders_;
    bool started_ = false;
    bool completed_ = false;
    NumericSliceTransition lastTransition_ = NumericSliceTransition::None;
    NumericRollbackReason rollbackReason_ = NumericRollbackReason::None;
    uint64_t committedSlices_ = 0;
    uint64_t rolledBackSlices_ = 0;
};

template <typename Runner>
NumericSliceResult resumeNumericKernel(
    Target& target, const ScratchNumericKernel& kernel,
    NumericContinuation& continuation, Runner&& runner) {
    const Executable& program = kernel.executable;
    const auto rollback = [&](NumericRollbackReason reason) {
        continuation.rejectUnrunSlice(reason);
        return NumericSliceResult::Failed;
    };
    if (!program.resumable || !program.externalValueSlots ||
        !kernel.requiresLoopScheduler || program.yieldBoundaries.empty() ||
        !continuation.verified_ || continuation.completed_ ||
        continuation.program_ != &program ||
        continuation.frame_.size() != NumericExecutionAbi::frameWords(program.ssa.definitions.size()))
        return rollback(NumericRollbackReason::InvalidState);

    const uint64_t pc = continuation.frame_[NumericExecutionAbi::kPcWord];
    if (pc >= program.ssa.cfg.blocks().size())
        return rollback(NumericRollbackReason::InvalidState);
    if (continuation.started_) {
        if (!continuation.resumeHeaders_[pc])
            return rollback(NumericRollbackReason::InvalidState);
    } else if (pc != NumericExecutionAbi::kInitialPc) {
        return rollback(NumericRollbackReason::InvalidState);
    }

    if (!continuation.slots_) {
        continuation.slots_ = std::make_unique<ScratchNumericSlots>(
            target, kernel.slotIds);
        continuation.owner_ = &target;
        if (!continuation.slots_->valid())
            return rollback(NumericRollbackReason::InvalidBindings);
    } else if (continuation.owner_ != &target ||
               !continuation.slots_->refresh()) {
        return rollback(NumericRollbackReason::InvalidBindings);
    }
    auto& staged = *continuation.slots_;
    if (staged.size() != program.variableSlots)
        return rollback(NumericRollbackReason::InvalidBindings);
    // The original and scratch frame arrays stay allocated for the entire
    // thread lifetime; copying fixed-width words replaces per-yield malloc.
    std::copy(continuation.frame_.begin(), continuation.frame_.end(),
              continuation.stagedFrame_.begin());
    auto& nextFrame = continuation.stagedFrame_;
    // 0: completed, 1: checkpoint, 2+: error. The callback MUST return
    // synchronously, with no scheduler yield or observable side effects.
    const uint64_t code = std::forward<Runner>(runner)(
        staged.data(), nextFrame.data());
    if (!NumericExecutionAbi::isCommitCandidate(code))
        return rollback(NumericRollbackReason::BackendFailure);
    if (code == NumericExecutionAbi::status(NumericRunStatus::Completed)) {
        if (nextFrame[NumericExecutionAbi::kPcWord] != NumericExecutionAbi::kCompletedPc ||
            !Value{nextFrame[NumericExecutionAbi::kResultWord]}.isNumber())
            return rollback(NumericRollbackReason::InvalidContinuation);
    } else {
        if (nextFrame[NumericExecutionAbi::kPcWord] >= continuation.resumeHeaders_.size() ||
            !continuation.resumeHeaders_[nextFrame[NumericExecutionAbi::kPcWord]])
            return rollback(NumericRollbackReason::InvalidContinuation);
    }
    // A legal resume PC alone is insufficient: a corrupted induction
    // value would publish writes that cannot be reconstructed on deopt.
    // Validate the same counter/limit contract before either state commits.
    if (code == NumericExecutionAbi::status(NumericRunStatus::Yielded)) {
        for (const auto& boundary : program.yieldBoundaries) {
            if (boundary.header != nextFrame[NumericExecutionAbi::kPcWord])
                continue;
            if (boundary.counterPhi == kNoValue) continue;
            const auto counterWord =
                NumericExecutionAbi::valueWord(boundary.counterPhi);
            const auto limitWord =
                NumericExecutionAbi::valueWord(boundary.limitValue);
            if (counterWord >= nextFrame.size() ||
                (boundary.limitValue != kNoValue && limitWord >= nextFrame.size()))
                return rollback(NumericRollbackReason::InvalidContinuation);
            const uint64_t limit = boundary.limitValue == kNoValue
                ? boundary.repeatCount : nextFrame[limitWord];
            if (nextFrame[counterWord] > limit)
                return rollback(NumericRollbackReason::InvalidContinuation);
        }
    }
    if (!staged.commit())
        return rollback(NumericRollbackReason::CommitConflict);

    continuation.frame_.swap(nextFrame);
    continuation.started_ = true;
    continuation.completed_ = code == NumericExecutionAbi::status(NumericRunStatus::Completed);
    continuation.lastTransition_ = continuation.completed_
        ? NumericSliceTransition::Completed : NumericSliceTransition::YieldCommitted;
    continuation.rollbackReason_ = NumericRollbackReason::None;
    ++continuation.committedSlices_;
    return code == NumericExecutionAbi::status(NumericRunStatus::Completed) ? NumericSliceResult::Completed
                     : NumericSliceResult::Yielded;
}

// Called from the host scheduler after selecting a runnable Scratch thread.
// A regular thread returns to the scheduler on every loop checkpoint. Warp
// mode immediately executes the next slice unless the caller-supplied
// deadline probe decides to yield. The caller owns the real Thread status,
// watchdog, GC roots and final deoptimization behavior.
template <typename Runner, typename WarpDeadlineDue>
NumericSliceResult driveNumericKernel(
    Target& target, const ScratchNumericKernel& kernel,
    NumericContinuation& continuation, bool warpMode,
    Runner&& runner, WarpDeadlineDue&& warpDeadlineDue) {
    for (;;) {
        const NumericSliceResult result = resumeNumericKernel(
            target, kernel, continuation, runner);
        if (result != NumericSliceResult::Yielded) return result;
        if (!warpMode || std::forward<WarpDeadlineDue>(warpDeadlineDue)())
            return result;
    }
}

} // namespace xyo::ir
