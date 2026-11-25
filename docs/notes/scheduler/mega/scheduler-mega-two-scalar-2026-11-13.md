# Mega Scheduler β — 2つの連続スカラー命令のLLVM化

日付: 2026-11-13
状態: 実験的・既定OFF・未コミット

## 変更

2つの data_changevariableby が連続するScratchソースだと静的に証明し、
両命令を同じスケジューラ実行権の中で順番に実行する。

- src/jit/mega_yield_source.hpp: 最多2命令の直列定数加算を証明する
  scalarChangeSequence を追加。3命令目、数値以外の命令、動的レポータ、
  分岐は拒否。既存のScalarと固定回数Repeatの本体にのみ適用
- src/jit/backend_llvm.cpp: sourceごとの2つの異なるVariable*へ
  load → fadd → store を個別に発行。同じVariable*への連続更新では
  最初のstore後に必ず再loadする。浮動小数点の結合はしない。
  汎用mega continuation内の2命令目も生成し、戻り先PCの
  binding indexは1増やす
- src/jit/jit.hpp: MultiPcItemへ2本目のVariable*を追加し、
  MultiPcBatchの各ソースへ2命令フラグを記録。
  単命令専用scalarBatchEntryは従来どおり維持
- src/engine/sequencer.cpp: 両変数が存在し、scalar型で値が有限な
  IEEE754 numberであることを確認。1本でも不適格なら
  従来JITへフォールバック。1つのVariable*を2回使う場合も対応
- src/engine/thread.hpp: LLVMのThread所有binding配列を、
  最大4階層の戻りsuffix + 本体2命令 = 6スロットに拡張。
  backendにもコンパイル時の容量アサートを追加
- src/jit/jit.cpp: green-flag/broadcast受信者の初期ソース検証で
  2本目のVariable IDを確認
- tests/unit/test_scheduler.cpp: s と t への2種類の加算、
  s の別個の2回更新、反復回数0/1/2/3の混在、
  ORCコード再利用、文字列への変更後のフォールバックを検証。
  元の「非対応2命令」回帰は「非対応3命令」へ更新した。
  総和を変えず、古い非対応分岐の意味を保持

## 安全性と制限

対象は静的な定数加算（1本または2本）の直列命令列だけ。
外部コールバック・GC操作・スレッド生成・分岐・汎用procedure body
については新しいバッチへ載せない。
従来と同じsource revision、Thread/Target、変数型、数値、
待機レジストリ・renderer・profiling・OSRなどのガードを維持。
1 Threadにつき一巡回で最大1回のRepeat本体のみ実行する。
第二変数が文字列・真偽値・NaN等の場合はヘルパーの元経路に戻す。
変数のgeneration変化時も持続的なVariable*を失効させる。

有効化（従来と同じフラグのみ）:

    XYO_JIT_MODE=eager
    XYO_JIT_MEGA_BATCH_YIELD=1
    XYO_JIT_MEGA_BCAST_RECEIVERS=1
    XYO_JIT_MEGA_DIRECT_SCALAR=1
    XYO_JIT_MEGA_SCALAR_BATCH=1
    XYO_JIT_MEGA_MULTI_PC_BATCH=1

## テスト

ソース変更と既存テストの適用条件更新後、
xyo-tests: scheduler 536 checks、0 failures。
他のテストスイートもPASS。

bindingスロットを6本に拡張した最終ビルドでも以下を確認：

- 多重PC一括ON: xyo-tests Scheduler **536/536 PASS**、終了コード0
- 多重PC一括OFF（従来Mega）: **514/514 PASS**、終了コード0
- 変数待機通知レジストリON: **509/509 PASS**、終了コード0
- ninjaによるxyo-tests/xyo-cliビルド成功
- git diff --check成功

ログ：build-scheduler-rewrite-20261111/mega-two-scalar/post-capacity-{on,off,wake}.out


## 実測

同じCLIバイナリ、同じSB3、CPU16固定、6組交互測定。
8本のgreen flag反復スクリプトがそれぞれ40,000回、
2つの別個の数値変数を各回に1回ずつ更新する。
すべての16変数の結果を厳密比較。壁時計時間（ms）。

| 条件 | 中央値 |
|---|---:|
| 従来Mega（複数PC一括OFF） | 433.4045 |
| 2命令LLVM一括ON | 384.7077 |

対応ペアの幾何平均速度比は1.1224倍、ONが6/6組で上回った。
ONでは40,001 bulk呼出し、320,008 Threadスロット処理。
これは640,000回の数値更新を1件ずつC++ dispatcherへ戻さず処理した試験。

再現用生成器:
tests/bench/algos/build_mega_two_scalar_fixture.py

交互測定器:
tests/bench/algos/bench_mega_two_scalar.py

実測結果:
build-scheduler-rewrite-20261111/mega-two-scalar/bench-40000x8/results.json

既存ScratchBench sched_*全般の改善はこの限定的な試験からは推定しない。
複数命令＋分岐の一般化は次段階で別のソース証明が必要。

共有作業ツリーで他エージェントの変更はリセット・コミットしていない。
