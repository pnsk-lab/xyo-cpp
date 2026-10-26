// xyo-cpp: engine/sequencer.js 移植（profiler / JIT 分岐を除く）。
#include "sequencer.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>

#include "../core/gc.hpp"
#include "../core/sstr.hpp"
#include "../jit/jit.hpp"
#include "../model/target.hpp"
#include "deps.hpp"
#include "fiber.hpp"
#include "pool.hpp"
#include "runtime.hpp"

namespace xyo {

namespace {

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

/// fiber エントリ: コンパイル済みスクリプトを実行（yield は fiber switch）
void jitEntry(void* arg) {
    JitCtx* c = static_cast<JitCtx*>(arg);
    const CompiledScript* cs =
        static_cast<const CompiledScript*>(c->thread->compiled);
    c->warpStartMs = c->runtime->currentMSecs;
    cs->entry(c);
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
    *c = JitCtx{};
    auto& p = jitCtxPool();
    if (p.size() < kJitCtxPoolCap) p.push_back(c);
    else delete c;
}

} // namespace

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
                const AccessSet* access = allowParallel
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
                    runtime.threads[nextActive++] = t;
                } else {
                    runtime.threadMap.erase(t->getId());
                    doneThreads.push_back(t);
                }
            }
            runtime.threads.resize(nextActive);
        }
    }

    this->activeThread = nullptr;
    stepping = nullptr;
    detachedLists.clear();
    return doneThreads;
}

void Sequencer::stepThread(Thread& thread) {
    if (thread.isCompiled) {
        std::optional<ProfScope> ps;
        if (prof().enabled) { ps.emplace(&prof().stepJitNs); ++prof().nJit; }
        jitStepThread(thread);
        return;
    }
    std::optional<ProfScope> ps;
    if (prof().enabled) { ps.emplace(&prof().stepInterpNs); ++prof().nInterp; }
    stepThreadBound(thread, 0);
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
void Sequencer::stepThreadBound(Thread& thread, size_t minDepth) {
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
        const size_t initialStackSize = thread.stack.size();
        bool isWarpMode = thread.peekStackFrame()->warpMode;
        if (isWarpMode && !thread.hasWarpTimer) {
            thread.hasWarpTimer = true;
            thread.warpTimer.start();
        }
        if (thread.target == nullptr) {
            retireThread(thread);
        } else {
            execute(*this, thread);
            if (countBlocks) ++blockCounter.n;
        }
        thread.blockGlowInFrame = currentBlockId;

        if (thread.status == STATUS_YIELD) {
            thread.status = STATUS_RUNNING;
            if (isWarpMode && !warpBudgetExhausted(thread)) {
                continue;
            }
            return;
        } else if (thread.status == STATUS_PROMISE_WAIT) {
            return;
        } else if (thread.status == STATUS_YIELD_TICK) {
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
    const bool isRecursive = thread.isRecursiveCall(procedureCode);
    thread.pushStack(definition);
    if (thread.peekStackFrame()->warpMode &&
        warpBudgetExhausted(thread)) {
        thread.status = STATUS_YIELD;
    } else {
        const Block* definitionBlock = thread.target->blocks->getBlock(definition);
        const BlockInput* cb = definitionBlock->inputs.find("custom_block");
        const Block* innerBlock = cb ? thread.target->blocks->getBlock(cb->block) : nullptr;
        bool doWarp = false;
        if (innerBlock && innerBlock->mutation) {
            doWarp = innerBlock->mutation->warp();
        }
        if (doWarp) {
            thread.peekStackFrame()->warpMode = true;
        } else if (isRecursive) {
            thread.status = STATUS_YIELD;
        }
    }
}

/// コンパイル済みスレッド: fiber 経由で生成コードを実行。
/// fiber の yield は「他スレッドへ譲る」チェックポイント（同一パスで再開）。
/// status は生成コード側のヘルパが必要時に設定する。
void Sequencer::jitStepThread(Thread& thread) {
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
        c->sequencer = this;
        c->compatEc = &thread.compatibilityStackFrame;
        thread.jitCtx = c;
        if (!cs->mayYield) {
            // yield 点を持たないスクリプト — fiber を介さず同期実行。
            // broadcast 受信等の短命スレッドの切替コストを削る。
            std::optional<ProfScope> ps;
            if (prof().enabled) { ps.emplace(&prof().syncNs); ++prof().nSync; }
            c->fiber = nullptr;
            c->warpStartMs = runtime.currentMSecs;
            cs->entry(c);
            thread.status = STATUS_DONE;
            thread.stack.clear();
            releaseJitCtx(c);
            thread.jitCtx = nullptr;
            return;
        }
        thread.fiber = FiberPool::acquire(jitEntry, c);
        c->fiber = thread.fiber;
    }
    c->target = thread.target;
    std::optional<ProfScope> ps;
    if (prof().enabled) { ps.emplace(&prof().fiberNs); ++prof().nFiber; }
    c->fiber->resume();
    if (c->fiber->finished()) {
        // エントリ関数が戻った = スクリプト完了。Thread オブジェクトは
        // ownedThreads で ~Runtime まで生存するため、fiber/jitCtx は
        // ここで即座にプールへ戻す（broadcast 等の大量スレッド生成対策）。
        thread.status = STATUS_DONE;
        thread.stack.clear();
        FiberPool::release(c->fiber);
        thread.fiber = nullptr;
        releaseJitCtx(c);
        thread.jitCtx = nullptr;
    }
}

void Sequencer::retireThread(Thread& thread) {
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
    static const bool enabled = std::getenv("XYO_PAR") == nullptr ||
                                std::string(std::getenv("XYO_PAR")) != "0";
    // Affinity is a per-script effect, not a property of the whole Runtime.
    // Graphical/IO-mutating scripts remain serial barriers; independent pure
    // computation may run on workers while a renderer is attached.
    return enabled && runtime.jit;
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
    auto* cs = static_cast<const CompiledScript*>(t.compiled);
    // GPU helpers may invoke the renderer or shared compute backends, even in
    // a script whose Scratch-level effects are purely numeric.
    if (!cs || !cs->entry || cs->mainThread || !cs->gpuProgs.empty()) return nullptr;
    const AccessSet* as = runtime.jit->accessFor(t, runtime);
    return as && as->workerSafe() ? as : nullptr;
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

    std::vector<std::vector<size_t>> groups;
    static const char* graphDump = std::getenv("XYO_DEPGRAPH");
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
    if (prof().enabled) {
        prof().parThreads += batch.size();
        prof().parGroups += groups.size();
        ++prof().parBatches;
    }
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
