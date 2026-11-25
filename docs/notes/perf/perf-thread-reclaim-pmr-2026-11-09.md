# PERF-001 — Thread回収のスタック内ハッシュ表（2026-11-09）

## 問題

`Runtime::reclaimThreads()` は各スケジューラ周期に、実行・待機・結合・ネイティブFiber・デタッチ済みスレッドから到達可能なThread*を追跡し、残りをリサイクルする。元の処理は `std::unordered_set<const Thread*>` に毎回ノードとバケットをヒープ確保する。機械的な参照追跡の簡略化はGC/ABA安全性の問題になり得るため、**参照到達性アルゴリズムを一切変更せず**割り当て手段だけを変える。

一時診断では、1回の回収直前に`clonechurn`約35,000個の所有Threadがあるのに生存参照は1個、`fanout`約84,000個に対して1個、`fanwait`約83,000個に対して102個、`pingpong`約49,000個に対して1〜2個だった。一時診断コードは残さない。

## 実装

`src/engine/runtime.cpp::Runtime::reclaimThreads()` に実験設定 `XYO_RECLAIM_INLINE_HASH=1` を追加。

- スタック上の**4KB**の適切にアラインした未初期化領域を `std::pmr::monotonic_buffer_resource` に渡す。
- 同じハッシュ表インターフェースの `std::pmr::unordered_set<const Thread*>` を使い、要素の追加・重複確認・最終的な生存判定を元と同じ条件で行う。
- ノードやバケットが4KBを超えた場合は、標準のupstreamヒープリソースへ自動的にフォールバックする。
- `XYO_THREAD_RECLAIM_STAMP=1` の既存世代番号方式は最優先で残し、それ以外の実行状態には触れない。
- 既定OFF（全体性能・正しさの最終検証後に採否決定）。

比較対象は元の `scratch-bench/performance` 由来の `build/scratchbench-cause-probes/projects/*.sb3` で、変更なし。CPU14 P-core固定、同じ凍結Releaseバイナリ `build/perf-reclaim-pmr-frozen-xyo`、SHA256 `f237791eb37388f0f54f650f2ec0d47d149d056f6b4f1cd1209f3a5f3f9cf614`。

## 同一バイナリの交互A/B

| 項目 | OFF中央値 ops/ms | ON中央値 ops/ms | ペアON勝利 | ペア速度比の幾何平均 |
|---|---:|---:|---:|---:|
| `sched_clonechurn` (24組) | 1,073.38 | **1,094.04** | **21/24** | **1.02008** |
| `sched_fanwait` (24組) | 2,375.10 | 2,376.71 | 12/24 | 0.99739 |
| `sched_fanout` (16組) | 2,414.75 | 2,407.89 | 4/16 | 0.99312 |
| `sched_pingpong` (16組) | 1,460.20 | 1,472.72 | 10/16 | 1.00698 |
| `sched_waituntil` (16組) | 1,031.62 | 1,033.65 | 7/16 | 1.00120 |

`clonechurn` は約2.0%改善。fanwait/pingpong/waituntilは明確な差なし、fanoutは約0.7%の低下傾向であり、この変更だけでは**全47項目の改善**は実証されていない。生データ `build/perf-reclaim-pmr-ab.{json,log}` および `build/perf-reclaim-pmr-cross-ab.{json,log}`。

## 棄却した比較

- `XYO_THREAD_RECLAIM_STAMP=1`（既存オプトイン）はclonechurnで+2.46%/20勝24組だったが、fanout-1.70%、fanwait-2.01%、pingpong-0.79%傾向があり、既定ONは棄却。
- Thread再利用プールの2,048→8,192/16,384/32,768/65,536上限変更は速度比が概ね1〜1.5%で12組中9勝以下、保持メモリ増加を伴うため完全撤回。
- 生存Thread 7個以下の小配列とハッシュ表を使い分ける実験はclonechurn +1.53%、fanwait -0.90%。1個だけ小配列にするとclonechurn +2.08%/22勝24組、fanwaitほぼ同等になったが、JITとGCを伴う全ユニットテストで**一度SIGSEGV（`Gc::drainPending`）**を起こし、次の再実行ではPASSした。原因が説明できないメモリ安全性リスクを採用できないため、**小集合方式のソース変更は完全に撤回済み**。クラッシュバックトレース `build/perf-reclaim-single-core-bt.log`、ON失敗/再実行PASS/OFF PASSログ `build/perf-reclaim-single-ctest-{on,on2,off}.log`。

## 正しさ

`tests/unit/test_scheduler.cpp` に、320個の保持中Threadと128個の回収可能Threadを作り、4KB PMR領域からヒープへ溢れる場合の到達性・所有順序保持と参照解放後の完全回収を検査する `testReclaimLargeJoinClosure()` を追加。既存 `testThreadReclamation()` には `XYO_THREAD_RECLAIM_STAMP=1` の64bit回収世代ラップアラウンドで、フレーム・join・JIT・待機等の参照が残ることの回帰テストを追加。結果は検証完了後に記入。

現状の実験経路はネイティブC++17 PMRを使用する。Wii等のツールチェーンでの互換性は、既定化する場合に別途検証またはコンパイルガードを追加する。元のScratchBench 47ケース全勝は未達、他エージェントのソースは維持しコミット/pushはしない。
