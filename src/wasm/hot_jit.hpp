// Browser HotSpot WebAssembly execution with transactional variable slots.
// Sequential top-level loops may resume one iteration per scheduler pass.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace xyo {
class Thread;
namespace wasm {
struct HotCode {
    int handle = -1;
    std::vector<std::string> slotIds;
    // Read-only scalar slots may legally contain strings. Only slots written
    // by the compiled program must be numeric and committed back to Scratch.
    // 0 = read-only, 1 = numeric write, 2 = exact tagged Value write.
    std::vector<uint8_t> writableSlots;
    std::vector<std::string> listIds;
    std::vector<std::u16string> stringLiterals;
    // Host string conversion/concatenation may allocate GC-managed SStr
    // objects. Keep speculative roots alive until commit/rollback.
    bool usesStringHost = false;
    // Restricted scalar-only, numeric-output statement ranges can bypass
    // the general list/GC/deopt journal: WASM edits only private slot bits,
    // and the host publishes those bits after a successful return.
    bool fastNumeric = false;
    // Unique across nested code objects owned by the sequencer. Needed to
    // prevent a recycled HotCode allocation from matching stale bindings.
    uint64_t nestedIdentity = 0;
    bool nestedWarpRepeat = false;
    bool nestedWarpTree = false;
    bool nestedWarpTailRecursion = false;
    bool nestedWarpReturnStack = false;
    bool nestedWarpPcStack = false;
    uint32_t nestedFrameWords = 0;
    struct Loop {
        enum class Kind : uint8_t { Repeat, Until, While, Forever, ForEach };
        Kind kind = Kind::Repeat;
        int bodyHandle = -1;
        int conditionHandle = -1;  // until/while only
        // Straight-line statements between this loop and the next one
        // (or the final suffix), optionally capturing next repeat's count.
        int suffixHandle = -1;
        std::string blockId;
        std::string suffixId;
        uint32_t variableSlot = 0;  // for-each induction variable, when used
    };
    std::vector<Loop> loops;
    // A hybrid script contains ordered, non-overlapping top-level regions.
    // A null code region executes through the exact Scratch interpreter.
    // Each successful WASM region commits transactionally, then transfers
    // its continuation to the next region. This never rewrites block graphs.
    struct Region {
        std::string startId;
        std::string pastEndId;  // empty means end of script
        std::shared_ptr<HotCode> code;
    };
    std::vector<Region> hybridRegions;
    HotCode(int handle, std::vector<std::string> slotIds);
    ~HotCode();
    HotCode(const HotCode&) = delete;
    HotCode& operator=(const HotCode&) = delete;
};

// Each invocation commits only on successful completion of its current
// WASM segment. Yielded runs one non-warp loop iteration; Fallback restores
// the interpreter at the exact loop/suffix continuation.
enum class HotResult : uint8_t { Completed, Yielded, Fallback };
HotResult executeHot(const HotCode& code, Thread& thread);
// Execute one pure compiled region of a hybrid script; the caller owns
// interpreter handoff, continuation and frame state.
HotResult executeHybridRegion(const HotCode& region, Thread& thread);
// Interpreter-owned nested body fast path for scalar numeric statements.
// Uses private scratch slots, then publishes only after all guards succeed.
HotResult executeNestedNumeric(const HotCode& region, Thread& thread);
} // namespace wasm
} // namespace xyo
