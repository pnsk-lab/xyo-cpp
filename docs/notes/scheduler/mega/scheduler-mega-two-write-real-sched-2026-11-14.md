# Mega Scheduler β — 実ScratchBench計測・不要な融合判定の除去・2更新Burst

2026-11-14 / `xyo-cpp` / opt-in機能 / 未コミット

## 概要

前段で作成した `XYO_JIT_MEGA_MULTI_REPEAT_BURST=1` を、
実際の ScratchBench の8つの `sched_*` 項目に対して同一バイナリの
ON/OFF交互試験で検証した。合成入力では2倍を超えることがあったものの、
元の `sched_*` では**一括実行0回**であり、実際の性能改善には使われない。

より重大なのは、未対応プロジェクトで毎パスの適格性チェックが
空振りしてコストになることであった。対応するコンパイル済み融合エントリの存在を
安価に確認し、無い場合はスキャナを呼ばない入口ガードを追加した。
これにより `sched_restart` の実測回帰を解消した。

さらに、2〜8本の純粋なrepeatの各ターンに
`change variable by literal` が**最大2個**直列に存在する場合も
一括実行できるようにした。変数の別名参照を含め、
Scratchパス内の書き込み順序を変えない。

## ScratchBenchの計測

計測スクリプト：
`tests/bench/algos/bench_mega_burst_real_sched.py`

元の `scratch-bench/.cache/build/performance.sb3` から、
非scheduler suiteへの**起動呼び出しのみ**を除いた一時SB3で、
8個のschedulerカーネル本体は一切変更していない。
ScratchBenchの時間依存チェックサムはON/OFFで一致要求しない。

元のビルド成果物が別プロセスにより消失したため、後続の実測では
初回実測時に生成・保存した**同一バイト列**の
`real-sched-multiburst-20261114/sched-8.sb3` を再利用した。
この一時SB3のSHA-256は以下で固定：
`bb5502584c8d96df9d2f2de4594d570fbdc47b39fc9038ef02dcf2f0d3588a23`。

CPU16固定、Eager JIT、30fps、同一の凍結Releaseバイナリを
OFF/ON順を入れ替えて交互実行。`XYO_PAR=0`、通常のScratch work-time予算を使用。
起動・JITコンパイルは各run内で行われる。
唯一の差は `XYO_JIT_MEGA_MULTI_REPEAT_BURST` のON/OFF。
それ以外の前提となるMega関連フラグは双方ON。

### 原因の切り分け

| 改善段階 | 試行組数 | sched_restart OFF中央値 | ON中央値 | 対応ペア幾何平均速度比 | 実際のMulti Burst |
|---|---:|---:|---:|---:|---:|
| 初期 | 4組 | 721.35 | 532.12 | 0.7401倍 | 0 |
| コンパイル時sourceゲート追加 | 5組 | 726.23 | 520.86 | 0.7182倍 | 0 |
| **stepThreads入口のcompiled-entryゲート追加** | **6組** | **721.80** | **715.13** | **1.0038倍** | **0** |

単位：ScratchBench `ops/ms`（大きいほど高速）。

先頭Threadに `megaDispatch` がない、または
`megaDispatch->multiRepeatBurstEntry` がない場合、
`tryMegaMultiRepeatBurst()` を呼ぶ前にスキップ。
これは大半の通常Scratch処理の反復スキャンの固定費を削減する。
最初のコンパイル時ゲートだけでは回帰が残り、入口ゲートで解消した。

最終6組の他項目の対応ペア速度比：
- sched_looptick: 0.9978倍（ノイズ範囲）
- sched_pingpong: 0.9922倍
- sched_restart: 1.0038倍
- sched_clonechurn: 1.0239倍
- sched_threads: 1.0157倍
- sched_fanout: 1.0244倍
- sched_fanwait: 1.0629倍
- sched_waituntil: 1.0609倍

ON/OFFいずれも8項目で **Multi Burst実行0回**。
個々の測定値のばらつきにはCPU負荷等の影響もある。
上の小さなスコア差をMulti Burstの高速化とは主張しない。

生データ：
- `build-scheduler-rewrite-20261111/mega-multi-pc/real-sched-multiburst-20261114/results.json`
- `.../real-sched-multiburst-gated-20261114/results.json`
- `.../real-sched-multiburst-prefilter-20261114/results.json`

## 2更新源のコンパイル時証明

`src/jit/backend_llvm.cpp` の
`compileMultiRepeatBurst` により以下をコンパイル時に厳密証明：

- 全sourceが `Kind::Repeat`
- 2〜8個のsourceが存在
- 各sourceは単一変数または2個の順序付き変数更新のみ
- `hasConditional` / else系 / `returnSuffixes` がない
- 変数書き込みに当たってWait通知などの未対応副作用がない

証明されないsource群では**専用LLVM burst関数をそもそも生成しない**。
`MegaDispatchCode::multiRepeatSourceArity` に各sourceの
更新回数 `1` または `2` を保存する。
従来の `singleFastSourceKinds` は未変更であり、
単独Thread早期走査の適格性を広げていない。

実行時は、source IDに対応する変数ID数・target一致・structural
generation一致を確認してから `Variable*` を取り出す。
2変数の場合は双方の型・数値有限性を独立に検査する。

LLVMの`*_multi_repeat_burst` は既存 `*_multi_pc_batch`
の1パスを順に実行したあと、**第一変数および任意の第二変数**
のIEEE754有限性を検査。途中で無限大等になった場合も
現在のScratchパスを最後まで実行してからC++へ戻る。
共有変数への2回書き込みは構造上の別名を許し、演算順序を守る。

1回のnative呼び出しは最大256パスに制限。
`runSchedulerPass()` を直接呼ぶ契約は引き続き1Thread/1パス。
renderer、待機通知registry、イベント駆動、並列待ちqueue等の
機能では保守的な従来経路を選ぶ。

## 2更新入力のA/B性能

`tests/bench/algos/bench_mega_multi_repeat_burst.py --two-change`

100,000回ずつ反復する2つのgreen flagスクリプトを固定、
CPU16、Release/Eager JIT、`--fps 0`、起動・JITコンパイル込みの
同一バイナリ交互A/B、各12組。

| パターン | OFF中央値 | ON中央値 | 対応ペア幾何平均速度比 | ON勝利 |
|---|---:|---:|---:|---:|
| 4つの独立変数に2回ずつ順序付き書込み | 199.412 ms | 114.516 ms | **1.75394倍** | 12/12 |
| 同一変数への2回書込み×2Thread | 144.837 ms | 63.728 ms | **2.28967倍** | 12/12 |

両パターンでLLVM呼び出しは100,001→393回。
Threadの論理ターンは双方200,002で一致。
終了時のScratch Stage変数状態も一致。
両計測は同じバイナリSHA-256：
`8aed26203355afe6decf1166a93eaa5aa1dcbded711b36e491a022cb49071bdb`。

生データ：
- `build-scheduler-rewrite-20261111/mega-multi-pc/bench-two-write-independent-100k/results.json`
- `build-scheduler-rewrite-20261111/mega-multi-pc/bench-two-write-alias-100k/results.json`

## 回帰テスト

集中テストはBurst ONで **77/77 PASS**、OFFで **45/45 PASS**。

- 混合Scalar/Repeat sourceは専用関数を生成しない。
- 2更新sourceを `multiRepeatSourceArity={2,2}` と判定する。
- 255パスの既存native batchとの変数値・PC・残反復数ビット単位一致。
- 125パスの第一/第二変数への別名(alias)・順序保持。
- 4,096回×2本の現実Scratch runtime終了時のStage変数ビット単位一致。
- 大きな有限数の更新でも1パスのnative結果と一致。
- 第二変数における `1e308+1e308` オーバーフローは
  1パス終了後に正確に中止し、従来結果とビット一致。
- 単独Thread Burst / Multi Burst 1更新系の旧回帰を全保持。

全体のEager ON/OFF回帰ログ：
`build-scheduler-rewrite-20261111/mega-multi-pc/two-write-final-full-on.out` と
`two-write-final-full-off.out`。

## 状態

新しい最適化は既定OFF。複数Threadの静的純粋ループ以外へ
適用すると意味論を壊す恐れがあるため、自動的には広げない。
ScratchBench `sched_*` は今回の機能の直接対象外と確認した。
実際の `sched_*` の継続的高速化には、Fiberの中断・再開、
broadcast fanout、clone生成/廃棄、wait依存管理といった
別のスケジューラ経路の最適化が必要。

複数エージェント共有worktreeの他作業をreset/commit/pushしていない。

## 最終Releaseバイナリの全体回帰

同一最終ソースをビルドした `xyo-tests` を Eager JIT に固定し、
単独Burst/事前証明/fast-scanなど前提フラグは双方ON、
複数Thread BurstフラグだけON/OFFで独立実行した。

- 複数Thread Burst **ON**：exit=0、Scheduler **638/638 PASS**、E2E **99/99 PASS**、他のテスト群も失敗0。
- 複数Thread Burst **OFF**：exit=0、Scheduler **606/606 PASS**、E2E **99/99 PASS**、他のテスト群も失敗0。
- 集中テスト ON: **77/77 PASS**、OFF: **45/45 PASS**。
- Fiber smoke / GC遅延回収 / マルチFiber barrier：**3/3 PASS**。
- `git diff --check`、ベンチPython 2本の構文チェックPASS。

テスト増分はオプトインのsource形状・順序・値表現・非有限値のassertであり、
同一機能をOFFにした場合に実行されない分が32件。

最終の全体回帰ログ：
- `build-scheduler-rewrite-20261111/mega-multi-pc/two-write-final-full-on.out`
- `build-scheduler-rewrite-20261111/mega-multi-pc/two-write-final-full-off.out`

今回のコードは未コミット。実験機能は既定OFF。
