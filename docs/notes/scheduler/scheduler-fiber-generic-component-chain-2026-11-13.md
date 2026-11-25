# Mega Scheduler / Fiber chain — 一般の単一衝突グループ

日付: 2026-11-13。共有作業ツリーで未コミット。対象は `xyo-cpp`。

## 対象

従来の `Sequencer::flushParBatch` は、全Threadが同じ書込キーに衝突する場合だけ、すでに開始された複数のFiberを `Fiber::resumeChain` で直接つないでいた。
例えばA→B→C→Aのように、グラフは連結だが全員に共通するwriterがない場合は、グラフ構築後 `groups.size() <= 1` となり、各Fiberが毎回C++スケジューラへ戻っていた。

## 実装

- `src/engine/sequencer.cpp` に `XYO_FIBER_CHAIN_COMPONENT=1`（既定OFF）を追加。
- 既存の共通writer経路の `resumeChain` の適格性検査と終了後の後片付けを `tryResumeExistingChain` に共通化。
- 汎用の単一衝突グループでは、オプトイン時だけ同じ検査に合格した既開始Fiberをまとめて再開する。新規Fiberを連鎖の途中から開始することはない。
- `parallelEligible`の事前検査（LLVM access workerSafe、source revision、main-thread/IOバリア等）、全Threadのidentity/active/status、compiled/JitCtx/Fiber状態を検証し、不適格なら従来の `jitStepThread` で逐次実行。
- `XYO_FIBER_CHAIN=0`、 `XYO_PROF` 有効、ASan（Fiber側）では既存制約を保持。生成済みSSA rootやOSR等の不安全な並列経路は `parallelEnabled` 側で拒否する。
- 各Threadについて `sampledWarpClockUs` 等の実行時刻サンプルを供給し、1パスで各Fiberを1回だけ再開。順序・Finished/YIELDステータス処理・Fiber/JitCtx返却は元のwriter専用経路と同一。

## 正しさ

`tests/unit/test_scheduler.cpp` に3本の競合スクリプトを追加。Stage変数 x/y/z をそれぞれ変更し、read依存を一周させる (A reads z, writes x / B reads x, writes y / C reads y, writes z)。7/9/11回のrepeatが異なる終了タイミングを作り、Scratchターゲットの実行順序に基づく逐次参照値と一致することを検証。
汎用単一成分が確実に `chainedPasses > 0` となることも検証。
集中テストOFF=3/3 PASS、ON=4/4 PASS。単にフラグを有効にしただけでなく、連鎖の実行まで確認した。

## マイクロ／合成SB3ベンチマーク

Stage x/y/zからなる3本のスクリプト（各40,000回、最後に値をlistへ保存）。
読出しと加算が循環依存になり、単一成分だが共通writerがない。
JIT=eager、CPU16固定、元のReleaseバイナリ、20組ON/OFF交互測定。
最終リスト出力 `22857` は全40回の試行で完全一致。

| 指標 | OFF | ON |
|---|---:|---:|
| 中央値 | 135.626ms | 131.712ms |
| 対応ペア速度比 | — | **1.0291倍** |
| 改善組数 | — | **17/20** |

ベンチスクリプト:
`build-scheduler-rewrite-20261111/fiber-20261113/bench-generic-chain-20pairs.py`。
元データ: `build-scheduler-rewrite-20261111/fiber-20261113/bench-generic-chain-20pairs.json`。
SB3ジェネレータ: `build-scheduler-rewrite-20261111/fiber-20261113/bench-generic-chain.py`。

## 注意

- 新しい経路は **既定OFF**。安全性と効果を対象を広げた実テストで継続検証する。
- ほとんどのScratchBenchでは、全員共通writerで以前からchainになるか、複数の独立成分になるため、新経路の改善対象とは異なる。
- 既存のソース変更・他エージェントの編集は上書きせず、新機能の差分だけ追加。

## ScratchBench実負荷（全8項目）

元のperformance.sb3からscheduler suiteのみを選択し、各カーネルの
Scratchブロック本体は変更しない。CPU16固定・Eager・同一バイナリ・
12組OFF/ON交互測定。スコアは大きいほど良い。

| カーネル | 対応ペア速度比ON/OFF | ON勝利 |
|---|---:|---:|
| sched_looptick | 0.99624倍 | 5/12 |
| sched_pingpong | 0.98912倍 | 4/12 |
| sched_restart | 0.98730倍 | 2/12 |
| sched_clonechurn | 0.99089倍 | 5/12 |
| sched_threads | 1.00627倍 | 7/12 |
| sched_fanout | 1.00235倍 | 5/12 |
| sched_fanwait | 0.99947倍 | 6/12 |
| sched_waituntil | 0.98336倍 | 6/12 |

原データ:
build-scheduler-rewrite-20261111/fiber-20261113/bench-generic-chain-scratchbench/results.json

新しい経路自体の成立頻度を診断するため
`XYO_FIBER_CHAIN_COMPONENT_STATS=1` を追加。
genericComponentCandidates はグラフ構築後の単一グループ数、
genericComponentChained は全実行時検証を通過して実際に
resumeChain できた回数、genericComponentThreads はその総Thread数。

| 対象 | 候補 | chain成立 | chain Thread |
|---|---:|---:|---:|
| 循環依存SB3（3 scripts, 40,000 repeat） | 40,003 | 40,002 | 120,004 |
| ScratchBench scheduler suite全8項目 | 0 | 0 | 0 |

したがって実ScratchBenchでは今回の新しい分岐に一度も入っておらず、
0.98〜1.01倍の差は外部負荷・測定誤差等の範囲と考えるべき。
合成SB3では確実に新しい経路が使われ、約2.9%速かった。

## 最終回帰

- 既定OFFの集中テスト 3/3 PASS
- 実験ONの集中テスト 4/4 PASS、genericComponentChained > 0を
  直接検証し、既存のall-writer経路を誤って数えていない。
- 最初の全体回帰（Eager、実験ON）Scheduler 454/454 PASS、
  全テスト群失敗0、終了コード0。
- 計測機能は明示指定した場合のみ出力。
- その後追加した診断カウンタについてもビルドと集中テストはPASS。
- generic component経路は既定OFFを維持する。
