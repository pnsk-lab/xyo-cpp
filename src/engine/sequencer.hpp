// xyo-cpp: engine/sequencer.js 相当。
#pragma once

#include <memory>
#include <vector>

#include "pool.hpp"
#include "thread.hpp"
#include "timer.hpp"

namespace xyo {

struct Runtime;
struct DepGraph;

class Sequencer {
public:
    explicit Sequencer(Runtime& rt) : runtime(rt) {}

    Runtime& runtime;
    Timer timer;
    Thread* activeThread = nullptr;

    /// JS: `const threads = this.runtime.threads` — パス中に掴んだ配列。
    /// stopAll() で runtime.threads が新配列に置き換わっても旧配列を
    /// 反復し続けられるようポインタ経由にする（stopAll が書き換える）。
    std::vector<Thread*>* stepping = nullptr;
    /// detach された旧スレッド配列の寿命管理（stepThreads 生存期間中）
    std::vector<std::unique_ptr<std::vector<Thread*>>> detachedLists;

    static constexpr double WARP_TIME = 500.0;

    /// 全スレッドを 1 ティック分ステップ。完了スレッドを返す。
    std::vector<Thread*> stepThreads();
    /// 1 スレッドを可能な限り進める。
    void stepThread(Thread& thread);
    /// stack.size() > minDepth の間だけ進める（JIT compat 入力評価用）
    void stepThreadBound(Thread& thread, size_t minDepth);
    /// コンパイル済みスレッドを fiber 経由で進める
    void jitStepThread(Thread& thread);
    void stepToBranch(Thread& thread, int branchNum, bool isLoop);
    void stepToProcedure(Thread& thread, const std::string& procedureCode);
    void retireThread(Thread& thread);

    // --- 並列実行（スレッド間メモリ依存グラフ）---
    /// 並列機能有効か（XYO_PAR=0 で無効化）
    bool parallelEnabled() const;
    /// このスレッドを並列バッチに入れられるか（JIT 済み・yield 無し・
    /// 依存解析が安全）
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

private:
    std::unique_ptr<WorkerPool> parPool_;
    std::vector<Thread*> parBatch_;
    int depGraphSeq_ = 0;
};

} // namespace xyo
