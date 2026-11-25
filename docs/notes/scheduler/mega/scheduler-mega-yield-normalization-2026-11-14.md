# Mega Scheduler β：STATUS_YIELD 正規化で単独senderの性能回復

2026-11-14 / 未コミット / 試験フラグ、既定OFF

## 原因

単独Mega senderの `repeat { broadcast and wait }` は、受信者が
存在すると通常Fiberより遅かった。`XYO_SCHED_TOTAL_STEPS=1`で
Runtime::step()のフレーム数を直接測ると、2万回の送信で：

| receiver | 通常Fiber | 正規化前Mega | 正規化後Mega |
| --- | ---: | ---: | ---: |
| repeat(1) | 2 | 20,001 | **2** |
| repeat(8) | 3 | 20,001 | **3** |

通常のjitStepThreadはSTATUS_YIELDから復帰したFiberを
STATUS_RUNNINGへ戻してからスケジューラpassのactiveを数える。
以前のMegaは待ち中に直接returnし、この正規化を飛ばしていた。
そのため各受信者を起動した1パスごとにnumActiveThreads=0となり、
次のRuntime::stepまで進めなくなった。

## 実装

- `src/engine/sequencer.cpp`:
  `XYO_JIT_MEGA_NORMALIZE_YIELD=1` を新設。正規化前のSTATUS_YIELDを
  STATUS_RUNNINGにして同じRuntime::stepフレームで続ける。
  既存のfast latchと通常Mega joinの両方で、待機中の受信者が
  RUNNINGであることを確認したときのみ正規化。
  STATUS_YIELD_TICKはそのまま維持し、次フレーム待ちを破壊しない。
- `src/engine/runtime.cpp`:
  `XYO_SCHED_TOTAL_STEPS=1`の一回限りの診断ログを追加。
  通常時はstepSerialを出力しない。
- `tests/unit/test_scheduler.cpp`:
  正規化が有効なら受信待ちの送信者はRUNNING、
  tick待ちの受信者ではYIELD_TICKのまま、という境界テストを追加。
- `tests/bench/algos/bench_mega_single_receiver.py`:
  正規化フラグ単独の比較と、通常Fiber vs 完成した
  singleton sender + fast latch + normalizationの直接A/Bを追加。
  すべてFrozenバイナリ、CPU16、XYO_PAR=0、eager、同一SB3、
  交互実行、Stage変数完全一致を確認。

## A/B 測定

10,000回送信、受信者repeat(3)、同じMegaバイナリ、
fast latch共通ON、正規化のみをON/OFF（16組）：

| 指標 | 正規化OFF | 正規化ON |
| --- | ---: | ---: |
| 中央値、起動から終了まで | 119.246ms | 77.650ms |

ペア比の幾何平均 **1.55028x**、16/16組でON勝利。
記録：
`build-scheduler-rewrite-20261111/mega-normalize-yield-20261114/bench-normalize-10k-r3-16/results.json`

Fiberと正規化Megaを直接比較（独立の16組交互ペア）：

| workload | Fiber中央値 | Mega中央値 | 幾何ペア速度比 | Mega勝利 |
| --- | ---: | ---: | ---: | ---: |
| 10k送信、repeat3 | 77.153ms | 77.322ms | 1.01659x | 8/16 |
| 20k送信、repeat1 | 89.804ms | 87.490ms | **1.02614x** | 13/16 |
| 20k送信、repeat8 | 126.003ms | 118.877ms | **1.06882x** | 16/16 |

それぞれ別の負荷、別のA/B。絶対時間の行間比較は禁止。
repeat3は8/16なので確実な改善とは断言しない。
repeat8では実測条件で約6.9%改善、16/16勝ち。

記録：
`build-scheduler-rewrite-20261111/mega-normalize-yield-20261114/bench-sender-latch-normalized-10k-r3-16/results.json`
`build-scheduler-rewrite-20261111/mega-normalize-yield-20261114/bench-vs-fiber-20k-r1-16/results.json`
`build-scheduler-rewrite-20261111/mega-normalize-yield-20261114/bench-vs-fiber-20k-r8-16/results.json`

## 最終全体回帰

同一Releaseバイナリ、JIT eager、Mega既存βフラグすべて共通ON、
single sender/receiver/fast latchも共通ON。
正規化のみ切替：

- 正規化ON（初回）：Scheduler **690/690 PASS**、E2E **99/99 PASS**、exit=0
- 正規化OFF（初回）：Scheduler **689/689 PASS**、E2E **99/99 PASS**、exit=0
- 実タイマー受信者の追加後、正規化ON：Scheduler **694/694 PASS**、
  E2E **99/99 PASS**、exit=0
- 実タイマー受信者の追加後、正規化OFF：Scheduler **693/693 PASS**、
  E2E **99/99 PASS**、exit=0

最終2実行は同一Releaseバイナリで、Fiber/GC smoke **3/3 PASS**。
改訂後の全体ログ：
`build-scheduler-rewrite-20261111/mega-normalize-yield-20261114-full-final-1.out`
`build-scheduler-rewrite-20261111/mega-normalize-yield-20261114-full-final-0.out`

ログ：
`build-scheduler-rewrite-20261111/mega-normalize-yield-20261114-full-1.out`
`build-scheduler-rewrite-20261111/mega-normalize-yield-20261114-full-0.out`

集中回帰は正規化OFFでsingleton sender16/16 PASS、
正規化ONで17/17 PASS。ONにはSTATUS_YIELD_TICKを
維持する負の対照が追加される。git diff --check、ビルドPASS。

## 制限と次の方向

1. 新機能は全て既定OFF。一般のScratchプロジェクトは変更なし。
2. Mega senderの静的ソース証明は固定ASCII broadcast、固定repeat、
   引数なしprocedureなど非常に限定的。実ScratchBenchの
   sched_pingpongはこの証明に適合せず、本改善の性能は未検証。
3. 今後は一般化された自作ブロック内の送信を継続PCへ移すが、
   その前にGC roots、ソース改訂、動的reporterと復帰順序が必要。
4. 送信側のネイティブJITステップ数の削減だけでなく、Scratchの
   同一フレーム内再開が最重要。不正なYIELD_TICK正規化は厳禁。

未コミット。他agentの変更へのreset/stash/commit/pushなし。


## 実ScratchBenchへの波及確認

元の`sched-8.sb3`から8本の`sched_*`スケジューラ計測部分のみを
抽出し、元のカーネルには手を加えず、
`sched_pingpong`、`sched_fanwait`、`sched_restart`を
同一バイナリ・CPU16・Eager・30fps・12組交互A/Bで比較した。

| case | OFF中央値 ops/ms | ON中央値 ops/ms | 対応ペア幾何比 | ON勝 |
| --- | ---: | ---: | ---: | ---: |
| sched_pingpong | 432.9805 | 443.2640 | 1.01885x | 7/12 |
| sched_fanwait | 813.2680 | 792.1765 | 0.97178x | 3/12 |
| sched_restart | 1344.4665 | 1327.6355 | 0.99797x | 3/12 |

結果の変動は小さくなく、このデータで実作品の改善を主張できない。
ソースコンパイルログを同条件で確認した：
- pingpong：Megaインストールなし
- fanwait：101 clone scalar receiverのMega batchのみ（待機するsenderではない）
- restart：単独受信側のMega repeat（待機senderではない）

**どのケースでも今回のMega sender join STATUS_YIELD正規化を実行しない。**
従って上記の値の変動は本新機能の直接的な高速化/悪化を示さない。
CPUノイズやその他のベンチ環境差を排除した上での主張はしない。

記録：
`build-scheduler-rewrite-20261111/mega-normalize-yield-20261114/bench-real-sched-12/results.json`
`build-scheduler-rewrite-20261111/mega-normalize-yield-20261114/trace-sched_pingpong.err`
`build-scheduler-rewrite-20261111/mega-normalize-yield-20261114/trace-sched_fanwait.err`
`build-scheduler-rewrite-20261111/mega-normalize-yield-20261114/trace-sched_restart.err`

## 時刻進行を要する実受信者の追加回帰

人工的なSTATUS_YIELD_TICKだけでなく、
受信ハットが`control_wait(0.005s)`→変数+1を実行する
正規のScratchプロジェクトを実行した。
送信側は repeat(3) { broadcast and wait } → 変数+10。
毎回5ms待機し、送信者のsuffixを先行実行しないことを確認した。
実行完了時の合計は13、receiverは全て退場、Fiber数は開始時と同数。

このテストとreceiver=0 / repeat(0)を含む
singleton sender集中回帰は、fast latch ONで
正規化OFF 20/20、ON 21/21 PASS。
fast latch OFFの通常Mega経路でも同じく
正規化OFF 20/20、ON 21/21 PASS。

ログ：
`build-scheduler-rewrite-20261111/mega-normalize-yield-20261114-timed-0.out`
`build-scheduler-rewrite-20261111/mega-normalize-yield-20261114-timed-1.out`
`build-scheduler-rewrite-20261111/mega-normalize-yield-20261114-generic-0.out`
`build-scheduler-rewrite-20261111/mega-normalize-yield-20261114-generic-1.out`
