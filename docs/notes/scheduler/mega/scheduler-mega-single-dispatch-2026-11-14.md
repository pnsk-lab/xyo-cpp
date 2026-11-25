# Mega Scheduler β: 単独残余ThreadのLLVM直接ディスパッチ（2026-11-14）

状態：実験機能、既定OFF、未コミット。共有worktreeの既存変更は保持。

## 背景

既存の `tryMegaScalarBatch()` は同一 `MegaDispatchCode` に属する
連続した2～8本のsource-certified Threadを1回のLLVM関数で処理するが、
ほかのThreadが先に終了して1本だけになるとバッチを拒否する。
その結果、`jitStepThread()` による `JitCtx` 準備・継続処理へ戻る。

## 実装

- `src/engine/sequencer.cpp` の `tryMegaScalarBatch`:
  `XYO_JIT_MEGA_SINGLE_DISPATCH=1` の場合、適格件数が1件でも
  既存の `multiPcBatchEntry(items, 1)` または
  `scalarBatchEntry(values, 1)` を呼び出す。
  各Threadの更新はそのscheduler passにつき1回まで。
- 既存の `JitCtx` または `Fiber` を持つThread、OSR、待機中、
  source revision不一致、変数世代不一致、数値以外・非有限の値、
  reporter・renderer・profiling・`XYO_WAIT_WAKE_REGISTRY` 等を
  **従来どおり拒否**する。これらのガードは緩和していない。
- LLVMとC++のABI、Yield位置、PC/remaining、join identityは変更なし。
  実行権の数も変えず、個別ThreadのC++ディスパッチを省略する。
- `tests/unit/test_scheduler.cpp` に数値初期化された2つのrepeatを
  1つの共有LLVM PCへ融合し、2本から1本への減少を跨いで
  `jitCtx == nullptr`、Fibers不変、変数の厳密値、
  呼出し回数を検証するテストを追加。
- 別の既存mixed-sourceテストでは初期値が空文字列のため
  通常のScratch型変換ヘルパーを一度呼んで `JitCtx` が存在する。
  この既存継続状態を尊重し、単独高速経路に移動しないことも検査。
- 独立した集中実行用 `XYO_TEST_ONLY_MEGA_SINGLE=1` は
  従来の `XYO_TEST_ONLY_STACKLESS_CORO=1` と併用可能。

## フラグ

```sh
XYO_JIT_MODE=eager \
XYO_JIT_MEGA_BATCH_YIELD=1 \
XYO_JIT_MEGA_DIRECT_SCALAR=1 \
XYO_JIT_MEGA_SCALAR_BATCH=1 \
XYO_JIT_MEGA_MULTI_PC_BATCH=1 \
XYO_JIT_MEGA_SINGLE_DISPATCH=1 \
./build-scheduler-rewrite-20261111/xyo run project.sb3 --fps 0
```

ブロードキャスト受信者を融合する場合は
`XYO_JIT_MEGA_BCAST_RECEIVERS=1` を追加。

## 対応ペア測定

`tests/bench/algos/bench_mega_single_dispatch.py` の専用fixture。
2つのgreen flagスクリプト（反復120,000回と1回）を生成。
CPU16固定、Eager JIT O1、同一凍結バイナリ・同一ソース、
12組ON/OFF交互。測定はコンパイルと起動を含むプロセス壁時計。

| 指標 | OFF | ON |
|---|---:|---:|
| 中央値 | 108.063ms | 105.818ms |
| 対応ペア速度比 | — | **1.02417倍** |
| ON勝利 | — | **10/12組** |
| Mega bulk呼出し | 2 | 120,001 |
| Mega bulkで処理したThreadスロット | 4 | 120,003 |

注意：bulk呼出しの増加は1スレッド継続をJitCtx経路ではなく
LLVMで処理していることを示す。Scratchの実際のYield回数や
スケジューラpass回数を削減したという意味ではない。

測定生データ：
`build-scheduler-rewrite-20261111/mega-multi-pc/bench-single-survivor-120k/results.json`

対象を限定した合成試験で約2.4%の改善を観測。
一般のScratchBenchや実際のScratch作品の改善はまだ示していない。
標本数が少ないため既定OFFとする。

## 再現性

- 集中テスト19/19件PASS
- `git diff --check` PASS
- 追加の全体回帰のログ：
  `build-scheduler-rewrite-20261111/mega-multi-pc/single-full-on.out`、
  `single-full-off.out`（実行結果はログを確認すること）


## 最終回帰結果

同一Release xyo-testsバイナリのEager JITと全Mega関連フラグON。
新しい単独残余フラグのみ変更した結果:

- SINGLE_DISPATCH=1: **Scheduler 578/578 PASS**、その他すべてのスイートで失敗0、exit=0。
- SINGLE_DISPATCH=0: 初回はE2E初期段階でSIGSEGV (exit=139)が1回発生。
  **同一バイナリ・同じ環境変数**で再実行したところ、**Scheduler 570/570 PASS**、
  他の全スイート失敗0、exit=0。クラッシュの原因は未特定であり、この結果は
  メモリ安全性の証明ではない。
- 集中テスト: SINGLE_DISPATCH=1で **19/19 PASS**、OFFで **11/11 PASS**。
- 待機通知有効 `XYO_WAIT_WAKE_REGISTRY=1` + SINGLE_DISPATCH=1: `v0=120000, v1=2`、exit=0。
  この構成では一括実行は拒否され、従来の実行経路が保持される。
- Python syntax check と `git diff --check` PASS。

ログ: `single-full-on.out`、`single-full-off-retry.out`、`single-full-off.out`。

**判断:** 限定的な改善はあるが、既定OFFを維持する。
