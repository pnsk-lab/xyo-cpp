# Mega Scheduler β — 非Megaスレッドの事前判定

2026-11-13。既定ON、=0で旧経路に復帰可能。共有作業ツリーで未コミット。

## 変更内容

src/engine/sequencer.cpp の runSchedulerPass に事前判定を追加。
従来はどの通常Threadでも tryMegaScalarBatch を呼んでいたが、
既定ONの事前判定では Thread::megaDispatch が
存在する場合にのみ大きな検証関数を呼び出す。

詳細検証は従来どおり tryMegaScalarBatch 内で実行する。
XYO_JIT_MEGA_SCHED_PREFILTER=0で旧経路へ切り戻す。判定はScratch状態を書き換えない。
Scratchスケジューラ順序、動的ブロードキャスト、wait、
Thread終了の処理には変更を加えない。

大量の通常Threadに対して既存関数内の複数getenv、runner状態、
CompiledScript型などの不要な検査を削減する。特にMegaでない
スクリプトが大量に動作する場合に効果が期待できる。

## 実行

既定ON。計測・検証のためXYO_JIT_MEGA_SCHED_PREFILTER=0で無効化できる。
他のMegaフラグの有無にかかわらず、Mega未装着Threadは
従来の通常スケジューラ経路を使用する。

## ScratchBench A/B

同じバイナリ、同じSB3、CPU16固定、eager、8組交互測定。
全ての他のMega実験フラグを同一にして、新しい1フラグのみON/OFF。
スコアは大きいほどよい。実行可能なScratchカーネルから未選択suiteの
呼出箇所のみを除去するbench_scheduler_flags.pyを使用。

| 項目 | OFF中央値 | ON中央値 | 対応ペア速度比 | ON勝利 |
|---|---:|---:|---:|---:|
| sched_threads | 1095.674 | 5093.009 | **4.6368倍** | 8/8 |
| sched_pingpong | 126.608 | 185.649 | **1.5180倍** | 8/8 |
| sched_clonechurn | 117.056 | 168.605 | **1.3562倍** | 7/8 |
| sched_fanwait | 391.704 | 405.252 | 0.9655倍 | 4/8 |

元データ: build-scheduler-rewrite-20261111/mega-ifelse/bench-prefilter-sched/results.json

この測定ではsched_threadsなどで顕著な改善を確認したが、
他のScratchBench項目・実際のプロジェクトでの再現を保証しない。
また、上記はON/OFFの比較でありThread処理全体の性能を示すものではない。

## 検証

ビルド xyo-tests/xyo-cli 成功。全体回帰は実行中。
新フラグONの全体テスト: Scheduler **593/593 PASS**、他のテストスイートも失敗ゼロ（終了コード0）。
ログ: build-scheduler-rewrite-20261111/mega-ifelse/sched-prefilter-validation/full-on.out。

追加検証: sched_threads単独10組交互測定、OFF中央値965.03、ON中央値3784.03、対応ペア速度比**4.0896倍**、**10/10組改善**。
元データ: build-scheduler-rewrite-20261111/mega-ifelse/bench-prefilter-sched-threads-isolated/results.json。

改善幅は測定環境・入力に依存。既定ONでも完全なMegaバッチ判定を通過したThreadのみ一括実行し、それ以外は通常経路を保持する。
変更をコミットせず、他エージェントの未コミット編集を保持する。


## 既定HotSpotテストの比較（機能由来ではない失敗）

環境変数を一切指定しない、JIT_MODE=HotSpot（既定）での
xyo-tests全体実行は終了コード1で、以下の5件を報告した。
この同一バイナリに対して
XYO_JIT_MEGA_SCHED_PREFILTER=0
を明示した再実行でも**全く同じ5件**が報告された。

- tests/unit/test_e2e.cpp:688 / 1067 — t->isCompiledを初回開始時に要求
- tests/unit/test_e2e.cpp:695 / 706 — NaNのtag表現をJIT生成済みと仮定
- scheduler: all fanout writer and waiter scripts compile to LLVM

いずれも未適用コンパイル等のHotSpot/Eager前提に関係するテスト期待値。
今回の前処理はrunSchedulerPass中だけであり、これらのコード生成・
実行モード選択には関与しない。新規のデフォルト回帰として扱わない。
比較元ログ:
- build-scheduler-rewrite-20261111/mega-ifelse/sched-prefilter-validation/full-default.out
- build-scheduler-rewrite-20261111/mega-ifelse/sched-prefilter-validation/full-hotspot-prefilter-off.out

Eager＋すべてのMega実験フラグ＋事前判定ONで実行した
全体テストは、終了コード0、scheduler **593/593件PASS**、
その他の全スイートでも失敗なし。

