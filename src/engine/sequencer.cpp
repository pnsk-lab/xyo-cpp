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
#include "../jit/layq.hpp"
#include "../jit/rt_rooted_ssa_arguments.hpp"
#include "../jit/rt_generated_ssa_roots.hpp"
#include "../jit/deopt_telemetry.hpp"
#include "../ir/scratch_pipeline.hpp"
#include "../ir/scratch_source_digest.hpp"
#include "../ir/source_typed_ssa_bridge.hpp"
#if defined(XYO_JIT)
// This compiler-only SSA reporter audit pulls LLVM IR headers, which must
// not be instantiated or linked in a native no-JIT build.
#include "../ir/source_reporter_ssa_location_bridge.hpp"
#include "../ir/source_reporter_storage_owner_audit.hpp"
#include "../ir/source_reporter_input_slot_audit.hpp"
#include "../ir/source_reported_entry_destination_audit.hpp"
#include "../ir/source_scalar_prefix_audit.hpp"
#include "../ir/source_reported_entry_prefix_plan.hpp"
#include "../ir/source_reporter_input_roots.hpp"
#include "../ir/source_list_read_index_trace.hpp"
#endif
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
    // LAYQ diagnostics: per-thread poll/handoff/quantum summary. Only
    // threads that actually polled print, so scheduler-churn workloads
    // (millions of short threads) stay quiet.
    if (layqStatsEnabled() && (c->layqPolls != 0 || c->layqTaken != 0)) {
        std::fprintf(stderr,
                     "[layq] polls=%llu taken=%llu avgQ=%.1f\n",
                     (unsigned long long)c->layqPolls,
                     (unsigned long long)c->layqTaken,
                     c->layqPolls ? (double)c->layqQSum / (double)c->layqPolls
                                  : 0.0);
    }
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

// Decide whether a runnable thread should execute serially, join a worker
// batch, or remain asleep on its proven event dependency. Parallel batches
// are flushed before any serial effect to preserve Scratch's spawn order.
Sequencer::ParFlush Sequencer::dispatchReadyThread(
    Thread& thread, bool eventDriven, bool isolatedWatch,
    bool allowParallel) {
    ParFlush outcome;
    Thread* activeThread = &thread;
    const bool wasEventWait =
        (eventDriven || isolatedWatch) &&
        activeThread->eventWaitArmed;
    const uintptr_t oldWaitSite = activeThread->eventWaitSite;
    const uint64_t oldWaitExit = activeThread->eventWaitExitCount;
    static const bool auditWaitNotifications = [] {
        const char* env = std::getenv("XYO_WAIT_NOTIFY_AUDIT");
        return env && env[0] == '1';
    }();
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
        if (auditWaitNotifications && watchValid)
            runtime.auditWaitDependencyDecision(*activeThread);
        if (watchUnchanged || epochUnchanged) {
            ++eventSkippedPasses_;
            return {};
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
        outcome.stopped |= fr.stopped;
        outcome.active += fr.active;
        // Snapshot the notification before the native wait completes:
        // xyo_wait_event_done() unregisters the watcher during stepThread().
        // This is diagnostic only and never changes the scheduler decision.
        const bool notifiedAtResume = auditWaitNotifications && wasEventWait &&
            runtime.waitNotificationPending(*activeThread);
        stepThread(*activeThread);
        if (auditWaitNotifications && wasEventWait) {
            const WaitResumeState before{true, oldWaitSite, oldWaitExit};
            const WaitResumeState after{
                activeThread->eventWaitArmed,
                activeThread->eventWaitSite,
                activeThread->eventWaitExitCount};
            runtime.auditWaitResumeOutcome(
                classifyWaitResume(before, after), notifiedAtResume);
        }
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
            ++outcome.active;
        if (activeThread->stack.empty() ||
            activeThread->status == STATUS_DONE) {
            outcome.stopped = true;
        }
    }
    activeThread->hasWarpTimer = false;
    return outcome;
}

// Preserve active-list order while retiring completed script identities.
// Thread reclamation remains owned by Runtime, not this compaction pass.
void Sequencer::compactFinishedThreads(std::vector<Thread*>& doneThreads) {
    size_t nextActive = 0;
    for (size_t i = 0; i < runtime.threads.size(); ++i) {
        Thread* t = runtime.threads[i];
        if (!t->stack.empty() && t->status != STATUS_DONE) {
            t->runtimeIndex = nextActive;
            runtime.threads[nextActive++] = t;
        } else {
            // Some interpreter stop paths set STATUS_DONE directly without
            // calling retireThread. Remove reverse watches before this
            // identity is reclaimed or recycled.
            runtime.untrackWaitDependencies(*t);
            runtime.threadMap.erase(t->getId());
            t->releaseFinished();
            doneThreads.push_back(t);
        }
    }
    runtime.threads.resize(nextActive);
    runtime.invalidateSchedulerReadySet();
}

// Execute exactly one Scratch scheduler pass. Iteration deliberately observes
// append/restart operations performed by a running receiver. The pass itself
// owns completion compaction; the outer tick loop owns time and redraw policy.
size_t Sequencer::runSchedulerPass(
    bool ranFirstTick, bool eventDriven, bool isolatedWatch,
    bool allowParallel, std::vector<Thread*>& doneThreads) {
        size_t numActiveThreads = 0;
        bool stoppedThread = false;
        // JS: const threads = this.runtime.threads — パス開始時に掴む。
        // stopAll で差し替わっても stepping 経由で旧配列を反復する。
        stepping = &runtime.threads;
        // Debug-only safety net for direct Target::variables edits. Normal
        // scheduling never scans the reverse dependency registry.
        static const bool verifyWaitBindings = [] {
            const char* env = std::getenv("XYO_WAIT_REGISTRY_VERIFY");
            return env && env[0] == '1';
        }();
        if (verifyWaitBindings)
            runtime.validateWaitDependencyBindings();
        static const bool indexedReady = [] {
            const char* env = std::getenv("XYO_SCHED_READY_INDEX");
            return env && env[0] == '1';
        }();
        // Experimental wait-ready suppression is distinct from the normal
        // Ready Index. Require a reverse writer registry, and keep all other
        // waiters in the conservative live scan.
        static const bool waitSleepRequested = [] {
            const char* flag = std::getenv("XYO_WAIT_READY_SLEEP");
            const char* registry = std::getenv("XYO_WAIT_WAKE_REGISTRY");
            return flag && flag[0] == '1' &&
                   registry && registry[0] == '1';
        }();
        const bool waitReadySleep = indexedReady && waitSleepRequested &&
            (eventDriven || isolatedWatch);
        // Always revisit suppressed waiters when the host enters a new
        // Runtime::step frame, and once every 256 passes even if a caller
        // steps this scheduler directly without advancing stepSerial.
        // A source edit, unchecked host write or cache invalidation can
        // therefore delay a wake but can never permanently strand a waiter.
        const bool periodicRecheck = waitReadySleep && hasDeferredWaiters_ &&
            (waitReadyLastFrame_ != runtime.stepSerial ||
             (++waitReadyPassSerial_ & 255u) == 0);
        if (periodicRecheck) {
            waitReadyLastFrame_ = runtime.stepSerial;
            ++waitReadyStats_.frameRechecks;
        }
        // Capture epochs *before* draining changes. A publication racing
        // reconstruction will then change the epoch and force the ordinary
        // live scan, rather than silently excluding a newly woken thread.
        const uint64_t mutationAtStart =
            runtime.schedulerMutationEpoch.load(std::memory_order_relaxed);
        const uint64_t structureAtStart =
            runtime.schedulerStructureEpoch.load(std::memory_order_relaxed);
        if (indexedReady &&
            (periodicRecheck ||
             readySlotsEpoch_ != mutationAtStart ||
             readySlotsStructureEpoch_ != structureAtStart ||
             readySlotsLength_ != stepping->size())) {
            bool rebuild = periodicRecheck ||
                readySlotsStructureEpoch_ != structureAtStart ||
                readySlotsLength_ != stepping->size() ||
                readySlotsEpoch_ == UINT64_MAX;
            const auto changes = runtime.takeSchedulerStatusChanges();
            if (!rebuild) {
                for (Thread* thread : changes) {
                    const size_t index = thread->runtimeIndex;
                    if (index >= stepping->size() ||
                        (*stepping)[index] != thread) {
                        // Another writer touched the public thread array
                        // without publishing a structural mutation.
                        rebuild = true;
                        break;
                    }
                    const auto at = std::lower_bound(
                        readySlots_.begin(), readySlots_.end(), index);
                    if (thread->status == STATUS_PROMISE_WAIT) {
                        if (at != readySlots_.end() && *at == index)
                            readySlots_.erase(at);
                    } else if (at == readySlots_.end() || *at != index) {
                        // DONE must also be included so the completion
                        // compactor can observe a previously sleeping slot.
                        readySlots_.insert(at, index);
                    }
                }
                if (waitReadySleep && hasDeferredWaiters_ && !rebuild) {
                    // Skip the registry mutex entirely until some native
                    // wait has actually been omitted from readySlots_.
                    // A native JIT store records a waiting Thread identity,
                    // even if no status transition occurred. Read but DO
                    // NOT drain it: the resume audit still needs that bit.
                    for (Thread* thread : runtime.snapshotWaitWakeups()) {
                        const size_t index = thread->runtimeIndex;
                        if (index >= stepping->size() ||
                            (*stepping)[index] != thread) {
                            rebuild = true;
                            break;
                        }
                        if (thread->status == STATUS_PROMISE_WAIT) continue;
                        const auto at = std::lower_bound(
                            readySlots_.begin(), readySlots_.end(), index);
                        if (at == readySlots_.end() || *at != index)
                            readySlots_.insert(at, index);
                    }
                }
            }
            if (rebuild) {
                // Every native waiter goes back in the rebuilt index.
                // It may be suppressed again after this pass proves it quiet.
                hasDeferredWaiters_ = false;
                readySlots_.clear();
                readySlots_.reserve(stepping->size());
                for (size_t i = 0; i < stepping->size(); ++i) {
                    if ((*stepping)[i]->status != STATUS_PROMISE_WAIT)
                        readySlots_.push_back(i);
                }
            }
            readySlotsEpoch_ = mutationAtStart;
            readySlotsStructureEpoch_ = structureAtStart;
            readySlotsLength_ = stepping->size();
        }
        // The experimental reverse index may also omit pure native waits
        // whose tracked values and source/binding stamp are unchanged.
        // The regular Promise-only index retains its original threshold.
        const bool sparsePass = indexedReady &&
            readySlotsLength_ >= (waitReadySleep ? 16u : 32u) &&
            readySlots_.size() * 2 <= readySlotsLength_;
        if (waitReadySleep && sparsePass) ++waitReadyStats_.sparsePasses;
        if (waitReadySleep) waitSleepCandidates_.clear();
        size_t nextIndex = 0;
        size_t sparseCursor = 0;
        for (;;) {
            size_t i;
            if (sparsePass &&
                readySlotsEpoch_ == runtime.schedulerMutationEpoch.load(std::memory_order_relaxed) &&
                readySlotsLength_ == stepping->size()) {
                // At most one execution per slot per pass, in Scratch order.
                while (sparseCursor < readySlots_.size() &&
                       readySlots_[sparseCursor] < nextIndex)
                    ++sparseCursor;
                if (sparseCursor == readySlots_.size()) break;
                i = readySlots_[sparseCursor++];
            } else {
                // A broadcast, promise resolution, or thread mutation can
                // make a later slot ready. Resume the ordinary live scan.
                if (nextIndex >= stepping->size()) break;
                i = nextIndex;
            }
            nextIndex = i + 1;
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
                const bool wasPureWait = waitReadySleep &&
                    activeThread->eventWaitArmed &&
                    activeThread->eventWatchValid;
                if (wasPureWait) ++waitReadyStats_.scannedWaiters;
                const ParFlush result = dispatchReadyThread(
                    *activeThread, eventDriven, isolatedWatch, allowParallel);
                stoppedThread |= result.stopped;
                numActiveThreads += result.active;
                if (wasPureWait && activeThread->eventWaitArmed &&
                    activeThread->eventWatchValid &&
                    activeThread->status == STATUS_RUNNING &&
                    i < stepping->size() && (*stepping)[i] == activeThread &&
                    canSkipEventWaitByDependency(*activeThread) &&
                    !runtime.waitNotificationPending(*activeThread)) {
                    // Defer erasure until after iteration: sparseCursor
                    // must not observe a mutated sorted vector mid-pass.
                    waitSleepCandidates_.push_back(i);
                }
            }
        }
        {
            ParFlush fr = flushParBatch();
            stoppedThread |= fr.stopped;
            numActiveThreads += fr.active;
        }
        // Suppress only *proven* quiet native waits, and only if no writer
        // or structural mutation raced our pass. The first pass of every
        // host frame and a periodic pass will examine them again. A watched
        // write bumps mutationEpoch and re-inserts its notified Thread.
        if (waitReadySleep && !waitSleepCandidates_.empty() &&
            readySlotsEpoch_ ==
                runtime.schedulerMutationEpoch.load(std::memory_order_relaxed) &&
            readySlotsStructureEpoch_ ==
                runtime.schedulerStructureEpoch.load(std::memory_order_relaxed) &&
            readySlotsLength_ == stepping->size()) {
            for (size_t index : waitSleepCandidates_) {
                auto at = std::lower_bound(
                    readySlots_.begin(), readySlots_.end(), index);
                if (at != readySlots_.end() && *at == index) {
                    readySlots_.erase(at);
                    hasDeferredWaiters_ = true;
                    ++waitReadyStats_.deferredSlots;
                }
            }
        }
        if (stoppedThread) compactFinishedThreads(doneThreads);
        // LAYQ v2 push: publish the recommended pure-class quantum derived
        // from this pass's still-runnable count, but only when it changes —
        // steady state performs zero stores, so JIT readers keep a shared
        // L1 line. Only adaptive JIT reads the hint (fixed pins its own
        // constant; off needs nothing), so publish only in adaptive mode.
        // Release store; relaxed JIT loads (see layq.hpp contract). Danger
        // pins the hint to the pure cap: polls can never fire for time
        // budgets there, so contention-driven shrinking would only add
        // overhead.
        if (layqMode() == LayqMode::Adaptive) {
            const LayqConfig layqCfg = layqConfig();
            const uint64_t competitors =
                layqCompetitors((uint64_t)numActiveThreads);
            uint64_t target = layqTarget(competitors, layqCfg.qmaxPure,
                                         layqCfg.qmin);
            if (runtime.dangerMode) target = layqCfg.qmaxPure;
            const auto qt = target > 0xFFFFFFFFu
                ? 0xFFFFFFFFu
                : static_cast<uint32_t>(target);
            static const bool layqTrace = std::getenv("XYO_YIELD_QTRACE") !=
                nullptr;
            if (layqTrace)
                std::fprintf(stderr,
                             "[layq-pub] active=%zu competitors=%llu "
                             "target=%llu\n",
                             numActiveThreads,
                             (unsigned long long)competitors,
                             (unsigned long long)target);
            if (runtime.yieldQuantumHint.load(std::memory_order_relaxed) !=
                qt)
                runtime.yieldQuantumHint.store(qt, std::memory_order_release);
        }
        return numActiveThreads;
}

std::vector<Thread*> Sequencer::stepThreads() {
    std::vector<Thread*> doneThreads;
    // Preserve the opt-in index across ticks. Runtime's mutation epoch
    // invalidates it on wakeups, script mutations and completion compaction.
    // Diagnostic-only override for isolating scheduler time budgeting from
    // the cost of visiting actual Scratch threads. The default remains
    // TurboWarp's 75% of currentStepTime, independent of CLI frame pacing.
    // A smaller budget reduces work per frame; it is NOT a free speedup.
    static const double explicitWorkBudgetMs = [] {
        const char* setting = std::getenv("XYO_SCHED_WORK_BUDGET_MS");
        if (!setting || !*setting) return 0.0;
        char* end = nullptr;
        const double ms = std::strtod(setting, &end);
        return end != setting && end && !*end &&
            std::isfinite(ms) && ms >= 0.5 && ms <= 250.0 ? ms : 0.0;
    }();
    const double WORK_TIME = explicitWorkBudgetMs > 0.0
        ? explicitWorkBudgetMs : 0.75 * runtime.currentStepTime;
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
        numActiveThreads = runSchedulerPass(
            ranFirstTick, eventDriven, isolatedWatch, allowParallel,
            doneThreads);
        ranFirstTick = true;
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
    // A collection postponed while another native Fiber owned unrooted
    // SSA can be drained only after that last Fiber retires. Do not
    // poll the collector in the common no-pending scheduler path.
    if (Gc::get().explicitCollectionPending()) {
        const auto collectionsBefore = Gc::get().collectCount();
        const size_t bytesBefore = Gc::get().bytesAllocated();
        Gc::get().collectIfNeeded();
        if (Gc::get().collectCount() > collectionsBefore &&
            std::getenv("XYO_RT_GC_SOFT_BUDGET_MB"))
            std::fprintf(stderr,
                "[native-gc-deferred-drained] collected=1 "
                "active_fibers=%zu bytes_before=%zu bytes_after=%zu\n",
                FiberPool::activeCount(), bytesBefore,
                Gc::get().bytesAllocated());
    }
    return doneThreads;
}

void Sequencer::stepThread(Thread& thread) {
    thread.pendingShortBroadcast = false;
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
        // IR-001's historical yields() counts NONROOT procedure mirror
        // yields. A pure LLVM root script never increments it. Count
        // genuine source-verified root loop suspensions separately per
        // Scratch Thread so --after=N can select the 2nd, 17th, or Nth
        // root latch rather than silently never attempting a handoff.
        // Only the opt-in producer is eligible; do not count unknown
        // phases, finished Fibers, or stale/disabled source sessions.
        static const bool rootOnlyProducerEnabled = [] {
            const char* v = std::getenv("XYO_RT_ROOT_ONLY_DEOPT");
            return v && v[0] == '1';
        }();
        const bool certifiedRootLatch = [&] {
            if (!rootOnlyProducerEnabled || !nativeSimpleDeopt ||
                !thread.fiber || thread.status != STATUS_RUNNING ||
                !thread.nativeProcedureFrames ||
                !thread.nativeProcedureFrames->enabled() ||
                !thread.nativeProcedureFrames->sourceUnchanged() ||
                thread.nativeProcedureFrames->depth() != 1 ||
                thread.nativeProcedureFrames->yields() != 0)
                return false;
            const auto state =
                thread.nativeProcedureFrames->sourceCheckpoint();
            return state &&
                state->phase == ir::SourceResumePhase::AfterLoopBody &&
                !state->loops.empty() &&
                state->frame.resumePc == state->loops.back().sourcePc;
        }();
        if (certifiedRootLatch &&
            thread.nativeRootLoopYieldCount != UINT64_MAX)
            ++thread.nativeRootLoopYieldCount;
        const bool rootOnlyYieldDue = certifiedRootLatch &&
            thread.nativeRootLoopYieldCount >= nativeDeoptAfter;
        // An actual native Fiber that observed the opt-in soft memory
        // budget at a source-certified loop yield may request the
        // *existing* independently verified interpreter handoff without
        // enabling the test-only forced Nth-yield deopt mode.
        // A failed verification must leave the Fiber runnable and
        // MUST NOT run GC over its unrooted SSA registers.
        const bool budgetRequest =
            thread.nativeGcSoftBudgetYieldDue &&
            !thread.nativeGcSoftBudgetAttempted;
        if (budgetRequest)
            thread.nativeGcSoftBudgetAttempted = true;
        const bool attemptDeopt =
            (nativeSimpleDeopt && thread.nativeProcedureFrames &&
             correctNativePhase &&
             (thread.nativeProcedureFrames->yields() >= nativeDeoptAfter ||
              rootOnlyYieldDue)) ||
            (budgetRequest && thread.nativeProcedureFrames &&
             correctNativePhase);
        // Test-only proof that each thread reaches the specified native
        // checkpoint exactly, rather than inheriting another hat's count.
        static const bool traceRootYield = [] {
            const char* v = std::getenv("XYO_RT_TEST_ROOT_YIELD_COUNT");
            return v && v[0] == '1';
        }();
        if (traceRootYield && certifiedRootLatch &&
            thread.nativeRootLoopYieldCount == nativeDeoptAfter) {
            std::fprintf(stderr,
                "[native-root-yield] script=%s count=%llu target=%llu\n",
                thread.topBlock.c_str(),
                static_cast<unsigned long long>(
                    thread.nativeRootLoopYieldCount),
                static_cast<unsigned long long>(nativeDeoptAfter));
        }
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
        // Memory-pressure deoptimization is a change of execution
        // engine, NOT a Scratch yield. A warp script must retain its
        // current scheduler turn until the ordinary warp time limit.
        // Otherwise a second green-flag hat observes partial side
        // effects that it could not observe in Interpreter-only mode.
        //
        // The native JitCtx is the authoritative *currently executing*
        // warp state even when its detached source/SSA certificate is
        // invalid and the Interpreter handoff must fail closed.
        // Capture it BEFORE a successful handoff releases JitCtx.
        const bool resumeNativeWarpSameTurn =
            budgetRequest && thread.jitCtx && thread.jitCtx->isWarp;
        bool resumeWarpSameTurn = false;
        if ((sourceHandoffProbe || attemptDeopt) &&
            thread.nativeProcedureFrames && thread.status == STATUS_RUNNING &&
            thread.fiber) {
            auto handoff = captureNativeSourceHandoff(thread);
            if (attemptDeopt && !handoff) {
                noteJitDeopt(JitDeoptPath::NativeHandoff,
                             JitDeoptReason::HandoffRefused, false);
                if (budgetRequest)
                    std::fprintf(stderr,
                        "[native-gc-soft-budget] script=%s requested=1 "
                        "adopted=0 collected=0 reason=no-handoff\n",
                        thread.topBlock.c_str());
            }
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
                noteJitDeopt(JitDeoptPath::NativeHandoff,
                             adopted ? JitDeoptReason::ExplicitHandoff
                                     : JitDeoptReason::HandoffRefused,
                             adopted);
                std::fprintf(stderr, "[native-simple-deopt] script=%s adopted=%d\n",
                    thread.topBlock.c_str(), int(adopted));
                // Never collect on a refused handoff. The native Fiber
                // is still live and its unrooted SSA must be preserved.
                if (budgetRequest && !adopted)
                    std::fprintf(stderr,
                        "[native-gc-soft-budget] script=%s requested=1 "
                        "adopted=0 collected=0 reason=source-refused\n",
                        thread.topBlock.c_str());
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
                if (adopted && budgetRequest) {
                    // The restored Interpreter has become the only
                    // owner of all committed Scratch effects. We can
                    // now safely collect and reclaim memory pressure
                    // *after* the native Fiber is discarded. If the
                    // existing post-adopt-GC test already collected,
                    // don't do an additional redundant sweep.
                    handoff.reset();
                    const size_t before = Gc::get().bytesAllocated();
                    const uint64_t collectionsBefore = Gc::get().collectCount();
                    if (!testGcAfterAdopt)
                        Gc::get().collect();
                    // A different suspended native Fiber can veto this
                    // sweep. Do not misreport a deferred collection as
                    // completed; the global GC barrier drains it only
                    // after the final live native Fiber has retired.
                    const bool collected = testGcAfterAdopt
                        ? !Gc::get().explicitCollectionPending()
                        : Gc::get().collectCount() > collectionsBefore;
                    const auto warpFrames = std::count_if(
                        thread.stackFrames.begin(),
                        thread.stackFrames.end(),
                        [](const auto& frame) {
                            return frame && frame->warpMode;
                        });
                    std::fprintf(stderr,
                        "[native-gc-soft-budget] script=%s requested=1 "
                        "adopted=1 collected=%d bytes_before=%zu "
                        "bytes_after=%zu warp_frames=%zu pending=%d "
                        "active_fibers=%zu\n",
                        thread.topBlock.c_str(), int(collected), before,
                        Gc::get().bytesAllocated(), size_t(warpFrames),
                        int(Gc::get().explicitCollectionPending()),
                        FiberPool::activeCount());
                }
                if (adopted && !testGcAfterAdopt &&
                    Gc::get().explicitCollectionPending()) {
                    // The native Fiber and detached non-executable
                    // SSA roots have now been retired. The restored
                    // interpreter variables/control frames are the
                    // authoritative GC roots. Fulfil a previously
                    // deferred native explicit-GC request only NOW,
                    // never at an intermediate unrooted native yield.
                    handoff.reset();
                    const auto before = Gc::get().collectCount();
                    Gc::get().collectIfNeeded();
                    std::fprintf(stderr,
                        "[native-ssa-explicit-gc-drained] script=%s "
                        "before=%llu after=%llu pending=%d\n",
                        thread.topBlock.c_str(),
                        static_cast<unsigned long long>(before),
                        static_cast<unsigned long long>(
                            Gc::get().collectCount()),
                        int(Gc::get().explicitCollectionPending()));
                }
                if (adopted && budgetRequest && thread.status == STATUS_RUNNING &&
                    !thread.stackFrames.empty() &&
                    thread.peekStackFrame()->warpMode)
                    resumeWarpSameTurn = true;
            }
        }
        if (resumeNativeWarpSameTurn && thread.isCompiled &&
            thread.fiber && thread.jitCtx &&
            thread.status == STATUS_RUNNING &&
            runtime.isActiveThread(&thread) && !thread.isKilled) {
            // Source/SSA refusal does not revoke the native Fiber, but
            // the GC request itself must not manufacture a Scratch
            // scheduling boundary. Continue the original machine code
            // until its ordinary warp watchdog (or a real yield) fires.
            // nativeGcSoftBudgetAttempted prevents a second budget
            // interruption, so one resume suffices and cannot loop on
            // the same rejected certificate.
            jitStepThread(thread);
        }
        if (resumeWarpSameTurn && runtime.isActiveThread(&thread) &&
            !thread.isKilled && !thread.stack.empty()) {
            // Preserve the time spent running the native half of this
            // scheduler slice. Starting a fresh 500-ms warp timer here
            // would incorrectly extend the atomic window after transfer.
            if (!thread.hasWarpTimer) {
                thread.hasWarpTimer = true;
                if (timer.usesSteadyClock() &&
                    thread.warpTimer.usesSteadyClock())
                    thread.warpTimer.setStartTime(timer.startTime());
                else
                    thread.warpTimer.start();
            }
            stepThreadBound(thread, 0);
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
    // IR-owned, deep-copied typed SSA diagnostics. Raw values are
    // deliberately never treated as GC roots or interpreter inputs.
    std::optional<ir::SourceTypedSsaSnapshot> typedSsaCopy;
    // Partial proof: only *live native function arguments* whose exact
    // NaN-boxed bits match independently rooted Scratch CallFrame args.
    // Ordinary LLVM instruction words and live pointers remain unsafe.
    std::optional<rt::RootedSsaArgumentAudit> rootedSsaAudit;
    // Unlike original CallFrame arguments, these are fresh native
    // strings retained at the source checkpoint via exact GC handles.
    std::optional<rt::GeneratedSsaRootAudit> generatedSsaAudit;

    // RT-001 optional detached integer-SSA diagnostic. Unlike the
    // lexical CallFrame counters this is native LLVM's exact raw SSA
    // instruction result vector, copied into Thread before Fiber yield.
    // It is NOT an executable deopt environment: unsupported pointers,
    // non-integer values, GC liveness and effects are still unresolved.
    // The already source-certified interpreter plan remains mandatory.
    static const bool requireTypedManifest = [] {
        const char* v = std::getenv("XYO_RT_REQUIRE_TYPED_SSA_MANIFEST");
        return v && v[0] == '1';
    }();
    static const bool checkDetachedSsa = [] {
        const char* v = std::getenv("XYO_RT_CHECK_LLVM_SSA_CAPTURE");
        return v && v[0] == '1';
    }();
    static const bool requireRootedSsaArgs = [] {
        const char* v = std::getenv("XYO_RT_REQUIRE_ROOTED_SSA_ARGS");
        return v && v[0] == '1';
    }();
    static const bool requireGeneratedSsaRoots = [] {
        const char* v = std::getenv("XYO_RT_REQUIRE_GENERATED_SSA_ROOTS");
        return v && v[0] == '1';
    }();
    // Strict gate ONLY on scheduler Fiber -> interpreter handoff.
    // A structurally matching reporter table by itself never authorizes
    // recovery when source reporter destinations or native SSA are missing.
    static const bool requireReporterSsaAudit = [] {
        const char* v = std::getenv(
            "XYO_RT_REQUIRE_REPORTER_SSA_AUDIT");
        return v && v[0] == '1';
    }();
    if (checkDetachedSsa || requireTypedManifest || requireRootedSsaArgs ||
        requireGeneratedSsaRoots || requireReporterSsaAudit) {
        static const std::string corrupt = [] {
            const char* v = std::getenv("XYO_RT_TEST_CORRUPT_LLVM_SSA");
            return v ? std::string(v) : std::string{};
        }();
        const bool forceComplete = [] {
            const char* v = std::getenv("XYO_RT_REQUIRE_COMPLETE_LLVM_SSA");
            return v && v[0] == '1';
        }();
        NativeRtSsaCapture* ssa = thread.nativeRtSsaCapture.get();
        if (!ssa || ssa->words.empty() ||
            ssa->words.size() != ssa->descriptors.size())
            return false;
        // Test corruption only touches detached host-owned metadata,
        // never the native machine registers or activation stack.
        if (corrupt == "value")
            ssa->words.front() ^= uint64_t{1};
        else if (corrupt == "source")
            ++ssa->sourcePc;
        else if (corrupt == "kind")
            ssa->descriptors.front() ^= uint64_t{0x400} << 32;
        else if (corrupt == "unresolved")
            ++ssa->unresolvedPointers;
        else if (corrupt == "origin")
            ++ssa->pointerOrigins[0];
        else if (corrupt == "stack-count")
            ++ssa->stackDerivedPointers;
        else if (corrupt == "stack-roots")
            ++ssa->distinctStackAllocas;
        else if (corrupt == "site-rehash") {
            ++ssa->siteId;
            ssa->fingerprint = ssa->computeFingerprint();
        } else if (corrupt == "kind-rehash") {
            // Preserve a syntactically valid descriptor width, but
            // change the instruction/argument kind. FNV recomputation
            // bypasses ONLY the local copy-integrity check; the
            // separately retained compiler manifest must still veto.
            if (!ssa->descriptors.empty()) {
                const uint64_t descriptor = ssa->descriptors.front();
                ssa->descriptors.front() =
                    (descriptor & UINT32_MAX) |
                    ((uint64_t(uint32_t(descriptor >> 32) ^ 0x200))
                     << 32);
                ssa->fingerprint = ssa->computeFingerprint();
            }
        } else if (corrupt == "rooted-arg-rehash") {
            // Leave the typed manifest and detached FNV both valid.
            // The independent, GC-rooted source owner must catch an
            // observed live i64 parameter that no longer matches.
            for (size_t i = 0; i < ssa->descriptors.size(); ++i) {
                const uint32_t family =
                    uint32_t(ssa->descriptors[i] >> 32) &
                    ~uint32_t{255};
                if (family == 0x200) {
                    ssa->words[i] ^= uint64_t{1};
                    ssa->fingerprint = ssa->computeFingerprint();
                    break;
                }
            }
        } else if (corrupt == "alias-map-rehash") {
            // Forge the mutable runtime SSA->source argument owner and
            // RECOMPUTE its FNV. The independent compiler mapping must
            // still reject before native Fiber retirement.
            if (!ssa->aliasArguments.empty()) {
                auto it = std::find_if(
                    ssa->aliasArguments.begin(),
                    ssa->aliasArguments.end(),
                    [](uint32_t a) { return a > 0; });
                if (it != ssa->aliasArguments.end()) {
                    *it = 0;
                    ssa->fingerprint = ssa->computeFingerprint();
                }
            }
        } else if (corrupt == "alias-both-drop") {
            // Even a perfectly rehashed old-style snapshot that
            // dropped BOTH provenance arrays must fail closed in the
            // new strict owner-checking mode.
            ssa->aliasArguments.clear();
            ssa->compilerAliasArguments.clear();
            ssa->fingerprint = ssa->computeFingerprint();
        } else if (corrupt == "alias-word-rehash") {
            // Alters an instruction PHI/SELECT value that claims exact
            // rooted source-argument ancestry, without altering the
            // original compiler alias map (when such a value exists).
            for (size_t i = 0; i < ssa->aliasArguments.size(); ++i) {
                const uint32_t family =
                    uint32_t(ssa->descriptors[i] >> 32) &
                    ~uint32_t{255};
                if (family == 0 && ssa->aliasArguments[i]) {
                    ssa->words[i] ^= uint64_t{1};
                    ssa->fingerprint = ssa->computeFingerprint();
                    break;
                }
            }
        } else if (corrupt == "generated-word-rehash") {
            // Corrupt only a certified native-created string SSA word,
            // keeping compiler descriptors and local FNV valid.
            for (size_t i = 0; i < ssa->generatedStrings.size(); ++i) {
                if (ssa->generatedStrings[i]) {
                    ssa->words[i] ^= uint64_t{1};
                    ssa->fingerprint = ssa->computeFingerprint();
                    break;
                }
            }
        } else if (corrupt == "generated-mask-rehash") {
            for (auto& source : ssa->generatedStrings) {
                if (source) {
                    source = 0;
                    ssa->fingerprint = ssa->computeFingerprint();
                    break;
                }
            }
        } else if (corrupt == "generated-both-drop") {
            ssa->generatedStrings.clear();
            ssa->compilerGeneratedStrings.clear();
            ssa->fingerprint = ssa->computeFingerprint();
        } else if (corrupt == "generated-root-drop") {
            for (size_t i = 0; i < ssa->generatedStringHandles.size(); ++i) {
                auto& handle = ssa->generatedStringHandles[i];
                if (handle) {
                    if (ssa->generatedStringRoots)
                        ssa->generatedStringRoots->release(handle);
                    handle = {};
                    break;
                }
            }
        } else if (corrupt == "manifest-drop") {
            ssa->compilerManifest.reset();
        } else if (corrupt == "manifest-site") {
            if (ssa->compilerManifest)
                ++ssa->compilerManifest->siteId;
        } else if (corrupt == "manifest-kind") {
            if (ssa->compilerManifest &&
                !ssa->compilerManifest->descriptors.empty())
                ssa->compilerManifest->descriptors.front() ^=
                    (uint64_t{0x200} << 32);
        } else if (corrupt == "reporter-input-root-drop") {
            // Preserve the compiled source and numeric SSA fingerprints;
            // lose only the separately registered historical list/reporter
            // heap-string root handle. The scheduler must fail closed.
            for (auto& handle : ssa->reporterInputHandles) {
                if (!handle) continue;
                if (ssa->reporterInputRoots)
                    ssa->reporterInputRoots->release(handle);
                handle = {};
                break;
            }
        } else if (corrupt == "reporter-owner-epoch") {
            // Test-only: mutate detached reporter OWNER epoch while
            // keeping LLVM words, compiler manifest and local FNV
            // unchanged. The independently reacquired Target stamp
            // must still refuse a native Fiber handoff.
            ++ssa->reporterOwnerLocalGeneration;
        } else if (corrupt == "origin-swap") {
            // This preserves the total pointer count, so a sum-only
            // check cannot detect it; the frozen fingerprint must.
            auto from = std::find_if(
                ssa->pointerOrigins.begin(), ssa->pointerOrigins.end(),
                [](uint32_t count) { return count > 0; });
            if (from != ssa->pointerOrigins.end()) {
                const size_t i = size_t(from -
                    ssa->pointerOrigins.begin());
                const size_t j = (i + 1) % ssa->pointerOrigins.size();
                --ssa->pointerOrigins[i];
                ++ssa->pointerOrigins[j];
            }
        }
        uint64_t pointerSum = 0;
        for (uint32_t count : ssa->pointerOrigins)
            pointerSum += count;
        const auto& source = *handoff.image->symbols;
        if (ssa->sourceRevision != source.sourceRevision ||
            ssa->digestLo != source.sourceDigest.lo ||
            ssa->digestHi != source.sourceDigest.hi ||
            ssa->sourcePc != leaf.frame.resumePc ||
            ssa->procedureId != leaf.frame.procedureId ||
            pointerSum != ssa->unresolvedPointers ||
            ssa->distinctStackAllocas > ssa->stackDerivedPointers ||
            ssa->stackDerivedPointers > ssa->unresolvedPointers ||
            uint64_t(ssa->unresolvedPointers) +
                uint64_t(ssa->unresolvedOther) !=
                uint64_t(ssa->unsupported) ||
            ssa->fingerprint != ssa->computeFingerprint() ||
            (forceComplete &&
             (!ssa->numericComplete || ssa->unsupported != 0)))
            return false;

        if (requireTypedManifest || requireRootedSsaArgs ||
            requireGeneratedSsaRoots || requireReporterSsaAudit) {
            const auto* expected = ssa->compilerManifest
                ? &*ssa->compilerManifest : nullptr;
            if (!expected ||
                expected->revision != source.sourceRevision ||
                expected->digestLo != source.sourceDigest.lo ||
                expected->digestHi != source.sourceDigest.hi ||
                expected->procedureId != leaf.frame.procedureId ||
                expected->resumePc != leaf.frame.resumePc)
                return false;
            // This copies raw values into an IR-owned, independently
            // verified diagnostic image. It never interprets those
            // bits as SStr*, Scratch Value, or executable continuation.
            typedSsaCopy = ir::verifyDetachedTypedSsa(*ssa, *expected);
            if (!typedSsaCopy || !typedSsaCopy->matches(*expected))
                return false;
        }
        if (requireRootedSsaArgs || requireGeneratedSsaRoots) {
            if (!handoff.image->frames) return false;
            rootedSsaAudit = rt::verifyRootedSsaArguments(
                *ssa, *handoff.image->frames,
                uint32_t(decoded->size()), leaf.frame.procedureId,
                leaf.frame.arguments, true);
            if (!rootedSsaAudit ||
                !rootedSsaAudit->matchedAllObservedArguments)
                return false;
        }
        if (requireGeneratedSsaRoots) {
            generatedSsaAudit = rt::verifyGeneratedSsaRoots(*ssa);
            if (!generatedSsaAudit) return false;
        }
#if defined(XYO_JIT)
        if (requireReporterSsaAudit) {
            const auto* compiled = static_cast<const CompiledScript*>(
                thread.compiled);
            const auto* entry = compiled && compiled->nativeReporterSites
                ? compiled->nativeReporterSites->find(ssa->siteId) : nullptr;
            if (!entry || !ssa->compilerManifest ||
                !handoff.image->frames)
                return false;
            // Revalidate at the actual scheduler safe point, not merely
            // at the earlier native capture, to reject stale frame roots
            // and tampered SSA data before retiring the live Fiber.
            const auto report = ir::auditSourceReporterSsaLocations(
                entry->gap, entry->frozen, *ssa->compilerManifest,
                *ssa, *handoff.image->frames);
            if (!report) return false;
            const auto owners = thread.target
                ? ir::auditCompiledReporterStorageOwners(
                    entry->frozen.reporters, *thread.target)
                : std::nullopt;
            const bool epochStable = owners && ssa->reporterOwnerReadback &&
                ssa->reporterOwnerTarget == thread.target &&
                ssa->reporterOwnerStage == owners->epoch.stage &&
                ssa->reporterOwnerLocalGeneration ==
                    owners->epoch.localGeneration &&
                ssa->reporterOwnerStageGeneration ==
                    owners->epoch.stageGeneration;
            std::fprintf(stderr,
                "[ir001-scheduler-reporter-owners] site=%llu "
                "matched=%d epoch_stable=%d lists=%u scalars=%u "
                "missing=%u wrong_type=%u unresolved=%u "
                "value_restorable=0 executable=0\n",
                static_cast<unsigned long long>(ssa->siteId),
                int(bool(owners)), int(epochStable),
                owners ? owners->typedListOwners : 0,
                owners ? owners->typedScalarOwners : 0,
                owners ? owners->missingOwners : 0,
                owners ? owners->wrongTypeOwners : 0,
                owners ? owners->unresolvedOrigins : 0);
            if (!epochStable || !owners->ownershipComplete())
                return false;
            // Recheck AFTER native yield and BEFORE Fiber retirement.
            // A stale dropped handle never becomes an executable value.
            if (!ir::verifySourceReporterInputStrings(
                    *ssa, entry->frozen, *ssa->compilerManifest))
                return false;
            // Only original (read-time) event records are acceptable.
            // No fresh Scratch INDEX evaluation or current list dereference.
            if (ssa->reporterListReadEvidence &&
                !ir::verifyListReadEvidence(
                    entry->frozen, *ssa->compilerManifest, *ssa,
                    *ssa->reporterListReadEvidence))
                return false;
            const auto capturedInputs = ir::auditReporterInputSlots(
                entry->frozen, *report, *ssa->compilerManifest, *ssa);
            if (!capturedInputs || !thread.blockContainer) return false;
            const auto destinations =
                ir::auditReporterInterpreterDestinations(
                    entry->frozen, *capturedInputs,
                    *thread.blockContainer);
            if (!destinations) return false;
            std::fprintf(stderr,
                "[ir001-scheduler-reported-entry-map] site=%llu "
                "mapped=%u input_count=%zu "
                "incomplete_prefix=%u contiguous_prefix=%u "
                "source_complete=0 executable=0\n",
                static_cast<unsigned long long>(ssa->siteId),
                destinations->mapped, capturedInputs->inputs.size(),
                destinations->incompletePrefix,
                destinations->contiguousReportedPrefix);
            if (destinations->mapped != capturedInputs->inputs.size())
                return false;
            const auto& historical = ssa->reporterScalarPrefixEvidence;
            if (!destinations->missingPrefixOps.empty()) {
                if (!historical || !handoff.image->symbols ||
                    !ir::verifyHistoricalScalarPrefix(
                        *historical, *destinations,
                        ssa->reporterListReadEvidence.get(),
                        *thread.blockContainer,
                        entry->frozen.revision,
                        [&](uint64_t pc) -> std::string_view {
                            const auto id = handoff.image->symbols->block(pc);
                            return id ? *id : std::string_view{};
                        }))
                    return false;
            }
            if (historical && handoff.image->symbols &&
                !ir::verifyLiveScalarReadWords(
                    *historical, entry->frozen,
                    *ssa->compilerManifest, *ssa,
                    *thread.blockContainer,
                    [&](uint64_t pc) -> std::string_view {
                        const auto id = handoff.image->symbols->block(pc);
                        return id ? *id : std::string_view{};
                    }))
                return false;
            const auto& restoredPrefix = ssa->reporterPrefixPlan;
            if (restoredPrefix &&
                !ir::verifyEvaluatedReportedPrefix(
                    *restoredPrefix, *destinations,
                    *thread.blockContainer))
                return false;
            std::fprintf(stderr,
                "[ir001-scheduler-reported-prefix-plan] site=%llu "
                "captured=%u valid=%d executable=0\n",
                static_cast<unsigned long long>(ssa->siteId),
                restoredPrefix ? restoredPrefix->capturedOperations : 0,
                int(bool(restoredPrefix)));
            std::fprintf(stderr,
                "[ir001-scheduler-scalar-prefix] site=%llu "
                "matched=%u expected=%zu "
                "source_complete=0 executable=0\n",
                static_cast<unsigned long long>(ssa->siteId),
                historical ? historical->matched : 0,
                destinations->missingPrefixOps.size());
            std::fprintf(stderr,
                "[ir001-scheduler-reporter-inputs] site=%llu "
                "inputs=%zu immediate=%u rooted_heap=%u "
                "unrooted_heap=%u ambiguous=%u "
                "restorable=0 executable=0\n",
                static_cast<unsigned long long>(ssa->siteId),
                capturedInputs->inputs.size(),
                capturedInputs->immediateCount,
                capturedInputs->rootedHeapObservations,
                capturedInputs->unrootedHeapObservations,
                capturedInputs->ambiguousInputs);
            // No interpreter ReportedEntry / parent ArgValues writeback
            // has been proven for this source location. Do not treat an
            // observed SSA word as an executable continuation, EVEN if
            // it is a pointer-free numeric Scratch immediate.
            std::fprintf(stderr,
                "[ir001-scheduler-reporter-audit] site=%llu "
                "observed=%zu unmapped=%u unsupported=%u "
                "source_complete=0 executable=0\n",
                static_cast<unsigned long long>(report->physicalSiteId),
                report->reporters.size(), report->unresolvedReporters,
                report->unsupportedLlvmValues);
            // This opt-in strict mode does not infer live native pointers
            // or Scratch temporary destinations. A rejected handoff
            // preserves the native Fiber for its normal continuation.
            if (report->unresolvedReporters ||
                report->unsupportedLlvmValues ||
                !capturedInputs->inputs.empty())
                return false;
        }
#else
        // No LLVM-produced SSA exists in a JIT-disabled runtime.
        if (requireReporterSsaAudit) return false;
#endif
        static const bool trace = [] {
            const char* v = std::getenv("XYO_RT_TRACE_LLVM_SSA_CAPTURE");
            return v && v[0] == '1';
        }();
        if (trace && generatedSsaAudit)
            std::fprintf(stderr,
                "[native-ssa-generated-roots] script=%s "
                "certified=%u rooted=%u immediate=%u checked=1\n",
                thread.topBlock.c_str(),
                generatedSsaAudit->provenStringSlots,
                generatedSsaAudit->rootedHeapStrings,
                generatedSsaAudit->immediateStrings);
        if (trace && rootedSsaAudit)
            std::fprintf(stderr,
                "[native-ssa-rooted-args] script=%s matched=%u "
                "heap_strings=%u nonargument_words=%u checked=1 "
                "aliased=%u aliased_strings=%u\n",
                thread.topBlock.c_str(),
                rootedSsaAudit->matchedArguments,
                rootedSsaAudit->rootedStringArguments,
                rootedSsaAudit->unverifiedInstructionWords,
                rootedSsaAudit->matchedAliasedInstructions,
                rootedSsaAudit->rootedAliasedStrings);
        if (trace && typedSsaCopy) {
            std::fprintf(stderr,
                "[native-typed-ssa-manifest] script=%s site=%llu "
                "pc=%llu words=%zu independent=1\n",
                thread.topBlock.c_str(),
                static_cast<unsigned long long>(
                    typedSsaCopy->manifest.siteId),
                static_cast<unsigned long long>(
                    typedSsaCopy->manifest.resumePc),
                typedSsaCopy->words.size());
        }
        if (trace) {
            size_t floats = 0, numericArgs = 0, floatArgs = 0;
            for (uint64_t desc : ssa->descriptors) {
                switch (uint32_t(desc >> 32) & ~uint32_t{255}) {
                    case 0x100: ++floats; break;
                    case 0x200: ++numericArgs; break;
                    case 0x300: ++floatArgs; break;
                    default: break;
                }
            }
            std::fprintf(stderr,
                "[native-llvm-ssa-detached] script=%s site=%llu "
                "pc=%llu words=%zu unsupported=%u complete=%d checked=1 "
                "floats=%zu intargs=%zu floatargs=%zu pointers=%u other=%u "
                "ptr_origins=%u,%u,%u,%u,%u,%u,%u "
                "stack_derived=%u stack_roots=%u\n",
                thread.topBlock.c_str(),
                static_cast<unsigned long long>(ssa->siteId),
                static_cast<unsigned long long>(ssa->sourcePc),
                ssa->words.size(), ssa->unsupported,
                int(ssa->numericComplete), floats, numericArgs, floatArgs,
                ssa->unresolvedPointers, ssa->unresolvedOther,
                ssa->pointerOrigins[0], ssa->pointerOrigins[1],
                ssa->pointerOrigins[2], ssa->pointerOrigins[3],
                ssa->pointerOrigins[4], ssa->pointerOrigins[5],
                ssa->pointerOrigins[6],
                ssa->stackDerivedPointers, ssa->distinctStackAllocas);
        }
    }
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

        // IR-001's opt-in numeric LLVM checkpoint sidecar contains
        // lexical *loop counter slots*, not all compiler-live SSA.
        // Cross-check this separate typed export against the source-
        // authenticated native CallFrame before any Fiber is retired.
        // It is extra consistency evidence only; effect continuity and
        // the exact Scratch latch are still proved by the RT plan.
        static const bool requireLoopSsa = [] {
            const char* v =
                std::getenv("XYO_RT_REQUIRE_LOOP_SSA_DIAGNOSTIC");
            return v && v[0] == '1';
        }();
        if (requireLoopSsa) {
            // Test-only fault injection mutates only the detached sidecar,
            // never the still-running LLVM Fiber or original CallFrame.
            static const std::string corruptMode = [] {
                const char* v =
                    std::getenv("XYO_RT_TEST_CORRUPT_LOOP_SSA");
                return v ? std::string(v) : std::string{};
            }();
            auto& sidecar = handoff.image->diagnosticSsa;
            if (corruptMode == "drop") {
                sidecar.reset();
            } else if (sidecar && !sidecar->slots.empty()) {
                if (corruptMode == "value")
                    sidecar->values.front() ^= uint64_t{1};
                else if (corruptMode == "slot")
                    ++sidecar->slots.front().id;
            }
            const auto& symbols = *handoff.image->symbols;
            ir::SourceSsaManifest manifest{
                symbols.sourceRevision, symbols.sourceDigest.lo,
                symbols.sourceDigest.hi, leaf.frame.procedureId,
                leaf.frame.resumePc, {}};
            std::vector<uint64_t> expectedWords;
            // Stable slot IDs belong to the source compiler's original
            // lexical loop table, not an untrusted snapshot manifest.
            // Absent and noncounted loops have no numeric SSA slot.
            for (uint32_t slot = 0; slot < symbols.loops.size(); ++slot) {
                const auto& sourceLoop = symbols.loops[slot];
                const auto saved = std::find_if(
                    leaf.loops.begin(), leaf.loops.end(),
                    [&](const ir::SourceLoopResume& loop) {
                        return loop.sourcePc == sourceLoop.sourcePc;
                    });
                if (saved == leaf.loops.end() ||
                    saved->counterBits ==
                        ir::CallFrameAbi::kNoLoopCounter)
                    continue;
                if (!sourceLoop.counted || sourceLoop.localSlot != slot)
                    return false;
                manifest.slots.push_back({slot, 64});
                expectedWords.push_back(saved->counterBits);
            }
            if (!handoff.image->matchesSsaManifest(manifest) ||
                !sidecar || sidecar->values != expectedWords)
                return false;
        }
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
        // pushStack inherits an enclosing warp flag; additionally
        // restore the original procedure's own verified warp setting.
        // This is required for correct loop latch and tick scheduling.
        call->warpMode = call->warpMode || restored.declaredWarp;
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
    // The numeric SSA words live in RT-owned heap storage, never in
    // the discarded native activation. Verify them *after* FiberPool
    // release in diagnostic mode, then destroy this inert copy.
    static const bool traceDetachedAfterRelease = [] {
        const char* v = std::getenv("XYO_RT_TRACE_LLVM_SSA_CAPTURE");
        return v && v[0] == '1';
    }();
    if (traceDetachedAfterRelease && thread.nativeRtSsaCapture)
        std::fprintf(stderr,
            "[native-llvm-ssa-post-release] script=%s words=%zu intact=%d\n",
            thread.topBlock.c_str(),
            thread.nativeRtSsaCapture->words.size(),
            int(thread.nativeRtSsaCapture->fingerprint ==
                thread.nativeRtSsaCapture->computeFingerprint()));
    if (generatedSsaAudit) {
        // Test-only full GC AFTER the native Fiber has been destroyed
        // but BEFORE the detached generated-string root owner expires.
        // The independent GC handles must still resolve exactly.
        static const bool gcDuringTransfer = [] {
            const char* v = std::getenv(
                "XYO_RT_TEST_GC_DURING_SSA_TRANSFER");
            return v && v[0] == '1';
        }();
        if (gcDuringTransfer) {
            Gc::get().collect();
            std::fprintf(stderr,
                "[native-ssa-generated-gc] script=%s collected=1\n",
                thread.topBlock.c_str());
        }
        const auto checkedAgain = thread.nativeRtSsaCapture
            ? rt::verifyGeneratedSsaRoots(*thread.nativeRtSsaCapture)
            : std::nullopt;
        const bool intact = checkedAgain &&
            checkedAgain->provenStringSlots ==
                generatedSsaAudit->provenStringSlots &&
            checkedAgain->rootedHeapStrings ==
                generatedSsaAudit->rootedHeapStrings &&
            checkedAgain->immediateStrings ==
                generatedSsaAudit->immediateStrings;
        if (traceDetachedAfterRelease)
            std::fprintf(stderr,
                "[native-ssa-generated-post-release] script=%s "
                "certified=%u rooted=%u immediate=%u intact=%d\n",
                thread.topBlock.c_str(),
                generatedSsaAudit->provenStringSlots,
                generatedSsaAudit->rootedHeapStrings,
                generatedSsaAudit->immediateStrings, int(intact));
    }
    if (rootedSsaAudit) {
        const auto checkedAgain =
            thread.nativeRtSsaCapture && handoff.image->frames
                ? rt::verifyRootedSsaArguments(
                    *thread.nativeRtSsaCapture,
                    *handoff.image->frames,
                    uint32_t(decoded->size()),
                    leaf.frame.procedureId, leaf.frame.arguments,
                    true)
                : std::nullopt;
        const bool intact = checkedAgain &&
            checkedAgain->matchedAllObservedArguments &&
            checkedAgain->matchedArguments ==
                rootedSsaAudit->matchedArguments &&
            checkedAgain->rootedStringArguments ==
                rootedSsaAudit->rootedStringArguments &&
            checkedAgain->matchedAliasedInstructions ==
                rootedSsaAudit->matchedAliasedInstructions &&
            checkedAgain->rootedAliasedStrings ==
                rootedSsaAudit->rootedAliasedStrings;
        if (traceDetachedAfterRelease)
            std::fprintf(stderr,
                "[native-ssa-rooted-args-post-release] script=%s "
                "matched=%u heap_strings=%u intact=%d "
                "aliased=%u aliased_strings=%u\n",
                thread.topBlock.c_str(),
                rootedSsaAudit->matchedArguments,
                rootedSsaAudit->rootedStringArguments, int(intact),
                rootedSsaAudit->matchedAliasedInstructions,
                rootedSsaAudit->rootedAliasedStrings);
    }
    if (typedSsaCopy) {
        const bool intact = thread.nativeRtSsaCapture &&
            thread.nativeRtSsaCapture->compilerManifest &&
            typedSsaCopy->matches(
                *thread.nativeRtSsaCapture->compilerManifest) &&
            typedSsaCopy->words == thread.nativeRtSsaCapture->words;
        if (traceDetachedAfterRelease)
            std::fprintf(stderr,
                "[native-typed-ssa-post-release] script=%s "
                "words=%zu intact=%d\n",
                thread.topBlock.c_str(),
                typedSsaCopy->words.size(), int(intact));
    }
    thread.nativeRtSsaCapture.reset();
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
    thread.pendingShortBroadcast = false;
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
    const JitDeoptReason rollbackReason =
        numericDeoptReason(ir::numericRollbackReasonName(
            session->continuation.rollbackReason()));
    const bool restored = ir::restoreNumericInterpreter(thread, *session);
    noteJitDeopt(JitDeoptPath::NumericCfg, rollbackReason, restored);
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
    thread.pendingShortBroadcast = false;
    const auto* compiled = static_cast<const CompiledScript*>(thread.compiled);
    // Defense in depth: even a directly attached CompiledScript must not
    // enter a native Fiber in fiberless mode. Only a fresh script may start
    // in the interpreter; never replay a mid-procedure/OSR continuation.
    if (jitFiberlessEnabled() && compiled && compiled->mayYield &&
        !compiled->wasm && !thread.jitCtx && !thread.fiber &&
        !thread.osrActive && !thread.nestedOsrActive &&
        !thread.interpreterStarted && thread.stack.size() == 1 &&
        thread.peekStack() == thread.topBlock) {
        thread.isCompiled = false;
        thread.compiled = nullptr;
        stepThreadBound(thread, 0);
        return;
    }
    // START-015: a native v3 artifact was compiled from a specific Blocks
    // revision. A source edit can invalidate both unresolved and resolved
    // BlockCached slots; do not enter or resume stale native machine code.
    // Source edits must be applied by the host at scheduler safe points,
    // after any worker batch joins. No concurrent unsynchronized edits.
    if (compiled && compiled->aotSourceRevision &&
        (!thread.blockContainer ||
         compiled->aotSourceBlocks != thread.blockContainer ||
         compiled->aotSourceRevision != thread.blockContainer->revision())) {
        const bool fresh = !thread.jitCtx && !thread.fiber &&
                           !thread.interpreterStarted &&
                           !thread.osrActive && !thread.nestedOsrActive;
        if (std::getenv("XYO_AOT_SOURCE_TRACE"))
            std::fprintf(stderr,
                "[aot-source-stale] script=%s fresh=%d action=%s\n",
                thread.topBlock.c_str(), int(fresh),
                fresh ? "interpreter" : "retire");
        if (fresh) {
            thread.isCompiled = false;
            thread.compiled = nullptr;
            // The root has not run yet, so interpretation starts once.
            stepThreadBound(thread, 0);
        } else {
            // There is no general source-certified continuation for an
            // arbitrary yielding native Fiber. Retire, never replay its
            // already committed side effects or resume stale code.
            retireThread(thread);
        }
        return;
    }
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
        // LAYQ: fresh quantum; stop snapshot so a pooled context reused
        // after a stop does not spuriously hand off on its first poll.
        // Pointer wiring follows the sampled-clock precedent (same block
        // below wires sampledWarpClockUs et al.).
        c->yieldQuantum = 64;
        c->observedStopEpoch =
            runtime.stopEpoch.load(std::memory_order_relaxed);
        c->quantumHintPtr = &runtime.yieldQuantumHint;
        c->stopEpochPtr = &runtime.stopEpoch;
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
            const bool innerPhase =
                c->nestedOsrDeoptPhase == 2 &&
                compiled->nestedOsrInnerYieldGuard;
            const bool triplePhase =
                c->nestedOsrDeoptPhase == 3 &&
                compiled->nestedOsrInnerYieldGuard &&
                !compiled->nestedOsrDeepInnerLoopId.empty();
            const bool quadPhase =
                c->nestedOsrDeoptPhase == 4 &&
                compiled->nestedOsrInnerYieldGuard &&
                !compiled->nestedOsrDeepInnerLoopId.empty() &&
                !compiled->nestedOsrFourthLoopId.empty();
            const bool outerPhase = c->nestedOsrDeoptPhase == 1;
            // The inner source ID is frozen in the CompiledScript, not
            // selected from a modified source graph. It must remain an
            // attached counted repeat in the same selected outer body.
            const bool innerTopology = [&] {
                if (!(innerPhase || triplePhase || quadPhase) ||
                    compiled->nestedOsrInnerLoopId.empty())
                    return false;
                const Block* outer =
                    blocks->getBlock(compiled->nestedOsrLoopId);
                const Block* inner =
                    blocks->getBlock(compiled->nestedOsrInnerLoopId);
                if (!outer || outer->opcode != "control_repeat" ||
                    !inner || inner->opcode != "control_repeat")
                    return false;
                const auto* sub = outer->inputs.find("SUBSTACK");
                if (!sub || sub->block.empty()) return false;
                std::unordered_set<std::string> visited;
                std::string id = sub->block;
                for (size_t budget = 16; !id.empty() && budget--; ) {
                    if (!visited.insert(id).second) return false;
                    if (id == compiled->nestedOsrInnerLoopId)
                        return true;
                    const Block* node = blocks->getBlock(id);
                    if (!node) return false;
                    id = node->next;
                }
                return false;
            }();
            const auto validRemaining = [](double n) {
                return std::isfinite(n) && n >= 0.0 &&
                       n <= 1099511627776.0 &&
                       std::floor(n) == n;
            };
            const bool deepestTopology = [&] {
                if (!(triplePhase || quadPhase) || !innerTopology)
                    return false;
                const Block* middle =
                    blocks->getBlock(compiled->nestedOsrInnerLoopId);
                const Block* leaf =
                    blocks->getBlock(compiled->nestedOsrDeepInnerLoopId);
                if (!middle || !leaf ||
                    middle->opcode != "control_repeat" ||
                    leaf->opcode != "control_repeat") return false;
                const auto* sub = middle->inputs.find("SUBSTACK");
                if (!sub || sub->block.empty()) return false;
                std::string id = sub->block;
                std::unordered_set<std::string> visited;
                for (size_t budget = 16; !id.empty() && budget--; ) {
                    if (!visited.insert(id).second) return false;
                    if (id == compiled->nestedOsrDeepInnerLoopId)
                        return true;
                    const Block* node = blocks->getBlock(id);
                    if (!node) return false;
                    id = node->next;
                }
                return false;
            }();
            const bool fourthTopology = [&] {
                if (!quadPhase || !deepestTopology) return false;
                const Block* deep =
                    blocks->getBlock(compiled->nestedOsrDeepInnerLoopId);
                const Block* leaf =
                    blocks->getBlock(compiled->nestedOsrFourthLoopId);
                if (!deep || !leaf ||
                    deep->opcode != "control_repeat" ||
                    leaf->opcode != "control_repeat") return false;
                const auto* sub = deep->inputs.find("SUBSTACK");
                if (!sub || sub->block.empty()) return false;
                std::unordered_set<std::string> visited;
                std::string id = sub->block;
                for (size_t budget = 16; !id.empty() && budget--; ) {
                    if (!visited.insert(id).second) return false;
                    if (id == compiled->nestedOsrFourthLoopId)
                        return true;
                    const Block* node = blocks->getBlock(id);
                    if (!node) return false;
                    id = node->next;
                }
                return false;
            }();
            const double innerRemaining =
                c->nestedOsrDeoptInnerRemaining;
            const double deepestRemaining =
                c->nestedOsrDeoptDeepRemaining;
            const double fourthRemaining =
                c->nestedOsrDeoptFourthRemaining;
            const bool safe = c->nestedOsrDeoptReady == 1 &&
                (outerPhase ||
                 (innerPhase && innerTopology &&
                  validRemaining(innerRemaining)) ||
                 (triplePhase && deepestTopology &&
                  validRemaining(innerRemaining) &&
                  validRemaining(deepestRemaining)) ||
                 (quadPhase && fourthTopology &&
                  validRemaining(innerRemaining) &&
                  validRemaining(deepestRemaining) &&
                  validRemaining(fourthRemaining))) &&
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
                noteJitDeopt((triplePhase || quadPhase) ?
                             JitDeoptPath::NestedTriple :
                             innerPhase ? JitDeoptPath::NestedInner :
                                          JitDeoptPath::NestedOuter,
                             JitDeoptReason::UnsafeResume, false);
                retireThread(thread);
                return;
            }
            ec->loopCounter = remaining;
            if (innerPhase || triplePhase || quadPhase) {
                // The original outer frame and GC argument roots remain
                // alive. Neither its body nor the middle body has ended
                // at a leaf-phase checkpoint. Push the exact frozen
                // source control frames, outermost first.
                const auto restoreCounted = [&](const std::string& id,
                                                double value) {
                    thread.pushStack(id);
                    StackFrame* frame = thread.peekStackFrame();
                    frame->isLoop = true;
                    frame->executionContext =
                        std::make_unique<ExecContext>();
                    frame->executionContext->hasLoopCounter = true;
                    frame->executionContext->loopCounter = value;
                };
                restoreCounted(compiled->nestedOsrInnerLoopId,
                               innerRemaining);
                if (triplePhase || quadPhase)
                    restoreCounted(compiled->nestedOsrDeepInnerLoopId,
                                   deepestRemaining);
                if (quadPhase)
                    restoreCounted(compiled->nestedOsrFourthLoopId,
                                   fourthRemaining);
            }
            thread.status = STATUS_RUNNING;
            noteJitDeopt((triplePhase || quadPhase) ?
                             JitDeoptPath::NestedTriple :
                         innerPhase ? JitDeoptPath::NestedInner :
                                      JitDeoptPath::NestedOuter,
                         JitDeoptReason::SourceChanged, true);
            if (std::getenv("XYO_JIT_OSR_NESTED_TRACE")) {
                if (quadPhase)
                    std::fprintf(stderr,
                        "[jit-osr-inflight] deopt quad outer=%.0f "
                        "middle=%.0f deep=%.0f leaf=%.0f "
                        "source-changed=1\\n",
                        remaining, innerRemaining,
                        deepestRemaining, fourthRemaining);
                else if (triplePhase)
                    std::fprintf(stderr,
                        "[jit-osr-inflight] deopt triple outer=%.0f "
                        "middle=%.0f leaf=%.0f source-changed=1\\n",
                        remaining, innerRemaining, deepestRemaining);
                else if (innerPhase)
                    std::fprintf(stderr,
                        "[jit-osr-inflight] deopt inner outer=%.0f "
                        "inner=%.0f source-changed=1\\n",
                        remaining, innerRemaining);
                else
                    std::fprintf(stderr,
                        "[jit-osr-inflight] deopt remaining=%.0f "
                        "source-changed=1; interpreter owns original loop\\n",
                        remaining);
            }
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
    // RT-001 (opt-in): before the next authenticated LLVM source
    // checkpoint, newly allocated SStrs may exist only in SSA registers.
    // Automatic heap-threshold GC inside xyo_join* could run before
    // any detached GC handle exists. Defer only that automatic GC during
    // the bounded native Fiber activation; it becomes eligible again
    // on return to the serial scheduler, after the checkpoint callback
    // has had a chance to register exact generated-string handles.
    //
    // Explicit Gc::collect() is now also postponed by the nested
    // DeferExplicitCollect guard, then drained only after the native
    // Fiber is retired at a verified handoff or on script completion.
    // This does NOT certify arbitrary live LLVM values.
    static const bool deferNativeAutomaticGc = [] {
        const char* value = std::getenv("XYO_RT_REQUIRE_GENERATED_SSA_ROOTS");
        return value && value[0] == '1';
    }();
    static const bool traceNativeGcDeferral = [] {
        const char* value = std::getenv("XYO_RT_TRACE_NATIVE_GC_DEFERRAL");
        return value && value[0] == '1';
    }();
    // The source-certified budget request is per actual scheduler
    // activation. Do not reuse a stale request on the next resume.
    thread.nativeGcSoftBudgetYieldDue = false;
    if (deferNativeAutomaticGc) {
        const uint64_t before = traceNativeGcDeferral
            ? Gc::get().collectCount() : 0;
        const size_t bytesBefore = traceNativeGcDeferral
            ? Gc::get().bytesAllocated() : 0;
        {
            Gc::DeferCollect defer;
            Gc::DeferExplicitCollect deferExplicit;
            c->fiber->resume();
        }
        if (traceNativeGcDeferral) {
            const uint64_t after = Gc::get().collectCount();
            const size_t bytesAfter = Gc::get().bytesAllocated();
            const size_t deferredBytes = bytesAfter >= bytesBefore
                ? bytesAfter - bytesBefore : 0;
            std::fprintf(stderr,
                "[native-gc-deferred] script=%s before=%llu "
                "after=%llu unchanged=%d allocated_bytes=%zu\n",
                thread.topBlock.c_str(),
                static_cast<unsigned long long>(before),
                static_cast<unsigned long long>(after),
                int(before == after), deferredBytes);
        }
    } else {
        c->fiber->resume();
    }
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
        // A deferred native explicit collection is safe to perform
        // once this native activation has FINISHED and no unrooted
        // SSA registers can be resumed. Do not drain on mere yield.
        if (deferNativeAutomaticGc &&
            (Gc::get().explicitCollectionPending() ||
             thread.nativeGcSoftBudgetAttempted)) {
            const auto before = Gc::get().collectCount();
            const size_t bytesBefore = Gc::get().bytesAllocated();
            // A rejected soft budget request can only be collected
            // AFTER the native Fiber completes. The usual GC threshold
            // may be larger than the configured soft budget, so a
            // plain collectIfNeeded() is insufficient in that case.
            if (thread.nativeGcSoftBudgetAttempted)
                Gc::get().collect();
            else
                Gc::get().collectIfNeeded();
            if (thread.nativeGcSoftBudgetAttempted)
                std::fprintf(stderr,
                    "[native-gc-soft-budget-finished] script=%s "
                    "collected=%d bytes_before=%zu bytes_after=%zu "
                    "pending=%d active_fibers=%zu\n",
                    thread.topBlock.c_str(),
                    int(Gc::get().collectCount() > before), bytesBefore,
                    Gc::get().bytesAllocated(),
                    int(Gc::get().explicitCollectionPending()),
                    FiberPool::activeCount());
            if (traceNativeGcDeferral &&
                !thread.nativeGcSoftBudgetAttempted)
                std::fprintf(stderr,
                    "[native-ssa-explicit-gc-finished] script=%s "
                    "before=%llu after=%llu pending=%d\n",
                    thread.topBlock.c_str(),
                    static_cast<unsigned long long>(before),
                    static_cast<unsigned long long>(
                        Gc::get().collectCount()),
                    int(Gc::get().explicitCollectionPending()));
        }
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
    runtime.untrackWaitDependencies(thread);
    thread.pendingShortBroadcast = false;
    // A killed async Fiber does not unwind the C++ reporter's stack.
    // Release Thread-owned LLVM Variable::value roots on retirement.
    // Every external GC sweep is still guarded while other Fibers run.
    if (!thread.nativeAsyncVariableRoots.empty() &&
        std::getenv("XYO_RT_TRACE_ASYNC_VAR_ROOTS")) {
        size_t heapRoots = 0;
        for (const auto& owner : thread.nativeAsyncVariableRoots)
            heapRoots += owner->size();
        std::fprintf(stderr,
            "[native-async-var-retire] owners=%zu heap_roots=%zu "
            "gc_safe=0\n",
            thread.nativeAsyncVariableRoots.size(), heapRoots);
    }
    thread.nativeAsyncVariableRoots.clear();
    thread.nativeAsyncReporterReturnPc = 0;
    thread.nativeAsyncReporterFramePtr = 0;
    thread.nativeAsyncReporterInputBuffer = 0;
    thread.nativeAsyncReporterInputWords = 0;
    thread.numericSession.reset();
    // JS 版は stack のみクリアし stackFrames は残す（edge-activated hat が
    // retire 直後に goToNextBlock → reuseStackForNextBlock で最終フレームを
    // 再利用するため）。
    thread.stack.clear();
    thread.requestScriptGlowInFrame = false;
    // Retirement can be called by external stop/broadcast paths as well as
    // from the running scheduler; invalidate any cached runnable membership.
    runtime.transitionThread(thread, STATUS_DONE);
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
    // Exact GcValueHandles source ownership is a serial scheduler
    // safe-point operation. Do not run the generated SSA experiment on
    // worker threads or through Fiber::resumeChain, even if a caller
    // enabled the GC-root producer without enabling a deopt probe.
    const char* generatedRoots =
        std::getenv("XYO_RT_REQUIRE_GENERATED_SSA_ROOTS");
    // The opt-in native Variable* rebind calls Target::lookupOrCreate
    // at each use. Target's VariableMap is intentionally serial and
    // can be structurally mutated by the editor; never resolve these
    // borrowed nodes concurrently with another worker's map access.
    const char* guardVarSlots =
        std::getenv("XYO_RT_GUARD_NATIVE_VAR_SLOTS");
    if ((handoff && handoff[0] == '1') ||
        (deopt && deopt[0] == '1') ||
        (generatedRoots && generatedRoots[0] == '1') ||
        (guardVarSlots && guardVarSlots[0] == '1')) return false;
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
    // A stale v3 script must not even enter a parallel batch or reuse
    // a cached worker eligibility decision from before an editor change.
    if (cs && cs->aotSourceRevision &&
        (!t.blockContainer || cs->aotSourceBlocks != t.blockContainer ||
         cs->aotSourceRevision != t.blockContainer->revision()))
        return nullptr;
    // GPU helpers may invoke the renderer or shared compute backends, even in
    // a script whose Scratch-level effects are purely numeric.
    if (!cs || !cs->entry || cs->wasm ||
        cs->mainThread || !cs->gpuProgs.empty()) return nullptr;
    const AccessSet* as = runtime.jit->accessFor(t, runtime);
    const AccessSet* res = as && as->workerSafe() ? as : nullptr;
    // Parallel native execution must not trigger lazy Blocks cache mutation
    // from worker threads. Resolve only worker-eligible scripts here, after
    // the last parallel batch has already joined.
    if (res && !cs->aotLazyWorkerSlots.empty() &&
        !runtime.jit->prepareAotWorker(*cs)) res = nullptr;
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
