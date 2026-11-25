# PERF-001 — Thread回収のスタック内ハッシュ表（2026-11-09）

## 問題

`Runtime::reclaimThreads()` はスケジューラの各周期に、実行・待機・結合・ネイティブFiber・デタッチ済みのスレッドから到達可能な Thread* を追跡し、残りをリサイクルする。元の処理は `std::unordered_set<const Thread*>` のノードとバケットを毎回ヒープに確保する。参照追跡を機械的に簡略化すると GC/ABA 安全性の問題になり得るため、**参照到達性アルゴリズムを一切変更せず**割り当て手段だけを変える。

一時診断では、1回の回収の直前に、`clonechurn` は約35,000個の所有 Thread に対して生存参照が1個、`fanout` は約84,000個に対して1個、`fanwait` は約83,000個に対して102個、`pingpong` は約49,000個に対して1〜2個だった。一時診断コードは残さない。

## 実装

`src/engine/runtime.cpp::Runtime::reclaimThreads()` に実験設定 `XYO_RECLAIM_INLINE_HASH=1` を追加。

- スタック上の、適切にアラインした **4KB** の未初期化領域を `std::pmr::monotonic_buffer_resource` に渡す。
- 同じハッシュ表インターフェースの `std::pmr::unordered_set<const Thread*>` を使い、要素の追加・重複確認・最終的な生存判定を元と同じ条件で行う。
- ノードやバケットが4KBを超えた場合は、標準のupstreamヒープリソースへ自動的にフォールバックする。
- `XYO_THREAD_RECLAIM_STAMP=1` の既存世代番号方式は最優先で残し、それ以外の実行状態には触れない。
- 既定OFF（全体性能・正しさの最終検証後に採否決定）。

比較対象は元の `scratch-bench/performance` 由来の `build/scratchbench-cause-probes/projects/*.sb3` で、変更はない。CPU は14 P-core に固定し、同じ凍結済み Release バイナリ `build/perf-reclaim-pmr-frozen-xyo`（SHA256 `f237791eb37388f0f54f650f2ec0d47d149d056f6b4f1cd1209f3a5f3f9cf614`）を使った。

## 同一バイナリの交互A/B

| 項目 | OFF中央値 ops/ms | ON中央値 ops/ms | ペアON勝利 | ペア速度比の幾何平均 |
|---|---:|---:|---:|---:|
| `sched_clonechurn` (24組) | 1,073.38 | **1,094.04** | **21/24** | **1.02008** |
| `sched_fanwait` (24組) | 2,375.10 | 2,376.71 | 12/24 | 0.99739 |
| `sched_fanout` (16組) | 2,414.75 | 2,407.89 | 4/16 | 0.99312 |
| `sched_pingpong` (16組) | 1,460.20 | 1,472.72 | 10/16 | 1.00698 |
| `sched_waituntil` (16組) | 1,031.62 | 1,033.65 | 7/16 | 1.00120 |

`clonechurn` は約2.0%改善。fanwait/pingpong/waituntil は明確な差がなく、fanout は約0.7%の低下傾向で、この変更だけでは**全47項目の改善**は実証されていない。生データ `build/perf-reclaim-pmr-ab.{json,log}` および `build/perf-reclaim-pmr-cross-ab.{json,log}`。

## 棄却した比較

- `XYO_THREAD_RECLAIM_STAMP=1`（既存のオプトイン）は `clonechurn` で +2.46%、24組中20勝だったが、fanout は -1.70%、fanwait は -2.01%、pingpong は -0.79% の傾向があり、既定ONは棄却した。
- Thread 再利用プールの上限を 2,048 から 8,192 / 16,384 / 32,768 / 65,536 に変える案は、速度比が概ね 1〜1.5% で12組中9勝以下にとどまり、保持メモリの増加も伴うため完全に撤回した。
- 生存 Thread が7個以下のときに小配列とハッシュ表を使い分ける実験は、`clonechurn` が +1.53%、fanwait が -0.90% だった。小配列の適用を1個だけにすると `clonechurn` は +2.08%、24組中22勝、fanwait はほぼ同等になったが、JIT と GC を伴う全ユニットテストで**一度SIGSEGV**（`Gc::drainPending`）を起こし、次の再実行ではPASSした。原因を説明できないメモリ安全性リスクは採用できないため、**小集合方式のソース変更は完全に撤回済み**。クラッシュバックトレース `build/perf-reclaim-single-core-bt.log`、ON失敗・再実行PASS・OFF PASS のログ `build/perf-reclaim-single-ctest-{on,on2,off}.log`。

## 正しさ

`tests/unit/test_scheduler.cpp` に、保持中の Thread 320個と回収可能な Thread 128個を作り、4KB PMR 領域からヒープへ溢れる場合の到達性・所有順序の保持と、参照解放後の完全回収を検査する `testReclaimLargeJoinClosure()` を追加した。既存の `testThreadReclamation()` には、`XYO_THREAD_RECLAIM_STAMP=1` の64bit回収世代がラップアラウンドしてもフレーム・join・JIT・待機などの参照が残ることを確認する回帰テストを追加した。結果は検証完了後に記入。

現状の実験経路はネイティブの C++17 PMR を使う。Wii などのツールチェーンでの互換性は、既定化する場合に別途検証するか、コンパイルガードを追加する。元の ScratchBench 47ケース全勝は未達で、他エージェントのソースは維持し、コミット/push はしない。
