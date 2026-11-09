// xyo-cpp: engine/sequencer.js 移植（profiler / JIT 分岐を除く）。
#include "sequencer.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <vector>

#include "../core/gc.hpp"
#include "../core/sstr.hpp"
#include "../jit/jit.hpp"
#include "../ir/scratch_pipeline.hpp"
#include "../ir/scratch_source_digest.hpp"
#include "numeric_thread.hpp"
#include "source_continuation_handoff.hpp"
#include "../wasm/hot_jit.hpp"
#include "../wasm/nested_jit.hpp"
#include "../model/target.hpp"
#include "deps.hpp"
#include "event_wait_dependency.hpp"
#include "fiber.hpp"
#include "pool.hpp"
#include "runtime.hpp"
#include "warp_clock.hpp"

namespace xyo {

Sequencer::Sequencer(Runtime& rt) : runtime(rt) {
#if defined(__EMSCRIPTEN__) && defined(XYO_WASM_HOTSPOT)
    // This is a correctness-validated experimental continuation, not a
    // blanket performance optimization: one tiny WASM invocation per
    // Scratch body can be slower than the interpreter. Never add the
    // dispatch cost to ordinary scripts unless explicitly opted in.
    const char* nested = std::getenv("XYO_NESTED_WASM");
    if (nested && nested[0] == '1') {
        nestedJit_ = std::make_unique<wasm::NestedJitCache>();
        const char* warpOnly = std::getenv("XYO_NESTED_WASM_WARP_ONLY");
        nestedWarpOnly_ = warpOnly && warpOnly[0] == '1';
    }
#endif
}
Sequencer::~Sequencer() {
    if (std::getenv("XYO_EVENT_SCHED_STATS")) {
        std::fprintf(stderr,
                     "[event-sched] skipped=%llu woken=%llu\n",
                     static_cast<unsigned long long>(eventSkippedPasses_),
                     static_cast<unsigned long long>(eventWakeups_));
    }
}

bool Sequencer::eventSchedulerEnabled() const {
    static const bool enabled = [] {
        const char* env = std::getenv("XYO_EVENT_SCHED");
        return env && env[0] == '1';
    }();
    return enabled;
}

void Sequencer::armEventWait(Thread& thread, uintptr_t site) {
    // A pure, source-validated scalar wait can be suspended on its own:
    // unlike the global event scheduler, no unrelated script is changed.
    if (!eventSchedulerEnabled() && !isolatedEventWaitDependencyEnabled())
        return;
    // Interpreter wait sites do not install a JIT dependency watcher. Never
    // reuse a snapshot from a previously compiled wait or different site.
    thread.eventWatchValid = false;
    thread.eventWaitEpoch = eventProgressEpoch.load(std::memory_order_relaxed);
    thread.eventWaitSite = site;
    thread.eventWaitArmed = true;
}


std::atomic<int64_t>* Sequencer::sampledWarpClock() const {
    return warpWatchdog_ && warpWatchdog_->armed()
        ? &warpWatchdog_->sampledUs : nullptr;
}

std::atomic<int64_t>* Sequencer::sampledWarpDeadline() const {
    return warpWatchdog_ && warpWatchdog_->armed()
        ? &warpWatchdog_->deadlineUs : nullptr;
}

std::atomic<int64_t>* Sequencer::sampledUnixClock() const {
    return warpWatchdog_ && warpWatchdog_->armed() &&
           warpWatchdog_->samplesWallClock()
        ? &warpWatchdog_->sampledUnixMs : nullptr;
}

std::atomic<uint64_t>* Sequencer::sampledUnixDays() const {
    return warpWatchdog_ && warpWatchdog_->armed() &&
           warpWatchdog_->samplesWallClock()
        ? &warpWatchdog_->sampledUnixDaysBits : nullptr;
}


namespace {

#if !defined(XYO_DISABLE_RT_PROF)
// Opt-in per-hat profiling. Reports which Scratch scripts consume
// the sequencer's time budget without requiring a native profiler.
struct TopProfile {
    struct Row { double ms = 0; uint64_t calls = 0; };
    std::mutex lock;
    std::unordered_map<std::string, Row> rows;
    ~TopProfile() {
        std::vector<std::pair<std::string, Row>> sorted(rows.begin(), rows.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
            return a.second.ms > b.second.ms;
        });
        const size_t n = std::min<size_t>(40, sorted.size());
        for (size_t i = 0; i < n; ++i)
            std::fprintf(stderr, "[topprof] %s %.3fms %llu calls\n",
                         sorted[i].first.c_str(), sorted[i].second.ms,
                         static_cast<unsigned long long>(sorted[i].second.calls));
    }
};
TopProfile& topProfile() { static TopProfile profile; return profile; }
struct TopTimer {
    const std::string& key;
    uint64_t frame;
    std::chrono::steady_clock::time_point start;
    explicit TopTimer(const std::string& id, uint64_t stepSerial)
        : key(id), frame(stepSerial),
          start(std::chrono::steady_clock::now()) {}
    ~TopTimer() {
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        if (std::getenv("XYO_TOPTRACE") && ms >= 2.0)
            std::fprintf(stderr, "[toptrace] frame=%llu script=%s %.3fms\n",
                static_cast<unsigned long long>(frame - 1),
                key.c_str(), ms);
        auto& profile = topProfile();
        std::lock_guard<std::mutex> guard(profile.lock);
        auto& row = profile.rows[key];
        row.ms += ms;
        ++row.calls;
    }
};
#endif  // !XYO_DISABLE_RT_PROF (per-hat profiling)

// A conservative poll-only script classifier. The loop is a tree of
// conditional tests whose only possible actions are sound-play-until-done.
// When sound starts, the thread blocks. When no branch starts a sound, one
// iteration has no observable effect. Do not classify random/time-dependent
// reporters, variable writes, movement, pen, or arbitrary custom blocks.
bool purePollExpression(const Blocks& blocks, const std::string& id,
                        int depth = 0) {
    if (id.empty()) return true; // Inline constant (no child block).
    if (depth >= 20) return false;
    const Block* block = blocks.getBlock(id);
    if (!block) return false;
    const std::string& op = block->opcode;
    if (op == "data_variable" || op == "math_number" ||
        op == "math_integer" || op == "math_whole_number" ||
        op == "math_positive_number" || op == "text" ||
        op == "operator_true" || op == "operator_false")
        return true;
    if (op != "operator_equals" && op != "operator_or" &&
        op != "operator_and" && op != "operator_not" &&
        op != "operator_lt" && op != "operator_gt")
        return false;
    for (const auto& [key, input] : block->inputs.items) {
        if (!purePollExpression(blocks, input.block, depth + 1)) return false;
    }
    return true;
}

bool pollBranch(const Blocks& blocks, const std::string& first,
                bool& foundWait, int& budget) {
    std::string id = first;
    while (!id.empty()) {
        if (--budget <= 0) return false;
        const Block* b = blocks.getBlock(id);
        if (!b) return false;
        if (b->opcode == "sound_playuntildone") {
            const BlockInput* menu = b->inputs.find("SOUND_MENU");
            if (!menu) return false;
            if (!menu->block.empty()) {
                const Block* name = blocks.getBlock(menu->block);
                if (!name || name->opcode != "sound_sounds_menu")
                    return false;
            }
            foundWait = true;
        } else if (b->opcode == "control_if" ||
                   b->opcode == "control_if_else") {
            const BlockInput* cond = b->inputs.find("CONDITION");
            if (!cond || !purePollExpression(blocks, cond->block))
                return false;
            for (const auto* branch : {"SUBSTACK", "SUBSTACK2"}) {
                if (const BlockInput* in = b->inputs.find(branch)) {
                    if (!pollBranch(blocks, in->block, foundWait, budget))
                        return false;
                }
            }
        } else {
            return false;
        }
        id = b->next;
    }
    return true;
}

bool isIdleSoundPoll(const Thread& thread) {
    const Blocks* blocks = thread.blockContainer;
    if (!blocks) return false;
    const Block* cur = blocks->getBlock(thread.topBlock);
    if (!cur || (cur->opcode != "event_whenbroadcastreceived" &&
                 cur->opcode != "event_whenflagclicked"))
        return false;
    // Only no-yield one-time initializers are permitted before forever.
    int budget = 100;
    cur = cur->next.empty() ? nullptr : blocks->getBlock(cur->next);
    while (cur && --budget > 0 && cur->opcode != "control_forever") {
        if (cur->opcode != "control_stop" &&
            cur->opcode != "sound_stopallsounds" &&
            cur->opcode != "event_broadcast")
            return false;
        cur = cur->next.empty() ? nullptr : blocks->getBlock(cur->next);
    }
    if (!cur || cur->opcode != "control_forever" || !cur->next.empty())
        return false;
    const BlockInput* body = cur->inputs.find("SUBSTACK");
    bool foundWait = false;
    return body && pollBranch(*blocks, body->block, foundWait, budget)
           && foundWait;
}

#if !defined(XYO_DISABLE_RT_PROF)
// --- 簡易プロファイラ（XYO_PROF=1 で有効、終了時に stderr へ集計）----
// worker スレッドからも更新されるためカウンタは atomic。
struct ProfAccum {
    std::atomic<double> stepJitNs{0}, stepInterpNs{0};
    std::atomic<double> fiberNs{0}, syncNs{0};
    std::atomic<uint64_t> nJit{0}, nInterp{0}, nFiber{0}, nSync{0};
    std::atomic<uint64_t> parThreads{0}, parGroups{0}, parBatches{0};
    bool enabled = std::getenv("XYO_PROF") != nullptr;
    ~ProfAccum() {
        if (!enabled) return;
        fprintf(stderr,
                "[prof] jitStep=%.1fms(%llu) interp=%.1fms(%llu)\n"
                "[prof] fiber=%.1fms(%llu) sync=%.1fms(%llu)\n"
                "[prof] par: threads=%llu groups=%llu batches=%llu\n",
                stepJitNs.load() / 1e6, (unsigned long long)nJit,
                stepInterpNs.load() / 1e6, (unsigned long long)nInterp,
                fiberNs.load() / 1e6, (unsigned long long)nFiber,
                syncNs.load() / 1e6, (unsigned long long)nSync,
                (unsigned long long)parThreads,
                (unsigned long long)parGroups,
                (unsigned long long)parBatches);
    }
};
ProfAccum& prof() { static ProfAccum p; return p; }
struct ProfScope {
    std::atomic<double>* acc;
    std::chrono::steady_clock::time_point t0;
    explicit ProfScope(std::atomic<double>* a)
        : acc(a), t0(std::chrono::steady_clock::now()) {}
    ~ProfScope() {
        acc->fetch_add(std::chrono::duration<double, std::nano>(
            std::chrono::steady_clock::now() - t0).count(),
                       std::memory_order_relaxed);
    }
};
#endif  // !XYO_DISABLE_RT_PROF

/// fiber エントリ: コンパイル済みスクリプトを実行（yield は fiber switch）
void jitEntry(void* arg) {
    JitCtx* c = static_cast<JitCtx*>(arg);
    const CompiledScript* cs =
        static_cast<const CompiledScript*>(c->thread->compiled);
    c->warpStartMs = c->runtime->currentMSecs;
    (c->thread->nestedOsrActive ? cs->nestedOsrEntry :
     c->thread->osrActive ? cs->osrEntry : cs->entry)(c);
}

/// Finished contexts contain no live roots and can be reused by whichever
/// worker completed the script (fibers may migrate). Shard the free list by
/// native thread instead of contending twice per script on one global mutex.
struct JitCtxFreeList {
    std::vector<JitCtx*> entries;
    ~JitCtxFreeList() { for (JitCtx* c : entries) delete c; }
};
std::vector<JitCtx*>& jitCtxPool() {
    static thread_local JitCtxFreeList p;
    return p.entries;
}
constexpr size_t kJitCtxPoolCap = 64;

JitCtx* acquireJitCtx() {
    auto& p = jitCtxPool();
    if (!p.empty()) { JitCtx* c = p.back(); p.pop_back(); return c; }
    return new JitCtx{};
}
void releaseJitCtx(JitCtx* c) {
    if (!c) return;
    // Covers normal entry return, stopped/killed threads, fiber-chain
    // completion and a context retired without a final yield.
    flushJitBlockCount(c);
    *c = JitCtx{};
    auto& p = jitCtxPool();
    if (p.size() < kJitCtxPoolCap) p.push_back(c);
    else delete c;
}

} // namespace

bool Sequencer::idleSoundPollFor(const Thread& thread) {
    const Blocks* blocks = thread.blockContainer;
    if (!blocks) return false;
    const Block* top = blocks->getBlock(thread.topBlock);
    if (!top) return false;
    const auto [it, inserted] = idleSoundPollCache_.try_emplace(top, false);
    if (inserted) it->second = isIdleSoundPoll(thread);
    return it->second;
}

bool Sequencer::warpBudgetExhausted(const Thread& thread) const {
    // dangerMode では予算が無い — 時計を読まずに false を返す（interp の
    // warp ループは毎反復ここを通るため、steady_clock の読みを消す価値がある）。
    if (runtime.dangerMode) return false;
    return thread.warpTimer.timeElapsed() > runtime.warpTimeMs;
}

std::vector<Thread*> Sequencer::stepThreads() {
    std::vector<Thread*> doneThreads;
    const double WORK_TIME = 0.75 * runtime.currentStepTime;
    runtime.updateCurrentMSecs();
    timer.start();
    const bool eventDriven = eventSchedulerEnabled();
    const bool isolatedWatch = isolatedEventWaitDependencyEnabled();
    // Process external changes/IO once per tick even for pure conditions.
    if (eventDriven)
        eventProgressEpoch.fetch_add(1, std::memory_order_relaxed);
    // Default JIT path: a shared, ~1ms sampled clock instead of calling
    // steady_clock from each warp polling checkpoint. Custom clocks remain
    // on the synchronous helper path.
    static const bool sharedClockEnabled = [] {
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
        // A non-pthread WebAssembly runtime cannot start the background
        // 1ms watchdog thread. Use the ordinary scheduler clock instead.
        return false;
#else
        const char* env = std::getenv("XYO_JIT_WARP_CLOCK");
        return !env || env[0] != '0';
#endif
    }();
    static const bool sharedDaysClockEnabled = [] {
        const char* env = std::getenv("XYO_JIT_DAYS_SHARED_CLOCK");
        return env && env[0] != '0' && !std::getenv("XYO_PROF");
    }();
    if (sharedClockEnabled && jitEnabled() && timer.usesSteadyClock()) {
        if (!warpWatchdog_)
            warpWatchdog_ = std::make_unique<WarpClockWatchdog>(sharedDaysClockEnabled);
        warpWatchdog_->arm(timer.startTime() + runtime.warpTimeMs,
                           runtime.dangerMode);
    }
    size_t numActiveThreads = SIZE_MAX;
    bool ranFirstTick = false;
    const bool allowParallel = parallelEnabled();

    while (!runtime.threads.empty() && numActiveThreads > 0 &&
           timer.timeElapsed() < WORK_TIME &&
           (runtime.turboMode || !runtime.redrawRequested)) {
        numActiveThreads = 0;
        bool stoppedThread = false;
        // JS: const threads = this.runtime.threads — パス開始時に掴む。
        // stopAll で差し替わっても stepping 経由で旧配列を反復する。
        stepping = &runtime.threads;
        for (size_t i = 0; i < stepping->size(); ++i) {
            Thread* activeThread = this->activeThread = (*stepping)[i];
            if (activeThread->stack.empty() ||
                activeThread->status == STATUS_DONE) {
                stoppedThread = true;
                continue;
            }
            if (activeThread->status == STATUS_YIELD_TICK && !ranFirstTick) {
                activeThread->status = STATUS_RUNNING;
            }
            if (activeThread->status == STATUS_RUNNING ||
                activeThread->status == STATUS_YIELD) {
                const bool wasEventWait =
                    (eventDriven || isolatedWatch) &&
                    activeThread->eventWaitArmed;
                const uintptr_t oldWaitSite = activeThread->eventWaitSite;
                const uint64_t oldWaitExit = activeThread->eventWaitExitCount;
                if (wasEventWait) {
                    // A pure condition with exactly one scalar dependency
                    // cannot change while that value's bits and source/
                    // binding generations remain identical. This is stronger
                    // than the global progress epoch: unrelated scripts can
                    // make progress without waking every suspended Fiber.
                    // If the watched value or its binding changed, force one
                    // reevaluation even if the global epoch is unchanged.
                    const bool watchValid = activeThread->eventWatchValid;
                    const bool watchUnchanged = watchValid &&
                        canSkipEventWaitByDependency(*activeThread);
                    const bool epochUnchanged = eventDriven && !watchValid &&
                        activeThread->eventWaitEpoch ==
                            eventProgressEpoch.load(std::memory_order_relaxed);
                    if (watchUnchanged || epochUnchanged) {
                        ++eventSkippedPasses_;
                        continue;
                    }
                }
                if (wasEventWait) {
                    ++eventWakeups_;
                    activeThread->eventWaitArmed = false;
                }
                // Tiny scripts whose generated entry cannot yield are
                // cheaper to run in-order than to enqueue, build conflict
                // groups and hand off to workers. Long or yielding scripts
                // retain the original parallel scheduling policy.
                static const bool serialShortSync = [] {
                    const char* env = std::getenv("XYO_SHORT_SYNC_SERIAL");
                    return !env || env[0] != '0';
                }();
                const auto* compiled = activeThread->isCompiled
                    ? static_cast<const CompiledScript*>(
                        activeThread->compiled)
                    : nullptr;
                const bool immediate = serialShortSync && compiled &&
                    compiled->shortSync;
                // Resumed event waiters run in-order: one extra evaluation
                // cannot silently overtake a writer in a parallel batch.
                const AccessSet* access = allowParallel && !immediate && !wasEventWait
                    ? parallelAccessFor(*activeThread) : nullptr;
                if (access) {
                    // 並列バッチに溜める — flush は次の serial スレッドの
                    // 直前に必ず行うため、先行スレッドの効果は後続へ
                    // 逐次スケジュールと同じ順序で見える。
                    // 帳簿（active 計数・stopped 判定）は flush 時に
                    // 実行後の状態で行う。
                    parBatch_.push_back(activeThread);
                    parSets_.push_back(access);
                } else {
                    ParFlush fr = flushParBatch();
                    stoppedThread |= fr.stopped;
                    numActiveThreads += fr.active;
                    stepThread(*activeThread);
                    if (eventDriven && !(wasEventWait &&
                        activeThread->eventWaitArmed &&
                        activeThread->eventWaitSite == oldWaitSite &&
                        activeThread->eventWaitExitCount == oldWaitExit)) {
                        // Same condition, false again = no script-side
                        // progress. A different wait or completed script may
                        // have modified observable Scratch state.
                        eventProgressEpoch.fetch_add(1, std::memory_order_relaxed);
                    }
                    // Self-rebroadcast can replace the active thread in the
                    // list while its current block continues to completion.
                    // Its next scheduling turn is now unreachable.
                    if (!runtime.isActiveThread(activeThread))
                        retireThread(*activeThread);
                    if (activeThread->status == STATUS_RUNNING)
                        ++numActiveThreads;
                    if (activeThread->stack.empty() ||
                        activeThread->status == STATUS_DONE) {
                        stoppedThread = true;
                    }
                }
                activeThread->hasWarpTimer = false;
            }
        }
        {
            ParFlush fr = flushParBatch();
            stoppedThread |= fr.stopped;
            numActiveThreads += fr.active;
        }
        ranFirstTick = true;

        if (stoppedThread) {
            size_t nextActive = 0;
            for (size_t i = 0; i < runtime.threads.size(); ++i) {
                Thread* t = runtime.threads[i];
                if (!t->stack.empty() && t->status != STATUS_DONE) {
                    t->runtimeIndex = nextActive;
                    runtime.threads[nextActive++] = t;
                } else {
                    runtime.threadMap.erase(t->getId());
                    t->releaseFinished();
                    doneThreads.push_back(t);
                }
            }
            runtime.threads.resize(nextActive);
        }
        // A lone, read-only sound-polling forever has nothing to observe
        // until another Scratch script can change a condition. No other
        // runnable thread remains in this tick, so probing it thousands of
        // times cannot start any new audio. Re-check once next frame.
        // Disable with XYO_IDLE_SOUND_POLL=0 for scheduling A/B tests.
        static const bool idleSoundPoll = [] {
            const char* v = std::getenv("XYO_IDLE_SOUND_POLL");
            return !v || v[0] != '0';
        }();
        if (idleSoundPoll && !runtime.threads.empty()) {
            Thread* lone = nullptr;
            bool multiple = false;
            for (Thread* t : runtime.threads) {
                if (t->status != STATUS_RUNNING && t->status != STATUS_YIELD)
                    continue;
                if (lone) { multiple = true; break; }
                lone = t;
            }
            if (!multiple && lone && idleSoundPollFor(*lone))
                break;
        }
    }

    this->activeThread = nullptr;
    stepping = nullptr;
    detachedLists.clear();
    if (warpWatchdog_) warpWatchdog_->disarm();
    return doneThreads;
}

void Sequencer::stepThread(Thread& thread) {
    // Optional cooperative startup of a pending native script. A long
    // one-shot warp procedure cannot currently OSR out of interpreter
    // stack frames, so defer its *first* Scratch instruction for a bounded
    // wall-clock interval while LLVM runs on a worker. No previously
    // executed Scratch operation is replayed and the render loop stays free.
    // This can change the relative order of scripts and is opt-in.
    static const double asyncWarmupMs = [] {
        const char* text = std::getenv("XYO_JIT_ASYNC_WARMUP_MS");
        if (!text || !*text) return 0.0;
        char* end = nullptr;
        const double ms = std::strtod(text, &end);
        return end != text && end && !*end && std::isfinite(ms) &&
               ms >= 1.0 && ms <= 5000.0 ? ms : 0.0;
    }();
    if (asyncWarmupMs && runtime.renderer && !thread.interpreterStarted &&
        !thread.isCompiled && !thread.isKilled && !thread.stackClick &&
        !thread.updateMonitor && thread.triedToCompile &&
        thread.topBlockPtr && runtime.jit &&
        runtime.jit->isCompiling(thread.topBlockPtr)) {
        if (!thread.asyncJitWarmupDeadlineMs) {
            thread.asyncJitWarmupDeadlineMs =
                runtime.currentMSecs + asyncWarmupMs;
            if (std::getenv("XYO_JIT_ASYNC_WARMUP_LOG"))
                std::fprintf(stderr,
                    "[jit-warmup] deferred script=%s budget=%.0fms\n",
                    thread.topBlock.c_str(), asyncWarmupMs);
        }
        if (runtime.currentMSecs < thread.asyncJitWarmupDeadlineMs) {
            thread.status = STATUS_YIELD_TICK;
            return;
        }
    }
#if !defined(XYO_DISABLE_RT_PROF)
    static const bool topProfEnabled =
        std::getenv("XYO_TOPPROF") || std::getenv("XYO_TOPTRACE");
    std::optional<TopTimer> topTimer;
    if (topProfEnabled) topTimer.emplace(thread.topBlock, runtime.stepSerial);
#endif
    if (thread.numericSession) {
        numericStepThread(thread);
        return;
    }
    if (thread.isCompiled) {
#if !defined(XYO_DISABLE_RT_PROF)
        std::optional<ProfScope> ps;
        if (prof().enabled) { ps.emplace(&prof().stepJitNs); ++prof().nJit; }
#endif
        jitStepThread(thread);
        static const bool sourceHandoffProbe = [] {
            const char* setting = std::getenv("XYO_RT_SOURCE_HANDOFF_PROBE");
            return setting && setting[0] == '1';
        }();
        static const bool nativeSimpleDeopt = [] {
            const char* setting = std::getenv("XYO_RT_NATIVE_DEOPT_ONCE");
            return setting && setting[0] == '1';
        }();
        // Tests can request a later checkpoint to exercise both the
        // middle and final repeat iterations, not only the first yield.
        static const uint64_t nativeDeoptAfter = [] {
            const char* setting = std::getenv("XYO_RT_NATIVE_DEOPT_AFTER");
            if (!setting || !*setting) return uint64_t(1);
            char* end = nullptr;
            const auto n = std::strtoull(setting, &end, 10);
            return n && end && !*end ? uint64_t(n) : UINT64_MAX;
        }();
        // Strict test mode: native source-level AfterCallee must be the
        // checkpoint adopted, not an earlier completed repeat body.
        static const bool onlyAfterCallee = [] {
            const char* setting =
                std::getenv("XYO_RT_TEST_DEOPT_AFTER_CALLEE_ONLY");
            return setting && setting[0] == '1';
        }();
        // Test-only precision gate: a root loop body's native effects
        // are committed, and only the source loop latch may run again.
        // This must not accidentally adopt the earlier callee checkpoint.
        static const bool onlyRootAfterLoop = [] {
            const char* setting =
                std::getenv("XYO_RT_TEST_DEOPT_ROOT_AFTER_LOOP_ONLY");
            return setting && setting[0] == '1';
        }();
        const bool correctNativePhase = [&] {
            if (!onlyAfterCallee && !onlyRootAfterLoop) return true;
            const auto checkpoint = thread.nativeProcedureFrames
                ? thread.nativeProcedureFrames->sourceCheckpoint()
                : std::nullopt;
            if (!checkpoint) return false;
            if (onlyRootAfterLoop)
                return thread.nativeProcedureFrames->depth() == 1 &&
                    checkpoint->phase ==
                        ir::SourceResumePhase::AfterLoopBody;
            return checkpoint->phase ==
                ir::SourceResumePhase::AfterCallee;
        }();
        // The native CallFrame mirror historically counts only yields
        // with an active nonroot procedure. The RT-001 root-only
        // producer deliberately leaves those stable statistics intact:
        // its first genuine post-Fiber root loop yield has 0 such
        // procedure yields. Permit it ONLY in the strict root-loop test
        // gate and ONLY for the first requested native checkpoint.
        static const bool rootOnlyProducerEnabled = [] {
            const char* v = std::getenv("XYO_RT_ROOT_ONLY_DEOPT");
            return v && v[0] == '1';
        }();
        const bool rootOnlyFirstYield =
            (onlyRootAfterLoop || rootOnlyProducerEnabled) &&
            nativeDeoptAfter == 1 && thread.nativeProcedureFrames &&
            thread.nativeProcedureFrames->depth() == 1 &&
            thread.nativeProcedureFrames->yields() == 0 &&
            // The opt-in producer may attach a frame at script entry,
            // but its mere presence never proves an executable source PC.
            // Before adoption, require a completed loop body from the
            // actual suspended LLVM Fiber, not an unknown/inside phase.
            [&] {
                const auto state =
                    thread.nativeProcedureFrames->sourceCheckpoint();
                return state &&
                    state->phase == ir::SourceResumePhase::AfterLoopBody &&
                    !state->loops.empty() &&
                    state->frame.resumePc ==
                        state->loops.back().sourcePc;
            }();
        const bool attemptDeopt = nativeSimpleDeopt &&
            thread.nativeProcedureFrames && correctNativePhase &&
            (thread.nativeProcedureFrames->yields() >= nativeDeoptAfter ||
             rootOnlyFirstYield);
        // Test-only revision change before native continuation handoff.
        // Do not resetCache(): doing so frees cached BlockCached objects
        // still referenced by the suspended native recursive Fiber.
        // A revision stamp alone is enough to test stale image rejection.
        static const bool testBumpSourceRevision = [] {
            const char* setting =
                std::getenv("XYO_RT_TEST_BUMP_SOURCE_REVISION");
            return setting && setting[0] == '1';
        }();
        static const bool testRealCacheReset = [] {
            const char* setting =
                std::getenv("XYO_RT_TEST_REAL_CACHE_RESET");
            return setting && setting[0] == '1';
        }();
        if (attemptDeopt && testBumpSourceRevision &&
            thread.nativeProcedureFrames && thread.blockContainer &&
            thread.nativeProcedureFrames->sourceUnchanged()) {
            const uint64_t compiled =
                thread.nativeProcedureFrames->compiledRevision();
            if (testRealCacheReset) {
                // Exercise the production invalidation path while native
                // compiled code still retains BlockCached* pointers.
                // Old cache generations must stay alive and rooted.
                thread.blockContainer->resetCache();
                Gc::get().collect();
                std::fprintf(stderr,
                    "[native-real-cache-reset] script=%s gc=1\n",
                    thread.topBlock.c_str());
            } else {
                thread.blockContainer->
                    bumpRevisionForTestingWithoutCacheReset();
            }
            std::fprintf(stderr,
                "[native-source-revision-bump] compiled=%llu live=%llu\n",
                static_cast<unsigned long long>(compiled),
                static_cast<unsigned long long>(
                    thread.blockContainer->revision()));
        }
        // Test the immutable source *content* fingerprint independently
        // of Blocks::revision(). Editor integrations should use resetCache,
        // but a raw Block edit must not authorize a stale compiled handoff.
        static const bool testDirectSourceEdit = [] {
            const char* setting = std::getenv("XYO_RT_TEST_DIRECT_SOURCE_EDIT");
            return setting && setting[0] == '1';
        }();
        if (attemptDeopt && testDirectSourceEdit &&
            thread.nativeProcedureFrames && thread.blockContainer &&
            thread.nativeProcedureFrames->sourceUnchanged()) {
            Block* hat = thread.blockContainer->getBlock(thread.topBlock);
            if (hat && hat->opcode == "event_whenflagclicked") {
                // The native Fiber has already passed the hat. This edit
                // deliberately avoids resetCache and only affects digest.
                hat->opcode = "event_whenkeypressed";
                std::fprintf(stderr,
                    "[native-source-content-edit] revision=%llu mutated=1\n",
                    static_cast<unsigned long long>(
                        thread.blockContainer->revision()));
            }
        }
        // Malformed lexical parent links must never authorize a restore
        // plan, even if the Blocks revision stays the same. This opt-in
        // test deliberately changes only the source graph after a native
        // yield, without resetCache() or modifying the compiled Fiber.
        static const bool testDirectParentEdit = [] {
            const char* setting = std::getenv("XYO_RT_TEST_DIRECT_PARENT_EDIT");
            return setting && setting[0] == '1';
        }();
        if (attemptDeopt && testDirectParentEdit &&
            thread.nativeProcedureFrames && thread.blockContainer &&
            thread.nativeProcedureFrames->sourceUnchanged()) {
            const auto image =
                thread.nativeProcedureFrames->exportSelfContained();
            const auto decoded = image ? image->decodeFrames() : std::nullopt;
            if (image && decoded && !decoded->empty() &&
                decoded->back().caller) {
                const auto id = image->symbols->block(
                    decoded->back().caller->callBlockPc);
                Block* call = id
                    ? thread.blockContainer->getBlock(std::string(*id))
                    : nullptr;
                if (call && call->opcode == "procedures_call") {
                    call->parent = "__xyo_invalid_parent_after_yield__";
                    std::fprintf(stderr,
                        "[native-source-parent-edit] revision=%llu mutated=1\n",
                        static_cast<unsigned long long>(
                            thread.blockContainer->revision()));
                }
            }
        }
        if ((sourceHandoffProbe || attemptDeopt) &&
            thread.nativeProcedureFrames && thread.status == STATUS_RUNNING &&
            thread.fiber) {
            auto handoff = captureNativeSourceHandoff(thread);
            if (sourceHandoffProbe) {
                if (handoff)
                    std::fprintf(stderr,
                        "[source-handoff] script=%s valid=1 depth=%u roots=%zu"
                        " revision=%llu\n",
                        thread.topBlock.c_str(), handoff->depth(),
                        handoff->rootedArguments(),
                        static_cast<unsigned long long>(handoff->sourceRevision));
                else
                    std::fprintf(stderr,
                        "[source-handoff] script=%s valid=0\n",
                        thread.topBlock.c_str());
            }
            if (attemptDeopt && handoff) {
                if (onlyAfterCallee) {
                    const auto successor = handoff->depth() == 1
                        ? handoff->image->rootPostCallSuccessor()
                        : handoff->image->postCallSuccessor();
                    std::fprintf(stderr,
                        "[native-postcall-site] kind=%d target=%llu "
                        "call=%llu valid=%d depth=%u\n",
                        successor ? int(successor->kind) : -1,
                        static_cast<unsigned long long>(
                            successor ? successor->targetPc : 0),
                        static_cast<unsigned long long>(
                            successor ? successor->callBlockPc : 0),
                        int(successor.has_value()), handoff->depth());
                }
                const bool adopted = adoptSimpleNativeSourceHandoff(
                    thread, *handoff);
                std::fprintf(stderr, "[native-simple-deopt] script=%s adopted=%d\n",
                    thread.topBlock.c_str(), int(adopted));
                // Opt-in GC proof: *both* the suspended native Fiber and
                // the exported, GC-rooted source snapshot must be gone
                // before collection. Only interpreter parameter frames
                // may retain the transferred string/list arguments.
                static const bool testGcAfterAdopt = [] {
                    const char* setting =
                        std::getenv("XYO_RT_TEST_GC_AFTER_ADOPT");
                    return setting && setting[0] == '1';
                }();
                if (adopted && testGcAfterAdopt) {
                    handoff.reset();
                    const size_t objectsBefore = Gc::get().objectCount();
                    Gc::get().collect();
                    std::fprintf(stderr,
                        "[native-gc-after-adopt] script=%s collected=1"
                        " objects_before=%zu objects_after=%zu\n",
                        thread.topBlock.c_str(), objectsBefore,
                        Gc::get().objectCount());
                }
            }
        }
        return;
    }
#if !defined(XYO_DISABLE_RT_PROF)
    std::optional<ProfScope> ps;
    if (prof().enabled) { ps.emplace(&prof().stepInterpNs); ++prof().nInterp; }
#endif
    stepThreadBound(thread, 0);
}

std::unique_ptr<NativeSourceHandoff>
Sequencer::captureNativeSourceHandoff(const Thread& thread) const {
    // This may be called by the host only after the native Fiber returned
    // control to the scheduler at a yield boundary. Do not borrow a live
    // JIT frame, an inactive target or any mutable source node.
    if (!thread.isCompiled || !thread.nativeProcedureFrames ||
        !thread.nativeProcedureFrames->enabled() ||
        !thread.nativeProcedureFrames->sourceUnchanged() ||
        !thread.fiber || thread.fiber == currentFiber() ||
        !thread.jitCtx || thread.status != STATUS_RUNNING ||
        !thread.target || !thread.blockContainer ||
        !runtime.isActiveThread(const_cast<Thread*>(&thread)))
        return {};

    auto image = thread.nativeProcedureFrames->exportSelfContained();
    if (!image || !image->valid() || !image->symbols) return {};
    const uint64_t compiledRevision =
        thread.nativeProcedureFrames->compiledRevision();
    if (!compiledRevision ||
        image->symbols->sourceRevision != compiledRevision ||
        thread.blockContainer->revision() != compiledRevision)
        return {};

    auto handoff = std::make_unique<NativeSourceHandoff>();
    handoff->image = std::move(image);
    handoff->sourceBlocks = thread.blockContainer;
    handoff->sourceTarget = thread.target;
    handoff->sourceRevision = compiledRevision;
    handoff->sourceYieldCount = thread.nativeProcedureFrames->yields();
    handoff->topBlock = thread.topBlock;
    if (!handoff->validFor(thread)) return {};
    return handoff;
}

bool Sequencer::adoptSimpleNativeSourceHandoff(
        Thread& thread, const NativeSourceHandoff& handoff) {
    // Called strictly at a post-yield host safe point. Keep handoff roots
    // alive through parameter transfer and frame reconstruction.
    auto plan = handoff.simpleInterpreterResume(thread);
    if (!plan || !thread.fiber || thread.fiber == currentFiber() ||
        !thread.jitCtx || !runtime.isActiveThread(&thread))
        return false;

    // Negative regression hook: prove that a validated but accidentally
    // emptied restore plan cannot retire the native Fiber. The injected
    // plan must be refused without changing any live Thread state.
    static const bool testEmptyPlan = [] {
        const char* v = std::getenv("XYO_RT_TEST_EMPTY_RESUME_PLAN");
        return v && v[0] == '1';
    }();
    if (testEmptyPlan) *plan = SimpleNativeInterpreterResume{};

    // Test-only corruption: remove the LAST reconstructed source loop
    // while retaining its committed ancestor IF frame. An empty-plan
    // guard alone cannot detect this malformed nonempty continuation.
    static const bool testDropLoopLatch = [] {
        const char* v = std::getenv("XYO_RT_TEST_DROP_RESUME_LOOP");
        return v && v[0] == '1';
    }();
    if (testDropLoopLatch) {
        auto& controls = plan->calls.empty()
            ? plan->rootControls : plan->calls.back().activeControls;
        if (!controls.empty() && controls.back().isLoop)
            controls.pop_back();
    }

    // Do not discard the native Fiber for an empty continuation plan.
    // A depth-one root AfterLoopBody used to pass source validation while
    // restoring ZERO interpreter frames, silently losing the remainder of
    // its loop and all following effects. A verified ScriptExit is the only
    // legitimate stackless transfer; it cannot also carry pending calls
    // or a runnable next statement. Reject before mutating Thread state.
    const bool hasExecutableContinuation =
        !plan->rootControls.empty() || !plan->calls.empty() ||
        !plan->postCalleeNextBlock.empty() || plan->postCalleeScriptExit;
    if (!hasExecutableContinuation ||
        (plan->postCalleeScriptExit &&
         (!plan->calls.empty() || !plan->postCalleeNextBlock.empty())))
        return false;

    // A nonempty plan can still be invalid: a partially reconstructed
    // IF -> repeat stack may retain the IF while losing the repeat.
    // The native AfterLoopBody checkpoint is the authoritative phase/PC,
    // independently of the proposed interpreter control-frame vector.
    // Cross-check the EXACT innermost saved loop before releasing Fiber.
    const auto decoded = handoff.image
        ? handoff.image->decodeFrames() : std::nullopt;
    if (!decoded || decoded->empty() || !handoff.image->symbols)
        return false;
    const auto& leaf = decoded->back();
    if (leaf.phase == ir::SourceResumePhase::AfterLoopBody) {
        if (leaf.loops.empty() || plan->postCalleeScriptExit ||
            !plan->postCalleeNextBlock.empty() ||
            leaf.frame.resumePc != leaf.loops.back().sourcePc)
            return false;
        const auto savedLoop = handoff.image->symbols->block(
            leaf.loops.back().sourcePc);
        const auto& controls = plan->calls.empty()
            ? plan->rootControls : plan->calls.back().activeControls;
        if (!savedLoop || controls.empty() ||
            !controls.back().isLoop ||
            controls.back().blockId != *savedLoop)
            return false;
    }

    thread.stack.clear();
    thread.stackFrames.clear();
    thread.trashFrames.clear();
    // Root script lexical scopes precede its first procedure call.
    // Counted scopes carry the exact snapshot counter (in interpreter
    // latch units); conditional loops require no numeric counter.
    for (const auto& scope : plan->rootControls) {
        thread.pushStack(scope.blockId);
        auto* frame = thread.peekStackFrame();
        frame->isLoop = scope.isLoop;
        frame->executionContext = std::make_unique<ExecContext>();
        if (scope.isLoop) {
            frame->executionContext->hasLoopCounter =
                scope.remaining.has_value();
            frame->executionContext->loopCounter =
                scope.remaining.value_or(0.0);
        } else {
            frame->executionContext->executed = true;
        }
    }
    for (auto& restored : plan->calls) {
        thread.pushStack(restored.callBlockId);
        auto* call = thread.peekStackFrame();
        call->hasParams = true;
        call->params = std::move(restored.arguments);
        call->executionContext = std::make_unique<ExecContext>();
        call->executionContext->executed = true;
        // The original lexical ordering can interleave IF and LOOP frames:
        // procedure -> IF -> repeat -> child procedure, or
        // procedure -> repeat -> IF -> child procedure. Neither reporter
        // is re-evaluated; counted loops keep their committed counters.
        for (const auto& scope : restored.activeControls) {
            thread.pushStack(scope.blockId);
            auto* frame = thread.peekStackFrame();
            frame->isLoop = scope.isLoop;
            frame->executionContext = std::make_unique<ExecContext>();
            if (scope.isLoop) {
                frame->executionContext->hasLoopCounter =
                    scope.remaining.has_value();
                frame->executionContext->loopCounter =
                    scope.remaining.value_or(0.0);
            } else {
                frame->executionContext->executed = true;
            }
        }
    }
    if (!plan->postCalleeNextBlock.empty()) {
        // An AfterCallee NextBlock names the *next* statement, not the
        // already executed procedures_call. A fresh interpreter frame
        // starts there and preserves the parent argument maps below it.
        thread.pushStack(plan->postCalleeNextBlock);
    }

    // Nothing from the discarded native Fiber is needed to re-enter the
    // already committed repeat latch. The interpreter will see the same
    // variables and the exact original procedure arguments.
    FiberPool::release(thread.fiber);
    thread.fiber = nullptr;
    releaseJitCtx(thread.jitCtx);
    thread.jitCtx = nullptr;
    thread.nativeProcedureFrames.reset();
    thread.jitVarCache.clear();
    thread.jitVarSlots.clear();
    thread.isCompiled = false;
    thread.compiled = nullptr;
    thread.interpreterStarted = true;
    thread.status = plan->postCalleeScriptExit
        ? STATUS_DONE : STATUS_RUNNING;

    return true;
}

namespace {
/// インタプリタが実行したブロック数を数え、関数を抜けるときに
/// HotSpot のプロファイルへ加算する。JIT compat 評価（minDepth > 0）と
/// JIT 実行中は対象外。
struct InterpBlockCounter {
    Runtime* rt = nullptr;
    const void* top = nullptr;
    uint64_t n = 0;
    ~InterpBlockCounter() {
        if (n && rt && rt->jit && top)
            rt->jit->noteInterpBlocks(top, n);
    }
};
} // namespace

/// minDepth: この深さ以下のスタックは触らない（JIT compat 評価の境界）。
/// 0 = 従来通りスタックが空になるまで。
bool Sequencer::tryNestedWasm(Thread& thread,
                              const BlockCached& cached,
                              std::string& pastEndId) {
#if defined(__EMSCRIPTEN__) && defined(XYO_WASM_HOTSPOT)
    if (runtime.blockCountEnabled || !nestedJit_ || !thread.topBlockPtr || !thread.blockContainer ||
        thread.isCompiled || thread.isKilled)
        return false;
    // Eligible opcodes were classified when Blocks::getCachedExecute
    // constructed this immutable BlockCached. Avoid repeated string
    // comparisons in the 100M+ interpreted-block hot path.
    if (!cached.nestedWasmCandidate ||
        cached.nestedWasmRejected.load(std::memory_order_relaxed) ||
        (nestedWarpOnly_ && !cached.nestedWasmWarpCandidate))
        return false;
    const auto& op = cached.opcode;
    const StackFrame* frame = thread.peekStackFrame();
    if (!frame || frame->waitingReporter || frame->hasReported ||
        !frame->reporting.empty() || frame->executionContext)
        return false;
    if ((op == "control_repeat" ||
         op == "control_repeat_until" ||
         op == "control_while" ||
         op == "control_for_each") &&
        (!frame->warpMode || warpBudgetExhausted(thread)))
        return false;
    const Block* source = thread.blockContainer->getBlock(cached.id);
    if (!source) return false;
    bool permanentlyUnsupported = false;
    const bool executed = nestedJit_->execute(
        thread, source, pastEndId, &permanentlyUnsupported);
    if (permanentlyUnsupported)
        cached.nestedWasmRejected.store(true, std::memory_order_relaxed);
    return executed;
#else
    (void)thread;
    (void)cached;
    (void)pastEndId;
    return false;
#endif
}

void Sequencer::stepThreadBound(Thread& thread, size_t minDepth,
                                const std::string* stopAt, bool* yielded) {
    if (yielded) *yielded = false;
    const bool countBlocks =
        minDepth == 0 && !thread.isCompiled && runtime.jit &&
        thread.topBlockPtr != nullptr;
    InterpBlockCounter blockCounter{
        countBlocks ? &runtime : nullptr,
        countBlocks ? static_cast<const void*>(thread.topBlockPtr) : nullptr,
        0};
    std::string currentBlockId = thread.peekStack();
    if (currentBlockId.empty()) {
        thread.popStack();
        if (thread.stack.empty()) {
            thread.status = STATUS_DONE;
            return;
        }
    }
    while (thread.stack.size() > minDepth &&
           !(currentBlockId = thread.peekStack()).empty()) {
        // Safe OSR boundary: a nested reporter/procedure cannot be confused
        // with the next top-level statement even when IDs are reused.
        if (stopAt && thread.stack.size() == 1 &&
            currentBlockId == *stopAt) return;
        const size_t initialStackSize = thread.stack.size();
        bool isWarpMode = thread.peekStackFrame()->warpMode;
        if (isWarpMode && !thread.hasWarpTimer) {
            thread.hasWarpTimer = true;
            thread.warpTimer.start();
        }
        if (thread.target == nullptr) {
            retireThread(thread);
        } else {
            if (minDepth == 0) thread.interpreterStarted = true;
            thread.nestedWasmEligible =
                nestedJit_ && countBlocks && thread.stack.size() > 1 &&
                !thread.isKilled &&
                (!nestedWarpOnly_ || isWarpMode);
            const bool interpreted = execute(*this, thread);
            thread.nestedWasmEligible = false;
            if (countBlocks && interpreted) ++blockCounter.n;
        }
        thread.blockGlowInFrame = currentBlockId;

        if (thread.status == STATUS_YIELD) {
            thread.status = STATUS_RUNNING;
            // danger モードは時間予算が無いが、**明示 yield（待機系ブロック:
            // wait / wait until / broadcast and wait 等）は尊重する** — これらは
            // 他スレッドが進まないと条件が変わらず、回り続けても完了しない。
            if (isWarpMode && !runtime.dangerMode &&
                !warpBudgetExhausted(thread)) {
                continue;
            }
            if (yielded) *yielded = true;
            return;
        } else if (thread.status == STATUS_PROMISE_WAIT) {
            if (yielded) *yielded = true;
            return;
        } else if (thread.status == STATUS_YIELD_TICK) {
            if (yielded) *yielded = true;
            return;
        } else if (thread.status == STATUS_DONE) {
            return;
        }

        // 制御フローがなければ次ブロックへ
        if (thread.stack.size() == initialStackSize &&
            thread.peekStack() == currentBlockId &&
            !thread.peekStackFrame()->waitingReporter) {
            thread.goToNextBlock();
        }
        // スタック上を遡って次を探す
        while (thread.stack.size() > minDepth && thread.peekStack().empty()) {
            thread.popStack();
            if (thread.stack.empty()) {
                thread.status = STATUS_DONE;
                return;
            }
            if (thread.stack.size() <= minDepth) return;
            StackFrame* stackFrame = thread.peekStackFrame();
            isWarpMode = stackFrame->warpMode;

            if (stackFrame->isLoop) {
                if (!isWarpMode || warpBudgetExhausted(thread)) {
                    if (yielded) *yielded = true;
                    return;
                }
                continue;
            } else if (stackFrame->waitingReporter) {
                continue;
            }
            thread.goToNextBlock();
        }
    }
}

void Sequencer::stepToBranch(Thread& thread, int branchNum, bool isLoop) {
    if (!branchNum) branchNum = 1;
    const std::string currentBlockId = thread.peekStack();
    const std::string branchId =
        thread.target->blocks->getBranch(currentBlockId, branchNum);
    thread.peekStackFrame()->isLoop = isLoop;
    thread.pushStack(branchId);   // 空文字列 = JS null
}

void Sequencer::stepToProcedure(Thread& thread, const std::string& procedureCode) {
    const std::string definition =
        thread.target->blocks->getProcedureDefinition(procedureCode);
    if (definition.empty()) return;

    const Block* definitionBlock = thread.target->blocks->getBlock(definition);
    const BlockInput* cb = definitionBlock->inputs.find("custom_block");
    const Block* innerBlock = cb ? thread.target->blocks->getBlock(cb->block) : nullptr;
    const bool doWarp = innerBlock && innerBlock->mutation &&
                        innerBlock->mutation->warp();

    // Recursion detection inspects up to five ancestor blocks, including
    // hash lookups. Its result is only used for non-warp procedures. Keep it
    // before pushStack() so the inspected ancestors are identical to before.
    const bool isRecursive = !doWarp && thread.isRecursiveCall(procedureCode);
    thread.pushStack(definition);
    if (thread.peekStackFrame()->warpMode &&
        warpBudgetExhausted(thread)) {
        thread.status = STATUS_YIELD;
    } else if (doWarp) {
        thread.peekStackFrame()->warpMode = true;
    } else if (isRecursive) {
        thread.status = STATUS_YIELD;
    }
}

void Sequencer::numericStepThread(Thread& thread) {
    // Only explicitly attached, deoptimizable sequential-repeat numeric
    // scripts reach this path. The native/WASM function is provided by the
    // caller. No GC
    // pointer or nonnumeric Scratch Value enters generated code.
    ir::NumericThreadSession* session = thread.numericSession.get();
    if (!session || !thread.target || thread.isKilled ||
        thread.status == STATUS_DONE ||
        !runtime.isActiveThread(&thread)) {
        retireThread(thread);
        return;
    }

    // For warp execution, respect the same thread-local warp timer used by
    // the interpreter. Normal Scratch repeat yields after every iteration.
    if (session->warp && !thread.hasWarpTimer) {
        thread.hasWarpTimer = true;
        thread.warpTimer.start();
    }
    auto run = [&](uint64_t* slots, uint64_t* frame) -> uint64_t {
        return session->run(slots, frame);
    };
    const bool sourceChanged = !session->sourceUnchanged(thread);
    if (sourceChanged)
        session->continuation.rejectUnrunSlice(
            ir::NumericRollbackReason::SourceChanged);
    const auto result = sourceChanged
        ? ir::NumericSliceResult::Failed : ir::driveNumericKernel(
        *thread.target, *session->kernel, session->continuation,
        session->warp, run,
        [&] { return warpBudgetExhausted(thread); });
    if (result == ir::NumericSliceResult::Completed) {
        if (std::getenv("XYO_JIT_VERBOSE"))
            std::fprintf(stderr, "xyo-common-cfg: completed script=%s result=%.17g\n",
                thread.topBlock.c_str(),
                Value{session->continuation.resultBits()}.asNumberUnchecked());
        thread.numericSession.reset();
        thread.stack.clear();
        thread.status = STATUS_DONE;
        return;
    }
    if (result == ir::NumericSliceResult::Yielded) {
        session->progressed = true;
        thread.status = STATUS_RUNNING;
        return;
    }
    // No speculative writes from the failing slice were committed.
    // Restore the Scratch interpreter's repeat counter when earlier
    // iterations have already committed to the real Target.
    if (std::getenv("XYO_JIT_VERBOSE"))
        std::fprintf(stderr,
            "xyo-common-cfg: deopt script=%s committed=%d source_changed=%d"
            " reason=%s slices=%llu rollbacks=%llu\n",
            thread.topBlock.c_str(),
            int(session->continuation.hasCommittedSlice()),
            int(sourceChanged),
            ir::numericRollbackReasonName(session->continuation.rollbackReason()),
            static_cast<unsigned long long>(session->continuation.committedSlices()),
            static_cast<unsigned long long>(session->continuation.rolledBackSlices()));
    const bool restored = ir::restoreNumericInterpreter(thread, *session);
    thread.numericSession.reset();
    if (!restored) {
        // This should be unreachable for the restricted single-loop form.
        // Fail closed instead of replaying previously committed effects.
        retireThread(thread);
        return;
    }
    thread.isCompiled = false;
    thread.compiled = nullptr;
    thread.status = STATUS_RUNNING;
    stepThreadBound(thread, 0);
}

/// コンパイル済みスレッド: fiber 経由で生成コードを実行。
/// fiber の yield は「他スレッドへ譲る」チェックポイント（同一パスで再開）。
/// status は生成コード側のヘルパが必要時に設定する。
void Sequencer::jitStepThread(Thread& thread) {
    const auto* compiled = static_cast<const CompiledScript*>(thread.compiled);
    const auto finishNative = [&] {
        // Verify every interpreter callsite still exists in exactly the
        // selected order. LLVM owns only the final counted loop; all prefix
        // effects and caller suffixes remain with the original interpreter.
        bool nestedPathMatches = thread.nestedOsrActive &&
            compiled && compiled->nestedOsrEntry &&
            !compiled->nestedOsrCallPath.empty() &&
            compiled->nestedOsrCallPath.size() <= 8 &&
            compiled->nestedOsrFramePath.size() >=
                compiled->nestedOsrCallPath.size() &&
            compiled->nestedOsrFramePath.size() <= 14 &&
            compiled->nestedOsrBranchArms.size() ==
                compiled->nestedOsrFramePath.size() &&
            thread.stack.size() ==
                compiled->nestedOsrFramePath.size() + 1 &&
            thread.stack.size() == thread.stackFrames.size() &&
            thread.peekStack() == compiled->nestedOsrLoopId;
        if (nestedPathMatches) {
            for (size_t i = 0;
                 i < compiled->nestedOsrFramePath.size(); ++i) {
                if (thread.stack[i] != compiled->nestedOsrFramePath[i]) {
                    nestedPathMatches = false;
                    break;
                }
            }
        }
        if (nestedPathMatches && !thread.isKilled &&
            runtime.isActiveThread(&thread) &&
            thread.status != STATUS_DONE) {
            // This compiled entry executed exactly ONE source loop.
            // All original procedure callers, including their GC roots,
            // remain in the interpreter. Only advance the innermost loop.
            thread.nestedOsrActive = false;
            thread.osrActive = false;
            thread.osrRemaining = 0;
            thread.isCompiled = false;
            thread.compiled = nullptr;
            thread.status = STATUS_RUNNING;
            thread.goToNextBlock();
            // A loop that was the LAST statement of an IF arm or a
            // procedure has no NEXT block. The ordinary interpreter
            // performs the following empty-sentinel unwind *inside*
            // stepThreadBound's main loop. A native handoff returns to
            // stepThreadBound at its entry, which otherwise pops only
            // one sentinel and accidentally re-executes its parent IF.
            // Drain finished non-loop control ancestors here, exactly
            // once, before returning to the interpreter. Earlier source
            // effects/caller frames must never be replayed.
            while (!thread.stack.empty() && thread.peekStack().empty()) {
                thread.popStack();
                if (thread.stack.empty()) {
                    thread.status = STATUS_DONE;
                    break;
                }
                StackFrame* parent = thread.peekStackFrame();
                if (!parent || parent->waitingReporter) {
                    thread.status = STATUS_DONE;
                    thread.stack.clear();
                    thread.stackFrames.clear();
                    break;
                }
                if (parent->isLoop) {
                    // Resume the interpreter's existing counted loop latch.
                    // Its frame still owns the exact private repeat counter.
                    break;
                }
                thread.goToNextBlock();
            }
            if (std::getenv("XYO_JIT_OSR_TIMING"))
                std::fprintf(stderr,
                    "[jit-osr-nested] restored caller, next=%s\n",
                    thread.peekStack().c_str());
            return;
        }
        thread.nestedOsrActive = false;
        thread.osrActive = false;
        thread.status = STATUS_DONE;
        thread.stack.clear();
    };
    if (compiled && compiled->wasm &&
        !compiled->wasm->hybridRegions.empty()) {
        // Hybrid execution resumes the interpreter at exact *top-level*
        // Scratch block boundaries. Unsupported opcodes, recursive calls,
        // reporters, clone and wait keep their original interpreter stacks.
        const auto& regions = compiled->wasm->hybridRegions;
        if (thread.wasmRegionIndex == 0 &&
            thread.peekStack() == thread.topBlock)
            thread.reuseStackForNextBlock(regions.front().startId);
        for (uint32_t slice = 0;
             slice < 64 && thread.wasmRegionIndex < regions.size();
             ++slice) {
            const auto& region = regions[thread.wasmRegionIndex];
            if (thread.isKilled || thread.status == STATUS_DONE ||
                thread.stack.empty())
                return;
            if (!region.code) {
                // The interpreter owns every yield/warp budget decision.
                // A normalized STATUS_RUNNING still means 'yielded' when
                // the explicit flag is true: never cross that boundary in
                // the same scheduler pass.
                bool yielded = false;
                thread.isCompiled = false;
                thread.interpreterStarted = true;
                stepThreadBound(thread, 0, region.pastEndId.empty()
                    ? nullptr : &region.pastEndId, &yielded);
                if (thread.compiled != compiled || thread.isKilled ||
                    thread.status == STATUS_DONE) return;
                thread.isCompiled = true;
                if (yielded) return;
                if (region.pastEndId.empty()) return;
                if (thread.stack.size() != 1 ||
                    thread.peekStack() != region.pastEndId) return;
                ++thread.wasmRegionIndex;
                continue;
            }
            if (thread.stack.size() != 1 ||
                thread.peekStack() != region.startId ||
                wasm::executeHybridRegion(*region.code, thread) !=
                    wasm::HotResult::Completed) {
                // The failed segment did not publish speculative writes.
                // Interpreter resumes *this* statement, not the hat or
                // the already committed previous regions.
                thread.isCompiled = false;
                thread.compiled = nullptr;
                thread.interpreterStarted = true;
                stepThreadBound(thread, 0);
                return;
            }
            ++thread.wasmRegionIndex;
            if (region.pastEndId.empty()) {
                thread.status = STATUS_DONE;
                thread.stack.clear();
                return;
            }
            thread.reuseStackForNextBlock(region.pastEndId);
        }
        // Bound host handoffs; retain region index and continuation until
        // the scheduler next advances this Scratch thread.
        if (thread.wasmRegionIndex == regions.size()) {
            thread.status = STATUS_DONE;
            thread.stack.clear();
        }
        return;
    }
    if (compiled && compiled->wasm) {
        const wasm::HotResult result =
            wasm::executeHot(*compiled->wasm, thread);
        if (result == wasm::HotResult::Completed) {
            thread.status = STATUS_DONE;
            thread.stack.clear();
        } else if (result == wasm::HotResult::Yielded) {
            // A non-warp control_repeat yields after each completed body.
            // Remaining iterations resume through this compiled thread.
            thread.status = STATUS_RUNNING;
        } else {
            // Segment changes were transactional. On a mid-loop bailout,
            // executeHot reconstructed the interpreter at the exact repeat
            // or suffix continuation, preserving previously committed work.
            thread.isCompiled = false;
            thread.compiled = nullptr;
            thread.interpreterStarted = true; // never replay the original hat
            stepThreadBound(thread, 0);
        }
        return;
    }
    JitCtx* c = thread.jitCtx;
    if (!c) {
        const CompiledScript* cs =
            static_cast<const CompiledScript*>(thread.compiled);
        if (!cs || !cs->entry) {
            // コンパイル失敗/破棄 — インタプリタへフォールバック
            thread.isCompiled = false;
            stepThreadBound(thread, 0);
            return;
        }
        c = acquireJitCtx();
        c->thread = &thread;
        c->target = thread.target;
        c->runtime = &runtime;
        c->blockCounter = runtime.blockCountEnabled
            ? &runtime.executedBlocks : nullptr;
        c->sequencer = this;
        c->compatEc = &thread.compatibilityStackFrame;
        if (thread.osrActive)
            c->osrRepeatRemaining = thread.osrRemaining;
        if (thread.nestedOsrActive) c->isWarp = 1;
        thread.jitCtx = c;
        if (!cs->mayYield) {
            // yield 点を持たないスクリプト — fiber を介さず同期実行。
            // broadcast 受信等の短命スレッドの切替コストを削る。
#if !defined(XYO_DISABLE_RT_PROF)
            std::optional<ProfScope> ps;
            if (prof().enabled) { ps.emplace(&prof().syncNs); ++prof().nSync; }
#endif
            c->fiber = nullptr;
            c->warpStartMs = runtime.currentMSecs;
            (thread.nestedOsrActive ? cs->nestedOsrEntry :
             thread.osrActive ? cs->osrEntry : cs->entry)(c);
            finishNative();
            releaseJitCtx(c);
            thread.jitCtx = nullptr;
            return;
        }
        thread.fiber = FiberPool::acquire(jitEntry, c);
        c->fiber = thread.fiber;
    }
    // Bounded post-native-yield deopt. The Fiber is suspended precisely
    // AFTER a complete counted warp loop body and its promoted-variable
    // spill. If the source changed while Scratch/editor had control,
    // discard the suspended LLVM stack and let the original interpreter
    // continue at the real counted-loop latch. This never executes a stale
    // native instruction after the mutation, nor replays committed effects.
    if (compiled && compiled->nestedOsrInFlightGuard &&
        compiled->nestedOsrSourceProofRequired &&
        thread.nestedOsrActive && c->fiber) {
        Blocks* blocks = thread.blockContainer;
        const Block* root = blocks
            ? blocks->getBlock(thread.topBlock) : nullptr;
        const auto& frozen = compiled->nestedOsrSourceSymbols;
        const bool stillSame = root && frozen &&
            blocks->revision() == frozen->sourceRevision &&
            [&] {
                const auto fresh =
                    ir::ScratchCompilationUnit::capture(*blocks, root);
                return fresh.valid() &&
                    fresh.sourceRevision == frozen->sourceRevision &&
                    ir::scratchSourceDigest(fresh) == frozen->sourceDigest;
            }();
        if (!stillSame) {
            StackFrame* inner = thread.peekStackFrame();
            ExecContext* ec = inner && inner->executionContext
                ? inner->executionContext.get() : nullptr;
            const double remaining = c->nestedOsrDeoptRemaining;
            const bool safe = c->nestedOsrDeoptReady == 1 &&
                !thread.isKilled &&
                runtime.isActiveThread(&thread) &&
                thread.peekStack() == compiled->nestedOsrLoopId &&
                thread.stack.size() == thread.stackFrames.size() &&
                ec && inner->isLoop && ec->hasLoopCounter &&
                std::isfinite(remaining) && remaining >= 0.0 &&
                remaining <= 1099511627776.0 &&
                std::floor(remaining) == remaining;
            // No resumed LLVM execution after any source change. Drop
            // the suspended fiber first; the original interpreter frames
            // and argument GC roots remain independently alive.
            FiberPool::release(c->fiber);
            thread.fiber = nullptr;
            releaseJitCtx(c);
            thread.jitCtx = nullptr;
            thread.nestedOsrActive = false;
            thread.osrActive = false;
            thread.osrRemaining = 0.0;
            thread.isCompiled = false;
            thread.compiled = nullptr;
            if (!safe) {
                // An unprovable frame must not replay effects.
                retireThread(thread);
                return;
            }
            ec->loopCounter = remaining;
            thread.status = STATUS_RUNNING;
            if (std::getenv("XYO_JIT_OSR_NESTED_TRACE"))
                std::fprintf(stderr,
                    "[jit-osr-inflight] deopt remaining=%.0f "
                    "source-changed=1; interpreter owns original loop\\n",
                    remaining);
            return;
        }
    }
    c->target = thread.target;
    c->sampledWarpClockUs = sampledWarpClock();
    c->sampledWarpDeadlineUs = sampledWarpDeadline();
    c->sampledUnixMs = sampledUnixClock();
    c->sampledUnixDaysBits = sampledUnixDays();
#if !defined(XYO_DISABLE_RT_PROF)
    std::optional<ProfScope> ps;
    if (prof().enabled) { ps.emplace(&prof().fiberNs); ++prof().nFiber; }
#endif
    c->fiber->resume();
    if (c->fiber->finished() || thread.status == STATUS_DONE ||
        thread.isKilled || !runtime.isActiveThread(&thread)) {
        // A thread may be restarted or stopped while it is the currently
        // executing fiber (e.g. a broadcast restarting its own hat). It
        // becomes unreachable after this resume returns even if it yielded:
        // return the suspended fiber to the pool instead of leaking its stack.
        // エントリ関数が戻った = スクリプト完了。Thread オブジェクトは
        // ownedThreads で ~Runtime まで生存するため、fiber/jitCtx は
        // ここで即座にプールへ戻す（broadcast 等の大量スレッド生成対策）。
        finishNative();
        FiberPool::release(c->fiber);
        thread.fiber = nullptr;
        releaseJitCtx(c);
        thread.jitCtx = nullptr;
    } else if (thread.status == STATUS_YIELD) {
        // パス内 yield（`u.yield()` 相当）は STATUS_RUNNING に戻す — JS の
        // stepThread も stepThreadBound も yield から戻るときに同じことを
        // する。YIELD のままにすると「このパスで動けるスレッド」に数えられず
        // (stepThreads の numActiveThreads)、1 スレッドだけのプロジェクトでは
        // フレームが即座に終わって yield が 1 tick 待ちになる。broadcast and
        // wait / wait until がこれで 1 フレーム 1 回になっていた
        // (sched_pingpong が scratch-vm の 1/2000)。
        thread.status = STATUS_RUNNING;
    }
}

void Sequencer::retireThread(Thread& thread) {
    thread.numericSession.reset();
    // JS 版は stack のみクリアし stackFrames は残す（edge-activated hat が
    // retire 直後に goToNextBlock → reuseStackForNextBlock で最終フレームを
    // 再利用するため）。
    thread.stack.clear();
    thread.requestScriptGlowInFrame = false;
    thread.status = STATUS_DONE;
    // 現在実行中の fiber 自身の解放はできない — ~Thread に委ねる。
    // 中断したままの他 fiber は status DONE で二度と resume されないので
    // ここで解放してよい。
    if (thread.fiber && thread.fiber != currentFiber()) {
        FiberPool::release(thread.fiber);
        thread.fiber = nullptr;
        releaseJitCtx(thread.jitCtx);
        thread.jitCtx = nullptr;
    } else if (thread.jitCtx && thread.fiber != currentFiber()) {
        // fiber 無し・ctx あり（通常あり得ないが安全のため）
        releaseJitCtx(thread.jitCtx);
        thread.jitCtx = nullptr;
    }
}

// --- 並列実行（deps 解析 → 衝突グラフ → グループ並列）---------------------

bool Sequencer::parallelEnabled() const {
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    return false;
#else
    // An explicit source-handoff probe needs the serial scheduler safe point,
    // never a parallel worker that might race GC root allocation.
    const char* handoff = std::getenv("XYO_RT_SOURCE_HANDOFF_PROBE");
    const char* deopt = std::getenv("XYO_RT_NATIVE_DEOPT_ONCE");
    if ((handoff && handoff[0] == '1') ||
        (deopt && deopt[0] == '1')) return false;
    static const bool enabled = std::getenv("XYO_PAR") == nullptr ||
                                std::string(std::getenv("XYO_PAR")) != "0";
    // Affinity is a per-script effect, not a property of the whole Runtime.
    // Graphical/IO-mutating scripts remain serial barriers; independent pure
    // computation may run on workers while a renderer is attached.
    return enabled && runtime.jit;
#endif
}

/// このスレッドを並列バッチへ入れてよいか。
/// 条件: JIT 済み・依存解析が spawn/unknown を含まない。
/// fiber スレッド（yield あり）も可 — fiber yield は呼出し側
/// (= worker) のスタックへ戻るだけなので、中断中のスレッドは
/// 他スレッドと並行しても逐次実行と同じ可視性になる。
bool Sequencer::parallelEligible(Thread& t) const {
    return parallelAccessFor(t) != nullptr;
}

const AccessSet* Sequencer::parallelAccessFor(Thread& t) const {
    if (!runtime.jit || !t.isCompiled || !t.target || t.stackClick || t.updateMonitor)
        return nullptr;
    // パスごとに 2 段の unordered_map を引かないようスレッド側に持つ。
    if (t.accessResolved && t.accessGen == runtime.jit->accessGeneration())
        return t.accessCache;
    auto* cs = static_cast<const CompiledScript*>(t.compiled);
    // GPU helpers may invoke the renderer or shared compute backends, even in
    // a script whose Scratch-level effects are purely numeric.
    if (!cs || !cs->entry || cs->wasm ||
        cs->mainThread || !cs->gpuProgs.empty()) return nullptr;
    const AccessSet* as = runtime.jit->accessFor(t, runtime);
    const AccessSet* res = as && as->workerSafe() ? as : nullptr;
    t.accessCache = res;
    t.accessResolved = true;
    t.accessGen = runtime.jit->accessGeneration();
    return res;
}

namespace {
struct ParGroupCtx {
    Sequencer* seq;
    const std::vector<Thread*>* batch;
    const std::vector<size_t>* group;
};
void runGroup(void* p) {
    auto* c = static_cast<ParGroupCtx*>(p);
    for (size_t idx : *c->group)
        c->seq->jitStepThread(*(*c->batch)[idx]);
}
} // namespace

/// 溜まった適格スレッドを実行し、実行後の帳簿を返す。
/// 衝突グラフの連結成分ごとにグループ化し、独立グループをワーカー
/// プールで並列実行する（グループ内はバッチ順 = spawn 順を保持）。
Sequencer::ParFlush Sequencer::flushParBatch() {
    ParFlush r;
    if (parBatch_.empty()) return r;
    // Retain batch storage across scheduler passes; no eligible script can
    // spawn/re-enter the scheduler while this batch is being drained.
    auto& batch = parBatch_;
    auto& sets = parSets_;

    auto finish = [&]() -> ParFlush {
        // Parallel batches only contain newly entered event waits; resumed
        // waiters are forced onto the serial path by stepThreads().
        if (eventSchedulerEnabled())
            eventProgressEpoch.fetch_add(1, std::memory_order_relaxed);
        for (Thread* t : batch) {
            if (t->status == STATUS_RUNNING) ++r.active;
            if (t->stack.empty() || t->status == STATUS_DONE)
                r.stopped = true;
        }
        batch.clear();
        sets.clear();
        return r;
    };

    if (batch.size() == 1) {
        jitStepThread(*batch[0]);
        return finish();
    }

    // A common writer key makes the entire batch a single conflict component.
    // This is especially common for cloned scripts updating a stage variable:
    // building the writer index, union/find and group vectors every pass would
    // only prove that they must all run serially anyway.
    // Keep the full graph path when explicitly requested for diagnostics.
    static const char* graphDump = std::getenv("XYO_DEPGRAPH");
    if (!graphDump || !*graphDump) {
        for (const MemKey& key : sets.front()->writes) {
            bool allConflict = true;
            for (size_t i = 1; i < sets.size(); ++i) {
                if (!sets[i]->writes.contains(key) &&
                    !sets[i]->reads.contains(key)) {
                    allConflict = false;
                    break;
                }
            }
            if (!allConflict) continue;

            ++parStats_.batches;
            parStats_.threads += batch.size();
            ++parStats_.groups;
#if !defined(XYO_DISABLE_RT_PROF)
            if (prof().enabled) {
                prof().parThreads += batch.size();
                ++prof().parGroups;
                ++prof().parBatches;
            }
#endif
            // All scripts are serialized anyway. Once their fibers have
            // started, run a direct fiber-to-fiber round-robin pass. Newborn
            // scripts and profiler runs retain the ordinary scheduler.
            static const bool chainEnabled = [] {
                const char* env = std::getenv("XYO_FIBER_CHAIN");
                return !env || env[0] != '0';
            }();
            bool chained = chainEnabled && batch.size() >= 2;
#if !defined(XYO_DISABLE_RT_PROF)
            chained = chained && !prof().enabled;
#endif
            auto& fibers = parFibers_;
            fibers.clear();
            if (chained) {
                for (Thread* t : batch) {
                    // A preceding serial script may have replaced a receiver.
                    // Validate identity, not hat type. Within this worker-safe
                    // batch no script can spawn/restart/stop another thread;
                    // serial barriers are always flushed before entering it.
                    if (!t->isCompiled || !t->jitCtx || !t->fiber ||
                        t->fiber->finished() || t->isKilled ||
                        !runtime.isActiveThread(t) ||
                        (t->status != STATUS_RUNNING && t->status != STATUS_YIELD)) {
                        chained = false;
                        break;
                    }
                    t->jitCtx->target = t->target;
                    t->jitCtx->sampledWarpClockUs = sampledWarpClock();
                    t->jitCtx->sampledWarpDeadlineUs = sampledWarpDeadline();
                    t->jitCtx->sampledUnixMs = sampledUnixClock();
                    t->jitCtx->sampledUnixDaysBits = sampledUnixDays();
                    fibers.push_back(t->fiber);
                }
            }
            if (chained) {
                ++parStats_.chainedPasses;
                Fiber::resumeChain(fibers.data(), fibers.size());
                for (Thread* t : batch) {
                    JitCtx* c = t->jitCtx;
                    if (t->fiber->finished() || t->status == STATUS_DONE ||
                        t->isKilled || !runtime.isActiveThread(t)) {
                        t->status = STATUS_DONE;
                        t->stack.clear();
                        FiberPool::release(c->fiber);
                        t->fiber = nullptr;
                        releaseJitCtx(c);
                        t->jitCtx = nullptr;
                    } else if (t->status == STATUS_YIELD) {
                        t->status = STATUS_RUNNING;
                    }
                }
            } else {
                for (Thread* t : batch) jitStepThread(*t);
            }
            return finish();
        }
    }

    std::vector<std::vector<size_t>> groups;
    if (graphDump && *graphDump) {
        // Full edges and their human-readable reasons are diagnostic work.
        DepGraph g = buildDepGraph(sets);
        dumpDepGraph(g, batch);
        groups = g.groups();
    } else {
        groups = buildDepGroups(sets);
    }
    ++parStats_.batches;
    parStats_.threads += batch.size();
    parStats_.groups += groups.size();
#if !defined(XYO_DISABLE_RT_PROF)
    if (prof().enabled) {
        prof().parThreads += batch.size();
        prof().parGroups += groups.size();
        ++prof().parBatches;
    }
#endif
    if (groups.size() <= 1) {
        // 全員が同一衝突成分 — バッチ順に直列
        for (Thread* t : batch) jitStepThread(*t);
        return finish();
    }

    if (!parPool_) parPool_ = std::make_unique<WorkerPool>();
    std::vector<WorkerPool::Task> tasks(groups.size());
    std::vector<ParGroupCtx> ctxs(groups.size());
    for (size_t i = 0; i < groups.size(); ++i) {
        ctxs[i] = ParGroupCtx{this, &batch, &groups[i]};
        tasks[i] = WorkerPool::Task{&runGroup, &ctxs[i]};
    }
    // worker スレッド上で Gc::collect が走らないよう collect を遅延。
    // 走査中に変数/SList が生えても sweep がルートを壊さないよう、
    // 並列区間は collect を起こさずフェーズ終了後に必要なら回収する。
    bool canCollect;
    {
        Gc::DeferCollect defer;
        canCollect = !defer.prev;
        ++parStats_.dispatches;
        parPool_->run(tasks.data(), tasks.size());
    }
    if (canCollect) Gc::get().collectIfNeeded();
    return finish();
}

/// XYO_DEPGRAPH=<dir> で各並列バッチの衝突グラフを DOT 出力
/// （dir/stepNNNNN.dot）。"-" なら stderr。
void Sequencer::dumpDepGraph(const DepGraph& g,
                             const std::vector<Thread*>& batch) {
    static const char* dir = std::getenv("XYO_DEPGRAPH");
    if (!dir || !*dir) return;
    std::vector<std::string> names(batch.size());
    for (size_t i = 0; i < batch.size(); ++i) {
        Thread* t = batch[i];
        std::string op = "?";
        if (t->blockContainer) {
            if (const Block* b = t->blockContainer->getBlock(t->topBlock))
                op = b->opcode;
        }
        names[i] = SStr::toUtf8(t->target ? t->target->getName()
                                          : std::u16string(u"?")) +
                   ":" + op;
    }
    std::string dot = depGraphToDot(g, names);
    if (dir[0] == '-' && dir[1] == '\0') {
        std::fprintf(stderr, "--- depgraph (batch %llu) ---\n%s",
                     (unsigned long long)batch.size(), dot.c_str());
        return;
    }
    char path[512];
    std::snprintf(path, sizeof path, "%s/step%05d.dot", dir,
                  depGraphSeq_++);
    if (FILE* f = std::fopen(path, "w")) {
        std::fwrite(dot.data(), 1, dot.size(), f);
        std::fclose(f);
    }
}

} // namespace xyo
