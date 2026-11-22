# ScratchBench sched_* の固定費検証：join候補の棄却とcloneハット高速化の既定化

2026-11-14 / xyo-cpp / 共有dirty worktree（未コミット）

## このラウンドで変更したこと

実際のScratchBenchで繰り返し見られるbroadcast-and-waitのjoinチェックと
receiverハンドル確保を2件実験したが、どちらも16組/12組の再現性ある改善を
示さなかった。そのため両案を**ソースから安全に取り除き**、実行時の
Scratchジョイン順序・受信者Thread identity・Fiber所有権は旧経路のまま。

既に opt-in 実装済みだった新規クローンのハット起動直接経路を、
現行コードのReleaseバイナリで16組交互測定したところ、
`sched_clonechurn`で安定した改善があった。そのため
`src/model/target.cpp` の `XYO_CLONE_NEW_HATS_FAST` を既定ONに変更した。
`=0`で旧汎用 `Runtime::startHats` 経路に戻せる。

生成コールバック `onTargetCreated` があると従来経路へ戻るチェックは不変。
GUI/SDL/ホストがクローン生成の途中でThreadを起動する可能性がある場合は、
未検証の高速経路へ入らない。
`Runtime::startFreshCloneHats` 自体は以前から存在し、今回内部のロジックは変更していない。

## 同一バイナリの交互A/B — クローン起動

元のScratchBench performanceから他のschedulerケースへの起動呼び出し
のみを取り除いたSB3。**選択したカーネル本体は変更なし**。
CPU16固定、Eager JIT、30fps、`XYO_PAR=0`、
`XYO_YIELD_QMODE=off`、同一凍結バイナリをOFF/ON交互に16組。
指標はScratchBench ops/ms（大きいほど良い）。

| workload | OFF中央値 | ON中央値 | 対応ペア幾何平均 ON/OFF | ON勝利 |
| --- | ---: | ---: | ---: | ---: |
| **sched_clonechurn** | **410.3135** | **420.2735** | **1.02926倍** | **14/16** |
| sched_pingpong（対照） | 428.7625 | 425.9650 | 0.99428倍 | 6/16 |
| sched_fanout（対照） | 771.0340 | 762.4555 | 0.99865倍 | 7/16 |

3項目すべての値と実行順序、バイナリ/SB3のSHA256、各試行のstdout/stderrは
`build-scheduler-rewrite-20261111/clone-hat-reverify-20261114/bench-paired-16/results.json`。

前段の別ビルド・別時点の6組ではclonechurn 1.0928倍、6/6勝という
測定結果があったが、**今回の再測定と数値を平均・合成していない**。
現時点で最も適切な主張は「clonechurn専用で約+2.9%、
16組中14組改善」。他のScratchBench項目や47個全体への改善を示さない。

## 棄却した2件のブロードキャストjoin最適化

### 1. 多数のjoin先の一括所属判定

終了済みの受信Threadが多いとき、古いruntimeIndexヒントで
`containsThread`がactive Thread配列を繰り返し走査する恐れがある。
16件以上のjoinで4件以上の古いヒントがあれば
hash setによる1回のlive-array sweepへ切り替える方法を試した。

12組交互測定：
- sched_pingpong：OFF 416.211、ON 411.639 ops/ms、
  対応ペア 0.98381倍、5/12勝。
- sched_fanwait：OFF 804.754、ON 789.872 ops/ms、
  対応ペア 0.98622倍、6/12勝。
- sched_restart（対照）：対応ペア1.00149倍、6/12勝。

生データ:
`build-scheduler-rewrite-20261111/bcast-join-sweep-20261114/bench-paired-12/results.json`。
加入判定の全生存・一部退職・並べ替え・公開配列再挿入の8件テストは
ON/OFFとも合格したが、実測が悪化したため実装・新規Runtime API・テストを
すべて撤回した。フラグは残っていない。

### 2. broadcast-and-wait のreceiver vector容量再利用

1本のFiberが繰り返しbroadcast-and-waitする際の
`std::vector<Thread*>`の反復mallocを削減する目的で、
通常JIT送信Threadの`megaJoinRefs`ストレージを借りる実験を行った。
ネスト時は従来のFiberローカルvectorに戻す実装と、テスト（16件）は成功。

16組交互測定：
- sched_pingpong：OFF 431.571、ON 423.057 ops/ms、
  対応ペア 0.99037倍、5/16勝。
- sched_fanwait：OFF 807.749、ON 804.766 ops/ms、
  対応ペア 0.98745倍、7/16勝。
- sched_restart（対照）：対応ペア0.99884倍、8/16勝。

生データ:
`build-scheduler-rewrite-20261111/bcast-join-reuse-20261114/bench-paired-16/results.json`。

明確な改善がなく、Thread退役時には`megaJoinRefs`がクリアされるため、
非ローカルstorageの共有はsender中断・停止経路の安全性にも余計な制約を
生む。従来のFiber-local vectorへ完全に戻し、専用APIやテストも撤回した。

## ベンチハーネス

`tests/bench/algos/bench_scheduler_flags.py` にのみ、
`--prepared-scheduler` を追加した。
ScratchBenchの共有ビルドキャッシュが他のエージェントによって消されても、
前回凍結したscheduler-only SB3をバイト単位で再利用する。
フラグOFF/ONの交互実行・実行ファイル固定・ログ・SHA256はそのまま。

再測定例：

```sh
python3 tests/bench/algos/bench_scheduler_flags.py \
  --binary build-scheduler-rewrite-20261111/xyo \
  --project build-scheduler-rewrite-20261111/mega-multi-pc/real-sched-multiburst-20261114/sched-8.sb3 \
  --prepared-scheduler \
  --flag XYO_CLONE_NEW_HATS_FAST \
  --common-flag XYO_PAR=0 \
  --common-flag XYO_YIELD_QMODE=off \
  --case sched_clonechurn --case sched_fanout --case sched_pingpong \
  --jit eager --runs 16 --cpu 16 \
  --out build-scheduler-rewrite-20261111/clone-hat-new-check
```

## 回帰

既存の`testFreshCloneHatLaunchOrder()`は2本のclone-startハットを
同じTargetで一度ずつ起動し、作成時コールバックがある場合に
旧経路へ戻ることを検査する。既定ONでも、`=0`でも両ケースを維持。

新規join最適化は不採用のため、元のjoin処理、再開順序、保存先は変更しない。

## 次の優先順位

`sched_pingpong` と `sched_fanwait` の主要コストは
単純なjoin集合の走査やvector確保にはなかった。
既存のネイティブプロファイラでは、
Fiber再開とLLVM内部の処理を含む区間の平均が数百ns級だった。
次はJITが`broadcast and wait`で発生するFiberへの再入回数そのものを
減らすには、正しいScratch yield・join境界の証明が必要。
拙速なネストjoin融合やThread再利用は避ける。

## 最終回帰結果（既定ON vs 明示OFF）

同じ最終Releaseバイナリで Eager JIT、Mega Scheduler βの既存フラグを
両条件で等しくONにし、**新しい既定ON**と
`XYO_CLONE_NEW_HATS_FAST=0`をそれぞれ全量実行。

| 条件 | Scheduler | E2E | プロセス終了 |
| --- | --- | --- | --- |
| 既定ON | **638/638 PASS** | **99/99 PASS** | **0** |
| 明示OFF | **638/638 PASS** | **99/99 PASS** | **0** |

全体テストの他グループでも失敗0件。
ログは `build-scheduler-rewrite-20261111/clone-hat-reverify-20261114-full-default.{out,err}` と
`build-scheduler-rewrite-20261111/clone-hat-reverify-20261114-full-off.{out,err}`。

Fiber/GC smoke 3件は **3/3 PASS**。
`git diff --check`、Python計測ハーネスの構文チェックPASS。

ローカルの共有未コミット差分は維持し、commit/push/resetは行っていない。
