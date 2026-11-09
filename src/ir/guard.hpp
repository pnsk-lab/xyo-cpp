// Backend-independent speculative guards and failure policy.
//
// A guard executes before side effects belonging to its speculative region.
// LLVM lowers a failed guard to the original Scratch path; standalone WASM
// returns a bailout status to its caller. This API deliberately does not
// assume that a failed guard is safe to resume after partial writes.
#pragma once

#include "deopt.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace xyo::ir {

enum class GuardReason : uint8_t {
    NumericValue,
    IntegralRange,
    LoopCount,
    OrderedComparison,
};

enum class GuardFailure : uint8_t {
    Fallback, // Continue through an unoptimized native Scratch path.
    Bail,     // Abort WASM execution; caller handles the failure.
};

struct Guard {
    GuardReason reason;
    GuardFailure failure;
};

// Backend emits the control-flow check, and must ensure that the fast path
// is the active insertion point afterwards. The predicate is backend-native
// (LLVM i1, WASM stack i32, or a reference interpreter boolean).
template <typename Backend, typename Predicate>
void lowerGuard(Backend& backend, const Guard& guard, Predicate&& predicate) {
    backend.require(std::forward<Predicate>(predicate), guard);
}

// Runtime counterpart of lowerGuard. The result separates whether the
// predicate passed from whether restoring an aborted snapshot succeeded.
struct GuardCheck {
    bool passed = false;
    bool restored = false;
};

inline GuardCheck checkSpeculativeGuard(bool condition, const Guard& guard,
                                       const DeoptSnapshot& checkpoint,
                                       uint64_t* staged, size_t count,
                                       ResumePoint* resume) {
    if (condition) return {true, false};
    if (guard.failure != GuardFailure::Bail &&
        guard.failure != GuardFailure::Fallback)
        return {false, false};
    return {false, checkpoint.restore(staged, count, resume)};
}

} // namespace xyo::ir
