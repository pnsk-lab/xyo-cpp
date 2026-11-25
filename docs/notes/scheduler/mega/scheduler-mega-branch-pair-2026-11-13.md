# Mega Scheduler β — 分岐内2命令と世代付き変数キャッシュ

2026-11-13。実験的、既定OFF、共有作業ツリーで未コミット。

## 実装

- src/jit/mega_yield_source.hpp: 真偽分岐それぞれを最大2つの連続した定数加算命令として静的に証明。3命令以上、動的レポータ、分岐の入れ子、他の命令は拒否。2命令のソースは XYO_JIT_MEGA_BRANCH_PAIR=1 のときだけ受理する。
- src/jit/jit.cpp: 両分岐の2番目の変数IDについて、グループ融合前に存在確認する。
- src/jit/jit.hpp: MultiPcItemは真1・偽1・条件・真2・偽2の最大5変数ポインタとPC・残反復の合計7ポインタ。追加のソースメタデータで各分岐の2番目の命令の有無を判別。
- src/jit/backend_llvm.cpp: LLVM switch(pc)内で選ばれた枝のload/fadd/storeをソース順で2回実行する。第1回目と第2回目が同じ変数でも加算をまとめず、値を再loadする。普通のMega継続のフォールバックにも第二命令を追加。
- src/engine/sequencer.cpp: 各変数ポインタを数値型・有限値としてチェック。Target/Stageの構造世代が一致する間だけ既存Thread::megaScalarBindingsを再利用。構造変更時には古いポインタを参照する前に破棄し、jitVarCacheも失効させる。毎passの最大5回のマップ検索を削減。

## 安全性

- Scratch Threadを束ねるのは1回のscheduler passにつき最大1回のrepeat本体実行のみ。join集合、Thread identity、元の実行順を変更しない。
- GC、文字列、リスト、外部コールバックを含む任意のScratchスクリプトへは適用しない。
- 数値以外の型、非有限値、バインディングの失効、実際の待機・観測状態は従来JITへフォールバック。
- 非対応の処理は元のJIT経路に残し、新機能フラグは既定OFF。

## 回帰テスト

- tests/unit/test_scheduler.cpp: testMegaConditionalTwoChangeBatchで、同じ変数xに-0.5を2回書く枝、別変数dへの追加書込み、分岐の順序、数値文字列からのフォールバック、ORCコード再利用を確認。
- 反復途中で2番目の書込先変数dを削除・再作成し、構造世代チェック後の新しい変数への書き込みを確認。
- testMegaIfNoElsePairBatchでは真枝2命令と偽枝no-op、PCと残反復状態、文字列フォールバックを検査。
- 集中テスト: 57 checks, 0 failures。

最終ソースの全体テスト（全テスト群とも終了コード0、失敗なし）：

| 条件 | Scheduler | 結果 |
|---|---:|---|
| XYO_JIT_MEGA_BRANCH_PAIR=1 | 593/593 | PASS |
| XYO_JIT_MEGA_BRANCH_PAIR=0 | 562/562 | PASS |
| XYO_WAIT_WAKE_REGISTRY=1、branch pair要求ON | 509/509 | PASS |

原データ: build-scheduler-rewrite-20261111/mega-ifelse/branch-pair-final-{on,off,wait}.out。


## 合成SB3ベンチマーク

CPU16固定、同一バイナリ、ON/OFF交互測定。8本の緑旗スクリプト、各40,000回repeat。分岐に2つの変数更新があり、16変数の最終値を毎回検証。時間は起動・コンパイルを含む壁時計時間で小さいほど高速。

| 条件 | OFF中央値 | ON中央値 | 対応ペア速度比 | ON勝利 |
|---|---:|---:|---:|---:|
| 導入直後・キャッシュなし (8組) | 565.12 ms | 579.92 ms | 0.9823倍 | 1/8 |
| 世代付きキャッシュあり if/else (10組) | 586.16 ms | 476.33 ms | 1.1999倍 | 9/10 |
| 世代付きキャッシュあり ifのみ (8組) | 591.49 ms | 477.48 ms | 1.2426倍 | 8/8 |

再現用生成器: tests/bench/algos/build_mega_ifelse_fixture.py (--two-writes, --no-else, --reverse)。
交互ベンチマーク: tests/bench/algos/bench_mega_branch_pair.py。
結果: build-scheduler-rewrite-20261111/mega-ifelse/bench-branch-pair-cache-40000x8/results.json
ifのみ結果: build-scheduler-rewrite-20261111/mega-ifelse/bench-branch-pair-if-only-40000x8/results.json

合成試験以外の性能を保証しない。デフォルトOFF。他エージェントのコードはreset、commit、pushせず保持する。

## ScratchBench 実プロジェクトでの影響

既存performance.sb3から未選択suite call siteだけを削除する
bench_scheduler_flags.pyを使い、同じバイナリ、CPU16固定、eager、
他のMegaフラグ固定の条件でbranch pairフラグだけON/OFFにし、
8組の交互測定を行った。スループットは大きいほどよい。

| 選択したカーネル | OFF中央値 | ON中央値 | 対応ペアON/OFF | ON勝利 |
|---|---:|---:|---:|---:|
| sched_fanwait単独 | 515.73 | 472.19 | 0.9646倍 | 3/8 |
| sched_pingpong単独 | 172.87 | 179.33 | 1.0589倍 | 6/8 |

元データ:
- build-scheduler-rewrite-20261111/mega-ifelse/scratchbench-fanwait-branch-pair-isolated/results.json
- build-scheduler-rewrite-20261111/mega-ifelse/scratchbench-pingpong-branch-pair-isolated/results.json

両方を一緒に計測した場合、さらに大きい差とばらつきがあり、
試験の負荷に依存する結果だった。単独8ペアでも結果は一致せず、
通常のsched_*に対する安定した改善は確認できていない。
合成分岐ベンチの1.20～1.24倍という改善を、ScratchBench全体へ
外挿しない。誤差・バックグラウンド負荷が高い環境での結果であり、
現時点では実験機能の既定OFFを維持する。
