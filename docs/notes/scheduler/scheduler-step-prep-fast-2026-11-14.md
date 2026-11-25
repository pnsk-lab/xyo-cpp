# xyo-cpp：Runtime::step の無割り当て圧縮とthreadMapの検証付き再利用

2026-11-14 / Release Eager JIT / 共有未コミットworktree

## 目的

ScratchBenchの`sched_pingpong`・`sched_fanwait`では、
`broadcast and wait`イベントと受信Threadの新規生成・終了が頻発する。
Fiberの実行回数を減らすとScratchのイベント時刻・順序が変わるため、
まず不変なScheduler準備処理の割当・ハッシュ操作だけを対象にした。

## 棄却した実装（現在はソースから撤回）

`src/engine/runtime.cpp::Runtime::step()` に独立した既定OFFの機能を2件追加。

- `XYO_STEP_INPLACE_PRUNE=1`：
  旧方式は毎step新しい`std::vector<Thread*>`を確保し、killされた
  Threadを除く全生存ポインタを移していた。
  新方式はstable-index書込みと`resize`だけで現在の`threads`配列を
  その場圧縮する。生存者の順序・`runtimeIndex`更新・旧Threadの
  `ownedThreads`保存とjoined Thread*の寿命は不変。
  killされたThreadが0本でも、配列全体の再確保は発生しない。
- `XYO_STEP_STABLE_THREADMAP=1`：
  旧方式は毎step`threadMap.clear()`を呼んで全キーを再挿入する。
  新方式はeligibleな各Threadの`getId()`が実際にマップから
  **同じThread*へ解決される**ことと、
  eligible数がmapのエントリ数と一致することを検証する。
  完全一致なら再構築をスキップする。
  公開`threads`への外部書き込み・不一致・map破損・
  同じscript IDの複数Thread（重複キー）は従来の再構築に戻す。
  旧来の「後方のThreadをマップへ登録する」規則を維持する。
  変化したマップを削除したり、Thread*の寿命を延長する変更はない。

いずれも`Runtime::updateThreadMap`の外部APIは変えず、
`Sequencer::stepThreads`での本体実行・コンパイル状態・
pause/resume/GCは一切変更していない。

## 計測1：配列内圧縮（16組交互）

元のScratchBenchのカーネル本体は無変更。
`tests/bench/algos/bench_scheduler_flags.py`で
CPU16固定、Eager、30fps、`XYO_PAR=0`、
`XYO_YIELD_QMODE=off`、同一バイナリ、交互A/B16組。
ON/OFFは`XYO_STEP_INPLACE_PRUNE`のみ。

| case | OFF median ops/ms | ON median ops/ms | paired geom ON/OFF | ON wins |
| --- | ---: | ---: | ---: | ---: |
| sched_pingpong | 471.6005 | 472.5395 | 1.00169x | 7/16 |
| **sched_fanwait** | **788.6755** | **797.0965** | **1.01574x** | **12/16** |
| sched_clonechurn | 442.7320 | 441.6150 | 0.99958x | 7/16 |
| sched_threads | 6473.9010 | 6464.2345 | 1.00124x | 8/16 |
| sched_restart | 1376.1730 | 1367.2665 | 0.99278x | 8/16 |

原データ：
`build-scheduler-rewrite-20261111/step-inplace-prune-20261114/bench-paired-16/results.json`。

## 計測2：マップ再構築省略の追加効果（16組交互）

同じEager・CPU16・30fpsで、`XYO_STEP_INPLACE_PRUNE=1`を
両側共通に固定し、`XYO_STEP_STABLE_THREADMAP`のみを交互に切替。

| case | OFF median ops/ms | ON median ops/ms | paired geom ON/OFF | ON wins |
| --- | ---: | ---: | ---: | ---: |
| sched_pingpong | 410.9320 | 414.7835 | 1.00532x | 9/16 |
| **sched_fanwait** | **748.7035** | **752.2945** | **1.01243x** | **13/16** |
| sched_clonechurn | 412.8870 | 412.7790 | 0.99809x | 8/16 |
| sched_threads | 6107.6100 | 6098.1885 | 0.99452x | 6/16 |
| sched_restart | 1284.0940 | 1291.1235 | 1.00623x | 10/16 |

原データ：
`build-scheduler-rewrite-20261111/step-stable-map-20261114/bench-paired-16/results.json`。

**2実験の倍率は合算不可**。交互試行は別の時間窓・基準であり、
総合改善は両フラグ同時切替の独立した交互比較で検証すべき。
数値はリリース実行ファイルの実測であって理論的計算量の比ではない。

## 撤回前に検証した正しさ

`tests/unit/test_scheduler.cpp::testStepInplacePrune()`：
- 中断中の3本（host reorderあり）からkillされた1本を除去。
- 生存者の正確な順序・statusを保つ。
- 全生存者のruntimeIndexと`containsThread`を照合。
- `threadMap`の旧kill IDを除去し、生存者の新IDを保持。
- killなしの連続ステップと、全Thread kill。
- `ownedThreads`に消滅済みの参照対象を残す。
- 同一IDの2本についてmapの「last wins」と公開配列入れ替えの再構築。
- 外部によるmap上書き、全消去の復元。

`XYO_TEST_ONLY_STACKLESS_CORO=1 XYO_TEST_ONLY_STEP_PRUNE=1`
で、2フラグすべてのON/OFF組合せ **12/12 PASS**（計4実行）。
Fiber/GC smoke、全体回帰は最終版で併せて検証する。

## 判断

FiberのResume回数やbroadcastの配送順序は変えていない。
`sched_fanwait`では一部改善傾向があるが、他のworkloadは
明確な改善に届かない。**両試験機能は既定OFFのまま**。
実行コストの主因は単なる一時vector確保/マップ再構築ではなさそうで、
将来の根本的な改善にはThread同一性・join・yieldを維持したうえで
Schedulerそのものの状態遷移を統合する必要がある。


## 計測3：2フラグ同時切替（20組交互・最終採否判定）

初回測定で改善したfanwaitが相乗して速くなるという仮説を、
同一バイナリ・同一ScratchBench SB3・CPU16固定・Eager・30fpsで
**OFF（両フラグ0）とON（両フラグ1）をまとめて**交互測定して検証した。

| case | OFF median ops/ms | ON median ops/ms | paired geom ON/OFF | ON wins |
| --- | ---: | ---: | ---: | ---: |
| sched_pingpong | 449.2630 | 448.7950 | 0.99813x | 11/20 |
| **sched_fanwait** | **808.8800** | **806.7455** | **0.99664x** | **7/20** |
| sched_clonechurn | 440.3725 | 441.5630 | 1.00262x | 11/20 |
| sched_threads | 6542.7980 | 6535.0125 | 0.99957x | 10/20 |
| sched_restart | 1372.7405 | 1368.7945 | 0.99487x | 9/20 |

記録：
`build-scheduler-rewrite-20261111/step-prep-combined-20261114/bench-paired-20/results.json`

**総合して、再現性のある改善は確認できない**。特にfanwaitの初回の
+1.57%と、同じprep条件でのmap再構築省略+1.24%を合成するのは
誤りで、最終的な直接比較ではむしろ0.3%程度低下した。

以上から2つのopt-in環境フラグ、`Runtime::step()`の試験経路、
専用ユニットテストは**コードから撤回済み**。
この文書の実装・テスト説明は撤回前の実験履歴であり、
現行ソースに機能が残っているという意味ではない。

撤回直前のEager JIT + Mega Scheduler各既存フラグを共通ONにした
全体回帰は、試験フラグ両方ONでも両方OFFでも
**Scheduler 663/663 PASS、E2E 99/99 PASS、exit=0**だった。
これは撤回前の測定なので、撤回後の最終ビルド回帰とは区別する。
ログ：`build-scheduler-rewrite-20261111/step-stable-map-20261114/full-both-{on,off}.out`。

## 次の診断へ

ScratchBenchのpingpong/fanwaitにおける本質的なコストが
stop配列コピーやmap再挿入にあるとは言い難い。
`src/jit/rt.cpp`のプロファイルビルド専用
`XYO_BCAST_JOIN_CENSUS=1`で、送信側Fiber yield
そのものの回数を調べる。
通常Release（`XYO_DISABLE_RT_PROF=ON`）に診断コードは存在しない。
