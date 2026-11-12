// xyo-cpp: engine/thread.js 移植。
#include "thread.hpp"

#include "../jit/jit.hpp"
#include "numeric_thread.hpp"
#include "../jit/procedure_frame_state.hpp"
#include "../ir/source_effect_runtime_trace.hpp"
#include "../ir/source_list_read_events.hpp"
#include "../ir/source_scalar_read_events.hpp"
#include "../model/target.hpp"
#include "fiber.hpp"
#include "runtime.hpp"

namespace xyo {

namespace {
bool ir001SourceTraceEnabled() {
    // The IR-001 instrumentation flag is fixed per process. Keep default
    // Scratch Thread creation/retirement free of registry locks/allocations.
    static const bool enabled = [] {
        const char* value = std::getenv("XYO_IR001_TRACE_EFFECT_PC");
        return value && value[0] == '1';
    }();
    return enabled;
}
} // namespace

Thread::Thread(std::string firstBlock) : topBlock(std::move(firstBlock)) {
    // warpTimer は遅延 init（stepThread で初めて必要になったとき start）
}

Thread::~Thread() {
    if (ir001SourceTraceEnabled())
        ir::NativeSourceEffectTimelineRegistry::get().erase(this);
    FiberPool::release(fiber);
    delete jitCtx;
}

std::string Thread::getIdFromTargetAndBlock(const Target* target,
                                            const std::string& tb) {
    return target->id + "&" + tb;
}

const std::string& Thread::getId() const {
    if (idCache_.empty()) idCache_ = getIdFromTargetAndBlock(target, topBlock);
    return idCache_;
}

void Thread::releaseFinished() {
    pendingShortBroadcast = false;
    if (ir001SourceTraceEnabled())
        ir::NativeSourceEffectTimelineRegistry::get().erase(this);
    nativeAsyncVariableRoots.clear();
    nativeAsyncReporterReturnPc = 0;
    nativeAsyncReporterFramePtr = 0;
    nativeAsyncReporterInputBuffer = 0;
    nativeAsyncReporterInputWords = 0;
    nativeRtSsaCapture.reset();
    nativeSourceListReads.reset();
    nativeSourceScalarReads.reset();
    nativeSourceReporterReadSequence = 1;
    nativeGcSoftBudgetYieldDue = false;
    nativeGcSoftBudgetAttempted = false;
    nativeRootLoopYieldCount = 0;
    if (nativeProcedureFrames) {
        if (std::getenv("XYO_JIT_CALLFRAME_STATS"))
            nativeProcedureFrames->report(topBlock.c_str());
        nativeProcedureFrames.reset();
    }
    numericSession.reset();
    osrActive = false;
    nestedOsrActive = false;
    osrRemaining = 0.0;
    eventWaitArmed = false;
    eventWatchValid = false;
    eventWatchVariable = nullptr;
    eventWatchAdditional.reset();
    wasmLoopPhase = 0;
    wasmLoopIndex = 0;
    wasmLoopRemaining = 0.0;
    wasmRegionIndex = 0;
    nestedWasmEligible = false;
    nestedWasmBindings.reset();
    wasmSlotBits.clear();
    stack.clear();
    stackFrames.clear();
    trashFrames.clear();
    framePool.clear();
    blockGlowInFrame.clear();
    justReported = Value::undefined();
    compatibilityStackFrame.vals.clear();
    compatibilityStackFrame.startedThreads.clear();
    compatibilityStackFrame.startedThreads.shrink_to_fit();
    compatibilityStackFrame.returnValue = Value::undefined();
    compatibilityStackFrame.hasStartedThreads = false;
    hasCompatStackFrame = false;
    jitVarCache.clear();
    jitVarSlots.clear();
    nativeVarCacheTarget = nullptr;
    nativeVarCacheStage = nullptr;
    nativeVarCacheLocalGeneration = 0;
    nativeVarCacheStageGeneration = 0;
    nativeVarCacheValid = false;
    nativeVarCacheHits = 0;
    nativeVarCacheMisses = 0;
    nativeVarCacheInvalidations = 0;
    nativeVarIndexedSlots.clear();
    nativeVarInlineEntries = nullptr;
    nativeVarInlineCount = 0;
    nativeVarInlineTraceHits = 0;
    nativeVarLocalGenerationAddress = nullptr;
    nativeVarStageGenerationAddress = nullptr;
}

void Thread::pushStack(const std::string& blockId) {
    pendingShortBroadcast = false;
    // A tiny native script can begin with only its root block ID. If it
    // unexpectedly enters a procedure or compatibility frame, materialize
    // the missing root frame before pushing the nested frame.
    while (stackFrames.size() < stack.size())
        stackFrames.emplace_back(std::make_unique<StackFrame>());
    stack.push_back(blockId);
    if (stack.size() > stackFrames.size()) {
        bool parentWarp = !stackFrames.empty() && stackFrames.back()->warpMode;
        if (!framePool.empty()) {
            // プール収納時に reset 済み（fresh と等価）
            stackFrames.push_back(std::move(framePool.back()));
            framePool.pop_back();
        } else {
            stackFrames.emplace_back(std::make_unique<StackFrame>());
        }
        stackFrames.back()->warpMode = parentWarp;
    }
}

void Thread::reuseStackForNextBlock(const std::string& blockId) {
    pendingShortBroadcast = false;
    // JS では stack が空でも stack[-1]=id は無害な代入、
    // stackFrames[length-1] は残った最終フレームを再利用する。
    // retireThread 直後（edge-activated hat 不発火）にここへ来る場合がある。
    if (!stack.empty()) stack.back() = blockId;
    if (!stackFrames.empty()) stackFrames.back()->reset(true);
}

std::string Thread::popStack() {
    pendingShortBroadcast = false;
    while (stackFrames.size() < stack.size())
        stackFrames.emplace_back(std::make_unique<StackFrame>());
    std::string id = stack.back();
    stack.pop_back();
    // JS GC 相当: ポップしてもオブジェクト自体は生かしておく
    trashFrames.push_back(std::move(stackFrames.back()));
    stackFrames.pop_back();
    return id;
}

void Thread::recycleTrashFrames() {
    constexpr size_t kMaxCachedFrames = 128;
    for (auto& frame : trashFrames) {
        if (framePool.size() >= kMaxCachedFrames) break;
        frame->reset();
        framePool.push_back(std::move(frame));
    }
    trashFrames.clear();
}

void Thread::stopThisScript() {
    std::string blockID = peekStack();
    while (!blockID.empty()) {
        // レポータ形式の procedures_call — waitingReporter 中はここで止まる
        StackFrame* sf = peekStackFrame();
        if (sf && sf->waitingReporter) break;

        const Block* block = target->blocks->getBlock(blockID);
        if (block && block->opcode == "procedures_call") {
            // 手続きは完了済み — 引数が再評価されないよう次へ進める
            goToNextBlock();
            break;
        }
        popStack();
        blockID = peekStack();
    }
    if (stack.empty()) {
        requestScriptGlowInFrame = false;
        status = STATUS_DONE;
    }
}

std::string Thread::peekStack() const {
    return stack.empty() ? std::string{} : stack.back();
}

StackFrame* Thread::peekStackFrame() {
    // Lazily materialize a root frame when an otherwise synchronous native
    // script visits interpreter compatibility or explicit frame inspection.
    while (stackFrames.size() < stack.size())
        stackFrames.emplace_back(std::make_unique<StackFrame>());
    return stackFrames.empty() ? nullptr : stackFrames.back().get();
}

StackFrame* Thread::peekParentStackFrame() {
    return stackFrames.size() > 1 ? stackFrames[stackFrames.size() - 2].get()
                                  : nullptr;
}

void Thread::initParams() {
    peekStackFrame()->hasParams = true;
}

void Thread::pushParam(const std::u16string& name, Value v) {
    peekStackFrame()->params[name] = v;
    peekStackFrame()->hasParams = true;
}

Value Thread::getParam(const std::u16string& name) const {
    for (int i = int(stackFrames.size()) - 1; i >= 0; --i) {
        const StackFrame& f = *stackFrames[i];
        if (!f.hasParams) continue;
        auto it = f.params.find(name);
        if (it != f.params.end()) return it->second;
        return Value::undefined();
    }
    return Value::undefined();
}

const std::unordered_map<std::u16string, Value>* Thread::getAllParams() const {
    const StackFrame* sf = stackFrames.empty() ? nullptr : stackFrames.back().get();
    return sf && sf->hasParams ? &sf->params : nullptr;
}

void Thread::goToNextBlock() {
    // execute() has already resolved the current block. Reuse its immutable
    // successor instead of performing a second unordered_map lookup. Control
    // primitives may change the stack, so verify the active block ID first.
    if (!stack.empty() && !stackFrames.empty()) {
        const BlockCached* cached = stackFrames.back()->currentBlock;
        if (cached && cached->id == stack.back()) {
            reuseStackForNextBlock(cached->nextBlockId);
            return;
        }
    }
    // Hats, compatibility paths and blocks reached without execute() fall
    // back to the original lookup.
    reuseStackForNextBlock(target->blocks->getNextBlock(peekStack()));
}

bool Thread::isRecursiveCall(const std::string& procedureCode) const {
    int callCount = 5;
    int sp = int(stackFrames.size()) - 1;
    for (int i = sp - 1; i >= 0; --i) {
        const BlockCached* op = stackFrames[i]->op;
        const Block* block = nullptr;
        if (op) {
            block = target->blocks->getBlock(op->id);
            if (!block && target->runtime)
                block = target->runtime->flyoutBlocks->getBlock(op->id);
        }
        if (block && block->opcode == "procedures_call" && block->mutation &&
            block->mutation->attr("proccode") == procedureCode) {
            return true;
        }
        if (--callCount < 0) return false;
    }
    return false;
}

void Thread::tryCompile(const Block* topHint) {
    triedToCompile = true;
    // Count deliberately interpreted suspending scripts, so every
    // fiberless smoke run can distinguish "no Fiber was needed" from
    // "the migration accidentally still allocated native stacks".
    static std::atomic<uint64_t> yieldingFallbacks{0};
    static const bool stats = [] {
        const char* flag = std::getenv("XYO_JIT_FIBERLESS_STATS");
        if (!jitFiberlessEnabled() || !flag || flag[0] != '1')
            return false;
        std::atexit([] {
            std::fprintf(stderr,
                "[fiberless-migration] interpreted_yield_scripts=%llu "
                "fiber_acquires=%zu\n",
                static_cast<unsigned long long>(
                    yieldingFallbacks.load(std::memory_order_relaxed)),
                FiberPool::fiberlessAcquires());
        });
        return true;
    }();
    (void)stats;
    Runtime* rt = target ? target->runtime : nullptr;
    if (!rt || !rt->jit || !jitEnabled() || !blockContainer) return;
#if defined(__EMSCRIPTEN__)
    // Browser WASM/Binaryen has no counting instrumentation yet; never
    // silently report incomplete totals.
    if (rt->blockCountEnabled) return;
#endif
    const Block* top =
        topHint ? topHint : blockContainer->getBlock(topBlock);
    if (!top) return;
    topBlockPtr = top;
    static const bool verbose = std::getenv("XYO_JIT_VERBOSE") != nullptr;

    // For a clone-start hat, the source script is immutable across
    // separately created clone Targets sharing a Sprite/Blocks container.
    // JitEngine::decideCompile and ::compile otherwise repeat two hash
    // lookups for *each* clone even when the code is already published.
    // Reuse only an already-ready native script, under exactly the same
    // native/CFG preconditions as the usual cached-compile path.
    // Enabled by default after paired benchmark and parity validation.
    // XYO_JIT_READY_CLONE=0 restores the original decideCompile/compile
    // path without changing any of the surrounding JIT mode semantics.
    static const bool cloneReadyFast = [] {
        const char* env = std::getenv("XYO_JIT_READY_CLONE");
        return !env || env[0] != '0';
    }();
    const char* cfg = std::getenv("XYO_JIT_COMMON_CFG");
    const bool numericCfg = cfg && cfg[0] == '1' && cfg[1] == '\0';
    if (cloneReadyFast && !numericCfg &&
        top->opcode == "control_start_as_clone" &&
        !rt->blockCountEnabled) {
        if (const CompiledScript* ready = rt->jit->readyScript(top, blockContainer);
            ready && (!jitFiberlessEnabled() || !ready->mayYield || ready->wasm)) {
            compiled = ready;
            isCompiled = true;
            return;
        }
    }
    // A short non-yielding native broadcast listener has no per-thread
    // compilation decision left to make once the JIT cache is populated.
    // Keep the normal HotSpot/numeric CFG path for all other scripts.
    // XYO_JIT_READY_CACHE_FAST=0 restores the historical behavior.
    static const bool readyCacheFast = [] {
        const char* env = std::getenv("XYO_JIT_READY_CACHE_FAST");
        return !env || env[0] != '0';
    }();
    if (readyCacheFast && !numericCfg &&
        top->opcode == "event_whenbroadcastreceived") {
        if (const CompiledScript* ready = rt->jit->readyScript(top, blockContainer);
            ready && ready->shortSync && !ready->mayYield &&
            !ready->mainThread) {
            compiled = ready;
            isCompiled = true;
            return;
        }
    }

    // HotSpot: 呼び出し回数・解釈実行ブロック数・スクリプト形状から
    // 今回のスレッドを JIT で走らせるか決める（Eager は常に JIT）。
    if (!rt->jit->decideCompile(*this, top)) {
        if (verbose)
            std::fprintf(stderr, "xyo-jit: interp top=%s\n", topBlock.c_str());
        return;
    }
    if (verbose)
        std::fprintf(stderr, "xyo-jit: compile top=%s target=%s\n",
                     topBlock.c_str(), target->id.c_str());
#if !defined(XYO_DISABLE_RT_PROF)
    struct CProf {
        bool enabled = std::getenv("XYO_PROF") != nullptr;
        double ns = 0; uint64_t n = 0;
        ~CProf() {
            if (enabled)
                fprintf(stderr, "[prof] compile=%.1fms(%llu)\n",
                        ns / 1e6, (unsigned long long)n);
        }
    };
    static CProf cp;
    std::chrono::steady_clock::time_point t0;
    if (cp.enabled) t0 = std::chrono::steady_clock::now();
#endif
    if (rt->jit->tryAttachNumeric(*this, top)) return;
    const CompiledScript* cs =
        rt->jit->compile(*this, *rt, *rt->sequencer, top);
#if !defined(XYO_DISABLE_RT_PROF)
    if (cp.enabled) {
        cp.ns += std::chrono::duration<double, std::nano>(
            std::chrono::steady_clock::now() - t0).count();
        ++cp.n;
    }
#endif
    if (cs && (!jitFiberlessEnabled() || !cs->mayYield || cs->wasm)) {
        compiled = cs;
        isCompiled = true;
    } else if (cs && jitFiberlessEnabled() && cs->mayYield) {
        yieldingFallbacks.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace xyo
