#include "hot_jit.hpp"
#include "host_bridge.hpp"
#include "list_host_bridge.hpp"
#include "string_host_bridge.hpp"
#include "../core/gc.hpp"
#include "../ir/guard.hpp"
#include "../ir/execution_abi.hpp"
#include "../engine/thread.hpp"
#include "../engine/runtime.hpp"
#include "../engine/sequencer.hpp"
#include "../model/target.hpp"
#include "../model/variable.hpp"
#include "../core/value.hpp"
#include "../core/jsnum.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <vector>

namespace xyo::wasm {

HotCode::HotCode(int h, std::vector<std::string> ids)
    : handle(h), slotIds(std::move(ids)) {}

HotCode::~HotCode() {
    release(handle);
    for (const auto& loop : loops) {
        release(loop.bodyHandle);
        release(loop.conditionHandle);
        release(loop.suffixHandle);
    }
}

#if defined(__EMSCRIPTEN__)
namespace {

// The interpreter's repeat loop counter is f64. Large counters are left in
// the interpreter rather than requiring millions of separate JS/WASM calls.
constexpr double kMaxWasmRepeat = 1000000.0;

bool invokeSegment(const HotCode& code, int handle, Thread& thread,
                   double* captured = nullptr, bool roundedCount = true,
                   int overrideSlot = -1, double overrideValue = 0) {
    if (!thread.target || handle < 0 || code.slotIds.size() > 4096)
        return false;

    // Resolve variables anew each time, as another Scratch thread may mutate
    // a variable between non-warp loop iterations. Storage is reused but
    // variable pointers are refreshed to avoid stale map references.
    // Stage resolution and clone
    // target ownership are handled by Target::lookupVariableById.
    auto& variables = thread.jitVarSlots;
    variables.clear();
    variables.reserve(code.slotIds.size());
    // One spare slot is used by the prefix to publish the repeat's rounded
    // count. It is NOT a Scratch variable and is never committed to Target.
    auto& slots = thread.wasmSlotBits;
    // [0,n): Scratch values; [n]: repeat/condition result;
    // [n+1,n+2]: exact fmod host-helper operands/results.
    slots.resize(code.slotIds.size() + 3);
    slots[code.slotIds.size()] = Value::num(0).bits;
    for (size_t i = 0; i < code.slotIds.size(); ++i) {
        Variable* var = thread.target->lookupVariableById(code.slotIds[i]);
        // Heap strings may be converted by the host ToNumber helper while
        // still rooted through the authoritative Target::Variable. Other
        // dynamic values remain in the interpreter until their ABI is
        // explicitly supported. The final writeback still requires numbers.
        if (!var || var->type != Variable::kScalarType || var->isCloud) {
            if (std::getenv("XYO_JIT_VERBOSE"))
                std::fprintf(stderr,
                    "xyo-wasm-hot: nonnumeric variable bailout id=%s\n",
                    code.slotIds[i].c_str());
            return false;
        }
        variables.push_back(var);
        slots[i] = var->value.bits;
    }
    std::vector<Variable*> listVariables;
    listVariables.reserve(code.listIds.size());
    for (const auto& id : code.listIds) {
        Variable* var = thread.target->lookupVariableById(id);
        if (!var || var->type != Variable::kListType || var->isCloud ||
            !var->list)
            return false;
        listVariables.push_back(var);
    }
    // The target owns each list. Binding indices are invocation-local; no
    // SList pointer is cached by WASM or retained across scheduler yields.
    NumericListTransaction listTxn(listVariables);
    ScopedListBindings activeLists(listTxn);

    // A string concatenation may produce an SStr that exists only in the
    // speculative WASM slot buffer. GC must not sweep it before successful
    // commit promotes the Value into the Target's rooted scalar storage.
    // Likewise, a late guard bailout discards uncommitted strings safely.
    std::optional<Gc::DeferCollect> deferStringCollection;
    if (code.usesStringHost || !code.listIds.empty())
        deferStringCollection.emplace();
    ScopedStringLiteralBindings activeLiterals(code.stringLiterals);

    // Capture a deoptimization checkpoint before speculative execution.
    // Runtime variables remain authoritative, and no speculative writes are
    // committed until the whole segment returns success.
    ir::DeoptSnapshot checkpoint;
    const ir::ResumePoint resume{
        thread.wasmLoopPhase == 1 ? ir::ResumePhase::LoopBody :
        thread.wasmLoopPhase == 2 ? ir::ResumePhase::LoopSuffix :
                                    ir::ResumePhase::ScriptEntry,
        thread.wasmLoopIndex, thread.wasmLoopRemaining, 0};
    if (!checkpoint.capture(slots.data(), code.slotIds.size(), resume))
        return false;
    auto abortSpeculation = [&](ir::GuardReason reason) -> bool {
        ir::ResumePoint restored;
        const auto result = ir::checkSpeculativeGuard(
            false, {reason, ir::GuardFailure::Bail},
            checkpoint, slots.data(), code.slotIds.size(), &restored);
        if (!result.restored) return false;
        thread.wasmLoopIndex = restored.loopIndex;
        thread.wasmLoopRemaining = restored.loopRemaining;
        thread.wasmLoopPhase =
            restored.phase == ir::ResumePhase::LoopBody ? 1 :
            restored.phase == ir::ResumePhase::LoopSuffix ? 2 : 0;
        return false;
    };

    // ForEach sets its induction variable before evaluating the loop body.
    // Stage the update in the same snapshot as body writes: if the body
    // traps/bails out, neither the induction variable nor partial body
    // effects are committed, and the interpreter resumes at the same index.
    if (overrideSlot >= 0) {
        if (size_t(overrideSlot) >= code.slotIds.size())
            return abortSpeculation(ir::GuardReason::NumericValue);
        slots[overrideSlot] = Value::num(overrideValue).bits;
    }
    const int status = invoke(handle,
        static_cast<int>(reinterpret_cast<uintptr_t>(slots.data())));
    if (std::getenv("XYO_JIT_VERBOSE"))
        std::fprintf(stderr, "xyo-wasm-hot: wasm return=%d\n", status);
    if (status != 0)
        return abortSpeculation(ir::GuardReason::NumericValue);

    for (size_t i = 0; i < variables.size(); ++i) {
        if (i < code.writableSlots.size() && code.writableSlots[i] == 1 &&
            !Value{slots[i]}.isNumber())
            return abortSpeculation(ir::GuardReason::NumericValue);
    }

    if (captured) {
        const Value count = Value{slots[code.slotIds.size()]};
        if (!count.isNumber()) return abortSpeculation(ir::GuardReason::NumericValue);
        double n = count.asNumberUnchecked();
        if (roundedCount) {
            // Scratch's num() maps NaN to zero before calling jsRound.
            if (std::isnan(n) || n < 0) n = 0;
            n = jsnum::jsRound(n);
            if (!std::isfinite(n) || n > kMaxWasmRepeat)
                return abortSpeculation(ir::GuardReason::LoopCount);
        }
        *captured = n;
    }

    listTxn.commit();

    // The complete segment is transactional. Even a late bailout from a
    // nested conditional cannot expose half of the segment's variable writes.
    for (size_t i = 0; i < variables.size(); ++i) {
        if (i >= code.writableSlots.size() || !code.writableSlots[i])
            continue;
        Variable* var = variables[i];
        var->value = Value{slots[i]};
        var->observeValueType(var->value);
    }
    checkpoint.discard();
    if (std::getenv("XYO_JIT_VERBOSE"))
        std::fprintf(stderr, "xyo-wasm-hot: committed\n");
    return true;
}

// Resume an already-partially-committed script without replaying earlier
// statements. The Scratch interpreter's loop counter is stored on the
// repeat block's StackFrame (not on its body frame).
void restoreInterpreterContinuation(const HotCode& code, Thread& thread) {
    if (code.loops.empty() || thread.wasmLoopPhase == 0 ||
        thread.wasmLoopIndex >= code.loops.size()) return;

    const auto& loop = code.loops[thread.wasmLoopIndex];
    const bool inBody = thread.wasmLoopPhase == 1;
    const std::string& next = inBody ? loop.blockId : loop.suffixId;
    if (next.empty()) {
        // The final loop ended and there is no suffix to interpret.
        thread.status = STATUS_DONE;
        thread.stack.clear();
        return;
    }
    if (thread.stack.size() != 1) {
        thread.stack.clear();
        thread.stackFrames.clear();
        thread.pushStack(next);
    } else {
        thread.reuseStackForNextBlock(next);
    }
    StackFrame* frame = thread.peekStackFrame();
    if (inBody) {
        frame->isLoop = true;
        if (loop.kind == HotCode::Loop::Kind::Repeat ||
            loop.kind == HotCode::Loop::Kind::ForEach) {
            frame->executionContext = std::make_unique<ExecContext>();
            if (loop.kind == HotCode::Loop::Kind::Repeat) {
                frame->executionContext->hasLoopCounter = true;
                frame->executionContext->loopCounter = thread.wasmLoopRemaining;
            } else {
                frame->executionContext->hasIndex = true;
                frame->executionContext->index = thread.wasmLoopRemaining;
            }
        }
    }
    thread.interpreterStarted = true;
    if (std::getenv("XYO_JIT_VERBOSE"))
        std::fprintf(stderr,
            "xyo-wasm-hot: resume interp %s remaining=%.0f loop=%u\n",
            inBody ? "repeat" : "suffix", thread.wasmLoopRemaining,
            thread.wasmLoopIndex);
}
} // namespace
#endif

HotResult executeNestedNumeric(const HotCode& code, Thread& thread) {
#if !defined(__EMSCRIPTEN__)
    (void)code;
    (void)thread;
    return HotResult::Fallback;
#else
    if (!code.fastNumeric || code.handle < 0 || thread.osrActive ||
        !code.listIds.empty() || !code.stringLiterals.empty() ||
        code.slotIds.size() > 256 || !thread.target)
        return HotResult::Fallback;
    const auto stamp = thread.target->variableBindingStamp();
    auto& cache = thread.nestedWasmBindings;
    // Unlike a value mutation, a structural variable-map mutation
    // invalidates Variable* pointers. The globally unique generations
    // also prevent clone deletion/recreation from producing an ABA match.
    if (!cache || cache->codeIdentity != code.nestedIdentity ||
        cache->target != thread.target || cache->stage != stamp.stage ||
        cache->localGeneration != stamp.localGeneration ||
        cache->stageGeneration != stamp.stageGeneration ||
        cache->variables.size() != code.slotIds.size()) {
        std::vector<Variable*> resolved;
        resolved.reserve(code.slotIds.size());
        for (const auto& id : code.slotIds) {
            Variable* var = thread.target->lookupVariableById(id);
            if (!var || var->type != Variable::kScalarType || var->isCloud)
                return HotResult::Fallback;
            resolved.push_back(var);
        }
        if (!cache)
            cache = std::make_unique<NestedWasmBindingCache>();
        cache->variables = std::move(resolved);
        cache->codeIdentity = code.nestedIdentity;
        cache->target = thread.target;
        cache->stage = stamp.stage;
        cache->localGeneration = stamp.localGeneration;
        cache->stageGeneration = stamp.stageGeneration;
    }
    auto& variables = cache->variables;
    auto& slots = thread.wasmSlotBits;
    // Host-provided private scratch is contiguous after 3 helper words.
    // Only multi-PC recursion reserves extra words (1024 bounded frames);
    // imports share the WebAssembly runtime memory, so never use an
    // unowned address for the frame stack.
    if (code.nestedFrameWords >
        ir::AtomicWarpExecutionAbi::frameWords(
            ir::AtomicWarpExecutionAbi::kMaxNumericArguments))
        return HotResult::Fallback;
    slots.resize(code.slotIds.size() +
                 ir::AtomicWarpExecutionAbi::kHelperScratchWords +
                 code.nestedFrameWords);
    for (size_t i = 0; i < code.slotIds.size(); ++i) {
        Variable* var = variables[i];
        if (var->type != Variable::kScalarType || var->isCloud)
            return HotResult::Fallback;
        slots[i] = var->value.bits;
    }
    slots[code.slotIds.size()] = Value::num(0).bits;
    // All input slots are copies. A WASM trap/deopt cannot publish
    // partially computed values, and the restricted IR contains no
    // list, string creation or effectful reporter instructions.
    if (invoke(code.handle,
            static_cast<int>(reinterpret_cast<uintptr_t>(slots.data()))) != 0)
        return HotResult::Fallback;
    // A pure warp repeat can be batched only while it stays inside the
    // original per-warp scheduling budget. If an unexpectedly expensive
    // math reporter exhausts the deadline, discard this private slot
    // transaction and let the interpreter resume at the same repeat.
    // No externally visible writes have happened yet.
    if (code.nestedWarpRepeat && thread.target->runtime &&
        thread.target->runtime->sequencer &&
        thread.target->runtime->sequencer->warpBudgetExhausted(thread))
        return HotResult::Fallback;
    for (size_t i = 0; i < code.slotIds.size(); ++i)
        if (i < code.writableSlots.size() && code.writableSlots[i] &&
            !Value{slots[i]}.isNumber())
            return HotResult::Fallback;
    for (size_t i = 0; i < code.slotIds.size(); ++i) {
        if (i >= code.writableSlots.size() || !code.writableSlots[i])
            continue;
        Variable* var = variables[i];
        var->value = Value{slots[i]};
        var->observeValueType(var->value);
    }
    return HotResult::Completed;
#endif
}

HotResult executeHybridRegion(const HotCode& region, Thread& thread) {
#if !defined(__EMSCRIPTEN__)
    (void)region; (void)thread;
    return HotResult::Fallback;
#else
    if (region.handle < 0 || !region.hybridRegions.empty() ||
        !region.loops.empty() || thread.osrActive)
        return HotResult::Fallback;
    return invokeSegment(region, region.handle, thread)
        ? HotResult::Completed : HotResult::Fallback;
#endif
}

HotResult executeHot(const HotCode& code, Thread& thread) {
#if !defined(__EMSCRIPTEN__)
    (void)code; (void)thread;
    return HotResult::Fallback;
#else
    if (std::getenv("XYO_JIT_VERBOSE"))
        std::fprintf(stderr,
            "xyo-wasm-hot: run handle=%d slots=%zu phase=%u loop=%u\n",
            code.handle, code.slotIds.size(),
            unsigned(thread.wasmLoopPhase), thread.wasmLoopIndex);
    if (!thread.target || code.handle < 0 || thread.osrActive)
        return HotResult::Fallback;

    if (code.loops.empty()) {
        return invokeSegment(code, code.handle, thread)
            ? HotResult::Completed : HotResult::Fallback;
    }

    using Kind = HotCode::Loop::Kind;
    if (thread.wasmLoopPhase == 0) {
        double count = 0;
        const bool repeat = code.loops.front().kind == Kind::Repeat;
        if (!invokeSegment(code, code.handle, thread,
                           repeat ? &count : nullptr))
            return HotResult::Fallback;
        thread.wasmLoopIndex = 0;
        thread.wasmLoopRemaining = count;
        thread.wasmLoopPhase = repeat && count <= 0 ? 2 : 1;
    }

    // Empty/count-zero loops and immediately-false while/until predicates
    // can be traversed without yielding; each successful loop body always
    // yields exactly once, preserving other Scratch threads' opportunity to
    // run between iterations.
    while (thread.wasmLoopIndex < code.loops.size()) {
        const auto& loop = code.loops[thread.wasmLoopIndex];
        if (thread.wasmLoopPhase == 1) {
            if (loop.kind == Kind::Until || loop.kind == Kind::While ||
                loop.kind == Kind::ForEach) {
                double condition = 0;
                if (!invokeSegment(code, loop.conditionHandle, thread,
                                   &condition, false)) {
                    restoreInterpreterContinuation(code, thread);
                    return HotResult::Fallback;
                }
                const bool truth = condition != 0 && !std::isnan(condition);
                if ((loop.kind == Kind::Until && truth) ||
                    (loop.kind == Kind::While && !truth) ||
                    (loop.kind == Kind::ForEach &&
                     !(thread.wasmLoopRemaining < condition))) {
                    thread.wasmLoopPhase = 2;
                }
            }
            if (thread.wasmLoopPhase == 1) {
                // Keep the JS/WASM handoff bounded for large dynamic
                // ranges; deopt into the existing interpreter at the
                // exact index instead of changing loop semantics.
                if (loop.kind == Kind::ForEach &&
                    thread.wasmLoopRemaining >= kMaxWasmRepeat) {
                    restoreInterpreterContinuation(code, thread);
                    return HotResult::Fallback;
                }
                const bool isForEach = loop.kind == Kind::ForEach;
                if (!invokeSegment(code, loop.bodyHandle, thread,
                                   nullptr, true,
                                   isForEach ? int(loop.variableSlot) : -1,
                                   thread.wasmLoopRemaining + 1.0)) {
                    restoreInterpreterContinuation(code, thread);
                    return HotResult::Fallback;
                }
                if (isForEach) thread.wasmLoopRemaining += 1.0;
                if (loop.kind == Kind::Repeat) {
                    thread.wasmLoopRemaining -= 1.0;
                    if (thread.wasmLoopRemaining <= 0)
                        thread.wasmLoopPhase = 2;
                }
                return HotResult::Yielded;
            }
        }

        if (thread.wasmLoopPhase != 2)
            return HotResult::Fallback;
        const size_t nextIndex = size_t(thread.wasmLoopIndex) + 1;
        const bool nextRepeat = nextIndex < code.loops.size() &&
                               code.loops[nextIndex].kind == Kind::Repeat;
        double nextCount = 0;
        if (!invokeSegment(code, loop.suffixHandle, thread,
                           nextRepeat ? &nextCount : nullptr)) {
            restoreInterpreterContinuation(code, thread);
            return thread.status == STATUS_DONE
                ? HotResult::Completed : HotResult::Fallback;
        }
        thread.wasmLoopIndex = static_cast<uint32_t>(nextIndex);
        thread.wasmLoopRemaining = nextCount;
        if (nextIndex == code.loops.size()) {
            thread.wasmLoopPhase = 0;
            return HotResult::Completed;
        }
        thread.wasmLoopPhase = nextRepeat && nextCount <= 0 ? 2 : 1;
    }
    return HotResult::Completed;
#endif
}

} // namespace xyo::wasm
