// Shared LLVM / WebAssembly execution ABI for the portable Scratch CFG.
// Both backends emit the same contract; callers own transactional snapshots
// and scheduler checkpoints. This header has no LLVM, Binaryen or GC types.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace xyo::ir {

enum class NumericRunStatus : uint64_t {
    Completed = 0,
    Yielded = 1,
    Failed = 2,
};

// The generated function takes two pointers to independent, caller-owned
// arrays of 64-bit words:
//   run(uint64_t* stagedScratchSlots, uint64_t* stagedFrame) -> uint64_t
//   frame[0] = resume CFG block ID (0 at entry; UINT64_MAX when done)
//   frame[1] = final, raw Scratch Value bits (valid on completion only)
//   frame[2 + SSA id] = exact raw SSA bits (i1/i32 extended to 64 bits)
// Yielded and Completed are commit candidates; Failed is never committed.
// The host must check variables and binding generations before publication.
struct NumericExecutionAbi {
    static constexpr uint32_t kPcWord = 0;
    static constexpr uint32_t kResultWord = 1;
    static constexpr uint32_t kValuesWord = 2;
    static constexpr uint32_t kWordBytes = sizeof(uint64_t);
    static constexpr uint64_t kInitialPc = 0;
    static constexpr uint64_t kCompletedPc =
        std::numeric_limits<uint64_t>::max();

    static constexpr uint32_t valueWord(uint32_t id) {
        return kValuesWord + id;
    }
    static constexpr uint32_t byteOffset(uint32_t word) {
        return word * kWordBytes;
    }
    static constexpr std::size_t frameWords(std::size_t definitions) {
        return kValuesWord + definitions;
    }
    static constexpr uint64_t status(NumericRunStatus result) {
        return static_cast<uint64_t>(result);
    }
    static constexpr bool isCommitCandidate(uint64_t code) {
        return code == status(NumericRunStatus::Completed) ||
               code == status(NumericRunStatus::Yielded);
    }
};

// Scheduler-owned procedure continuation frame, shared across the LLVM
// and WASM backends. All offsets refer to 64-bit words in a private,
// aligned, contiguous numeric frame arena. No raw Block*, Variable*,
// GC object pointer or host-side std::vector object enters JIT memory.
//
// Every compiled source snapshot assigns dense procedure IDs through its
// captured ScratchTransferGraph; 0 is the top-level script. returnPc is
// the *caller* continuation; resumePc is the next CFG block in this frame.
// A root frame has returnPc == kNoPc. Local slots carry SSA bits and are
// preserved on yield and restored when a child returns.
struct CallFrameAbi {
    static constexpr uint32_t kProcedureIdWord = 0;
    static constexpr uint32_t kReturnPcWord = 1;
    static constexpr uint32_t kResumePcWord = 2;
    static constexpr uint32_t kArgumentCountWord = 3;
    static constexpr uint32_t kLocalCountWord = 4;
    static constexpr uint32_t kHeaderWords = 5;
    static constexpr uint32_t kWordBytes = NumericExecutionAbi::kWordBytes;
    static constexpr uint64_t kRootProcedure = 0;
    static constexpr uint64_t kNoPc = NumericExecutionAbi::kCompletedPc;
    static constexpr uint32_t kMaxDepth = 1024;
    static constexpr uint32_t kMaxArguments = 32;
    static constexpr uint32_t kMaxLocals = 4096;
    // First optional source-loop SSA local. Further locals are assigned to
    // lexical Scratch loop IDs in stable block-ID order. A counter stores
    // the numeric value BEFORE the compiler's post-yield decrement;
    // uncounted/unsupported loop counters use kNoLoopCounter.
    static constexpr uint32_t kLoopCounterLocal = 0;
    static constexpr uint64_t kNoLoopCounter =
        std::numeric_limits<uint64_t>::max();

    static constexpr uint32_t argumentWord(uint32_t index) {
        return kHeaderWords + index;
    }
    static constexpr uint32_t localWord(uint32_t argumentCapacity,
                                        uint32_t index) {
        return kHeaderWords + argumentCapacity + index;
    }
    static constexpr uint32_t frameWords(uint32_t argumentCapacity,
                                         uint32_t localCapacity) {
        return kHeaderWords + argumentCapacity + localCapacity;
    }
    static constexpr uint32_t byteOffset(uint32_t word) {
        return word * kWordBytes;
    }
};

static_assert(CallFrameAbi::frameWords(2, 4) == 11);
static_assert(CallFrameAbi::localWord(2, 0) == 7);
static_assert(CallFrameAbi::byteOffset(CallFrameAbi::kReturnPcWord) == 8);

// Synchronous atomic warp regions use a different ABI:
//     f(uint64_t* stagedSlots) -> i32 (0=completed, 1=rollback)
// A rollback here is NOT the scheduler's Yielded status. Keep its contract
// distinct to prevent incorrectly committing a speculative warp fragment.
enum class AtomicWarpStatus : int32_t {
    Completed = 0,
    Rollback = 1,
};

struct AtomicWarpExecutionAbi {
    // Shared WASM/C++ buffer layout for the guarded warp-only path.
    // The Scratch scalars are copied from live variables; the next three
    // words belong to legacy numerical host helpers, and frame words
    // belong exclusively to the generated multi-PC numeric recurrence.
    static constexpr uint32_t kHelperScratchWords = 3;
    static constexpr uint32_t kMaxRecursionFrames = 1024;
    static constexpr uint32_t kMaxNumericArguments = 2;
    static constexpr int32_t status(AtomicWarpStatus result) {
        return static_cast<int32_t>(result);
    }
    static constexpr std::size_t frameWords(std::size_t numericArguments) {
        // All native/WASM procedure frames use the common header even
        // when a restricted warp self-recursion needs no SSA locals.
        return kMaxRecursionFrames *
               CallFrameAbi::frameWords(uint32_t(numericArguments), 0);
    }
    static constexpr std::size_t bufferWords(
        std::size_t scalarSlots, std::size_t numericArguments) {
        return scalarSlots + kHelperScratchWords +
               frameWords(numericArguments);
    }
    static constexpr uint32_t frameOffsetBytes(uint32_t scalarSlots) {
        return (scalarSlots + kHelperScratchWords) *
               NumericExecutionAbi::kWordBytes;
    }
};

static_assert(NumericExecutionAbi::kWordBytes == 8);
static_assert(NumericExecutionAbi::frameWords(0) == 2);
static_assert(NumericExecutionAbi::byteOffset(
    NumericExecutionAbi::kResultWord) == 8);

} // namespace xyo::ir
