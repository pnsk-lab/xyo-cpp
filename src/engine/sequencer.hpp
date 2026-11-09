// xyo-cpp: engine/sequencer.js 相当。
#pragma once

#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

#include "pool.hpp"
#include "thread.hpp"
#include "timer.hpp"

namespace xyo {

struct Runtime;
struct Block;
namespace wasm { class NestedJitCache; }
struct DepGraph;
struct AccessSet;
class Fiber;
class WarpClockWatchdog;
struct NativeSourceHandoff;

class Sequencer {
public:
    explicit Sequencer(Runtime& rt);
    ~Sequencer();

    /// Available only while stepThreads() is executing with a steady clock.
    std::atomic<int64_t>* sampledWarpClock() const;
    std::atomic<int64_t>* sampledWarpDeadline() const;
    std::atomic<int64_t>* sampledUnixClock() const;
    std::atomic<uint64_t>* sampledUnixDays() const;

    Runtime& runtime;
    Timer timer;
    // Incremented at every tick and after actual Scratch thread progress.
    // This avoids per-variable write barriers, and intentionally over-wakes
    // waiters when an unrelated script runs.
    std::atomic<uint64_t> eventProgressEpoch{0};
    bool eventSchedulerEnabled() const;
    void armEventWait(Thread& thread, uintptr_t site);
    Thread* activeThread = nullptr;

    /// JS: `const threads = this.runtime.threads` — パス中に掴んだ配列。
    /// stopAll() で runtime.threads が新配列に置き換わっても旧配列を
    /// 反復し続けられるようポインタ経由にする（stopAll が書き換える）。
    std::vector<Thread*>* stepping = nullptr;
    /// detach された旧スレッド配列の寿命管理（stepThreads 生存期間中）
    std::vector<std::unique_ptr<std::vector<Thread*>>> detachedLists;

    /// warp スレッドの時間予算（`Runtime::warpTimeMs`、TurboWarp の
    /// `WARP_TIME` = 500ms 相当）を使い切ったか。`Runtime::dangerMode`
    /// では予算が無制限（+inf）なので常に false = 時間では譲らない。
    bool warpBudgetExhausted(const Thread& thread) const;

    /// 全スレッドを 1 ティック分ステップ。完了スレッドを返す。
    std::vector<Thread*> stepThreads();
    /// 1 スレッドを可能な限り進める。
    void stepThread(Thread& thread);
    /// stack.size() > minDepth の間だけ進める（JIT compat 入力評価用）
    // Optional top-level checkpoint stops *before* the selected block.
    // yielded is set when an interpreter scheduling checkpoint was reached,
    // even when its status is normalized back to STATUS_RUNNING.
    void stepThreadBound(Thread& thread, size_t minDepth,
                         const std::string* stopAt = nullptr,
                         bool* yielded = nullptr);
    // Called only from execute() after the interpreter has resolved its
    // BlockCached. Avoid a second block lookup on every interpreted opcode.
    bool tryNestedWasm(Thread& thread, const BlockCached& cached,
                       std::string& pastEndId);
    bool nestedWarpOnly() const noexcept { return nestedWarpOnly_; }
    /// コンパイル済みスレッドを fiber 経由で進める
    void jitStepThread(Thread& thread);
    // Transfer a suspended native LLVM CallFrame, its immutable source-PC
    // decoder and independently rooted arguments to the scheduler. The
    // result is data only; arbitrary SSA/effect resumption is unsupported.
    std::unique_ptr<NativeSourceHandoff> captureNativeSourceHandoff(
        const Thread& thread) const;
    // Strict opt-in conversion from a validated native repeat yield to an
    // interpreter loop frame. On rejection the native Fiber is untouched.
    bool adoptSimpleNativeSourceHandoff(
        Thread& thread, const NativeSourceHandoff& handoff);
    // Explicitly attached shared-IR numeric continuation, experimental.
    void numericStepThread(Thread& thread);
    void stepToBranch(Thread& thread, int branchNum, bool isLoop);
    void stepToProcedure(Thread& thread, const std::string& procedureCode);
    void retireThread(Thread& thread);

    // --- 並列実行（スレッド間メモリ依存グラフ）---
    /// 並列機能有効か（XYO_PAR=0 で無効化）
    bool parallelEnabled() const;
    /// このスレッドを並列バッチに入れられるか（JIT 済み・依存解析が
    /// worker-safe。yield する fiber も可）
    bool parallelEligible(Thread& t) const;
    /// flushParBatch の実行後帳簿
    struct ParFlush {
        bool stopped = false;     // 完了/破棄スレッドあり
        size_t active = 0;        // 実行後も RUNNING のスレッド数
    };
    /// 溜まった適格スレッドを実行（独立グループはワーカー並列、
    /// 衝突グループ内は spawn 順直列）
    ParFlush flushParBatch();
    /// XYO_DEPGRAPH 環境変数で衝突グラフを DOT 出力
    void dumpDepGraph(const DepGraph& g,
                      const std::vector<Thread*>& batch);

    struct ParallelStats {
        uint64_t batches = 0, dispatches = 0, threads = 0, groups = 0;
        uint64_t chainedPasses = 0;
    };
    const ParallelStats& parallelStats() const { return parStats_; }

private:
    // Nested interpreter statements can be compiled independently of the
    // event hat while the interpreter retains exact loop/procedure/yield
    // frames. No native JIT backend uses this cache.
    std::unique_ptr<wasm::NestedJitCache> nestedJit_;
    bool nestedWarpOnly_ = false;
    std::unique_ptr<WarpClockWatchdog> warpWatchdog_;
    const AccessSet* parallelAccessFor(Thread& t) const;
    std::unique_ptr<WorkerPool> parPool_;
    std::vector<Thread*> parBatch_;
    std::vector<const AccessSet*> parSets_;
    std::vector<Fiber*> parFibers_;
    ParallelStats parStats_;
    int depGraphSeq_ = 0;
    uint64_t eventSkippedPasses_ = 0;
    uint64_t eventWakeups_ = 0;
    std::unordered_map<const Block*, bool> idleSoundPollCache_;
    bool idleSoundPollFor(const Thread& thread);
};

} // namespace xyo
