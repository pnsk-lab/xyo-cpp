# Mega Scheduler β: 単独sender/receiverのThread-owned継続PC

2026-11-14 / xyo-cpp / experimental opt-in / 共有dirty worktree、未コミット

## 目的と実態

実ScratchBenchの sched_pingpong は、緑旗Threadが1本で
broadcast and wait の受信Threadも原則1本。
既存のMega Scheduler βの2～8本条件が障害だったため、
既存のソース証明とLLVMのswitch(pc)を変更せず単独にも対応。

重要な結果：
- 機能の正しさ・Fiberなしの継続・手続き2段ネスト・入れ子joinは検証できた。
- **通常のFiber経路より遅い。既定ONは採用しない。**
- 同じMega経路内部でC++側がrepeat latchを保持して
  LLVMとJitCtxの中間呼び出しを減らすと1.109倍改善したが、
  それでも通常Fiber経路より遅い。

## 新しい3フラグ（すべて既定OFF）

- XYO_JIT_MEGA_SINGLE_RECEIVER=1
  - XYO_JIT_MEGA_BATCH_YIELD=1 と XYO_JIT_MEGA_BCAST_RECEIVERS=1 が前提。
  - Runtime.startBroadcastAndWaitが返す**実際の**Thread* 1個のみが対象。
  - 事前にCompiledScript::mayYieldを検査し、
    同期受信者ではJIT fusion走査を呼ばない。
  - ソースの正確なSource::KindとreturnSuffixesを証明できるときだけ
    LLVMのswitch(pc)へ入れる。受信者のID/起動順はRuntimeが決定する。
- XYO_JIT_MEGA_SINGLE_SENDER=1
  - XYO_JIT_MEGA_BATCH_YIELD=1が前提。
  - 未実行の緑旗Thread1本でも、ソース証明済みの
    固定文字列broadcast-and-wait / 反復repeat / 引数なし自作ブロック
    に適用可能。受信者数自体は制限しない。
  - Thread::megaPc、megaReturnPcs/megaReturnDepth、
    megaRemaining、megaJoinRefsで中断・再開を表現。
- XYO_JIT_MEGA_FAST_BCAST_LATCH=1
  - 単独のSource::Kind::BroadcastRepeatで、
    returnSuffixesが空であることをコンパイル済みメタデータで証明する。
  - 非warp、固定反復数、固定ASCIIメッセージ、固定数値suffixだけを受理。
  - 中間回数の完了joinはThread::megaRemainingをC++で減算し、
    次のscheduler selectionまで待つ。LLVM呼び出し、JitCtx作成なし。
  - 最後の受信者が完了したときだけLLVMへ1回入って
    suffix（既存の変数Binding経路）を正確に1回実行する。
  - sender status、Thread-owned joinRefs、GC/retirement、source revision、
    callbackによるstop/restartへのガードは維持。

旗を設定しないと既存の挙動に変更なし。

## 実装箇所

- src/jit/jit.cpp: singleton sender/receiverのopt-in適格性と
  同期receiverの早期拒否。元の2～8組の処理は不変。
- src/jit/backend_llvm.cpp: singleton ORC生成の許可、
  static proofからfast-latch専用カウントを記録。
- src/jit/jit.hpp: MegaDispatchCode::broadcastRepeatFastCounts追加。
- src/engine/runtime.cpp: 同期scalar receiver singletonを
  installMegaBroadcastReceiversへ送らない。
- src/engine/sequencer.cpp: 純粋なbounded repeat(join)の
  中間ラッチと最終LLVM suffixのfast lane。
- tests/unit/test_scheduler.cpp: singleton sender/receiver、
  2段自作ブロック・nested broadcast、空受信者、0回repeat、
  2回目の緑旗・キャッシュ再利用・Fiber leak・join順序の回帰。
- tests/bench/algos/bench_mega_single_receiver.py:
  固定SB3と同一バイナリを用いた交互A/BとScratch変数全量比較。

## 実ScratchBench: singleton receiver単独

CPU16固定、Eager、30fps、同一Frozenバイナリ、
sched_pingpong/fanwait/restart 16組交互。
同期受信者をMega検査より前に拒否した最終候補:

| case | OFF median ops/ms | ON median ops/ms | paired ratio | ON wins |
| --- | ---: | ---: | ---: | ---: |
| sched_pingpong | 439.9715 | 429.7755 | 0.99742x | 8/16 |
| sched_fanwait | 759.6355 | 751.2190 | 1.00188x | 8/16 |
| sched_restart | 1283.6605 | 1272.8660 | 0.99034x | 8/16 |

生データ:
build-scheduler-rewrite-20261111/mega-single-receiver-20261114/bench-hoisted-16/results.json

初期に mayYield 判定を実装しなかった場合は
pingpong 0.839x、0/16勝という深刻な遅延。
元の受信hat `sc_ping` は mayYield=0 / procedures=0。
ここをMegaの関数に変換する利点はなく、
同期receiverの事前除外によってほぼ元の速度に戻った。

## 合成単独receiver

原プロジェクトではない固定数値repeatのSynthetic fixture、
CPU16固定、同一バイナリ、プロセス総時間（起動・LLVMコンパイル込）：

| workload | OFF median ms | ON median ms | paired OFF/ON | ON wins |
| --- | ---: | ---: | ---: | ---: |
| 10k send, receiver repeat3 (12 pairs) | 79.927 | 82.183 | 0.94857x | 3/12 |
| 100k send, receiver repeat3 (8 pairs) | 332.271 | 335.861 | 0.97984x | 3/8 |

src:
build-scheduler-rewrite-20261111/mega-single-receiver-20261114/bench-single-yield-synth-12/results.json
build-scheduler-rewrite-20261111/mega-single-receiver-20261114/bench-single-yield-synth-100k-8/results.json

## 合成単独sender: Fast Latchの効果と限界

送信repeat(10k){broadcast and wait PING}、
受信repeat(3){数値更新}、CPU16固定、Eager、同一バイナリ。
いずれもScratch Stageの最終値を厳密照合。

| 比較 | OFF median ms | ON median ms | paired OFF/ON | ON wins |
| --- | ---: | ---: | ---: | ---: |
| 通常Fiber vs Mega sender (12組) | 88.582 | 146.782 | 0.61423x | 0/12 |
| Mega sender (latch OFF vs ON,12組) | 150.862 | 141.830 | **1.10936x** | **12/12** |
| **通常Fiber vs Mega sender+latch (16組)** | **97.857** | **152.326** | **0.63983x** | **0/16** |

絶対値は試験間で揺れるため、各試験内のペア比のみ比較。
追加コンパイル費用とC++側の1回/反復のhost join管理が残り、
単独sender Megaの現状は生産採用に値しない。

src:
build-scheduler-rewrite-20261111/mega-single-sender-20261114/bench-synth-10k-12/results.json
build-scheduler-rewrite-20261111/mega-fast-bcast-latch-20261114/bench-synth-10k-12/results.json
build-scheduler-rewrite-20261111/mega-fast-bcast-latch-20261114/bench-vs-fiber-10k-16/results.json

## 未解決課題

実ScratchBenchの送信元は自作ブロック内の複数の
条件・動的値・副作用を持ち、現在のstatic proofは不適格。
今回の性能測定は既存の全schedカーネル最適化を意味しない。

今後の本命は：
- ネイティブ関数ごとのfiberとscheduler dispatchを統合する
  source-certified continuation ABI。
- 少数の既知のjoin receiverの完了を通知で伝え、
  ホストが毎ターン受信Thread*を照合する費用を減らす。
- 従来のコンパイル済みscriptを重複してORCへ渡すことを避ける。
- senderの任意procを一般化する際にはScratchの実行順序、
  GC/SSA値・変数Binding・source revision・停止/再起動を保証する。

決して単純な同期受信者をMega化して全体を既定ONにしない。


## 最終の適格性修正（重要）

ソース証明が可能な1本の緑旗スクリプトに対する
XYO_JIT_MEGA_SINGLE_SENDER は **BroadcastWaitまたはBroadcastRepeat**
だけを受理するようにした。
初回の全体ON回帰では、数値だけのrepeatを含む別Targetの単独
緑旗ThreadまでMega化し、従来のFiber競合連鎖を期待する
`shared-writer direct fiber chain exercised` テストが失敗した
（Scheduler 689 checks / 1 failure; E2E 99/99）。
Scratch出力だけを検証するのでは不十分だった。

`JitEngine::installMegaDispatch` の singleton installに、
source-certified Source::kindを使った事前確認を追加。
数値repeatは旧LLVM/Fiber経路に留める。
変更後の全体ON回帰ではFiber連鎖が実際に使われ、PASS。
**試験的な1本sender対応を「ソース検証済みbroadcast送信」に限定**した。

## 最終回帰検証

| 構成 | Scheduler | E2E | Exit |
| --- | --- | --- | --- |
| singleton receiver/sender/fast-latch 全ON | **689/689 PASS** | **99/99 PASS** | **0** |
| singleton新フラグ全OFF（同一最終バイナリ） | **659/659 PASS** | **99/99 PASS** | **0** |

全ONと全OFFを同じ最終Releaseバイナリで実行し、Eager JIT /
Megaバッチ既存フラグ（single dispatch、scalar、multi-PC、burst）は
共通ONにして確認。
ログ：
`build-scheduler-rewrite-20261111/mega-fast-bcast-latch-20261114-full-restricted-on.out`
`build-scheduler-rewrite-20261111/mega-fast-bcast-latch-20261114-full-restricted-off.out`

最終バイナリの集中回帰：
- singleton sender: fast latch=0 と =1 の両方で **16/16 PASS**
  （単独送信、repeat(3)、キャッシュ、Fiberなし、受信者0、repeat(0)）
- singleton receiver: **22/22 PASS**
  （2段自作ブロック、nested broadcast、return PC、join、内部同期受信者の旧JIT保持）
- Fiber/GC smoke **3/3 PASS**
- `git diff --check` PASS
- Python bench syntax PASS

今回の実験的なコードは全て既定OFF。
共有作業ツリーの既存未コミット変更には触れず、
commit / push / reset / stash は実行していない。
