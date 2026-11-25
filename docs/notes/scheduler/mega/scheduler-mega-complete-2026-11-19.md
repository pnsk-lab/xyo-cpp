# Mega Scheduler 一括プリセットと全機能検証（2026-11-19）

## 概要

Mega Scheduler は「複数の Scratch スクリプト / Thread を 1 つの LLVM
関数へまとめる」実験群で、`docs/notes/scheduler/mega/*.md` に 39 本の記録が
ある。機能ごとに `XYO_JIT_MEGA_*` が分かれており、これまでは 30 個近い
環境変数を手で組み合わせないと動かせなかった。

今回、**1 スイッチのプリセット** `XYO_JIT_MEGA=1`（CLI: `--mega`）を追加し、
互換性のある全機能をまとめて有効化したうえで、単体テスト・fixture パリティ・
機能別 A/B ハーネスの 3 層で「全部同時に入れても正しい」ことを検証した。

```sh
XYO_JIT_MEGA=1 ./xyo run project.sb3
./xyo run project.sb3 --mega
```

## 既定 ON（`run` / `test`）

`xyo run` / `xyo test` は、JIT モードを明示していない場合
（`XYO_JIT_MODE` / `XYO_JIT_DISABLE` / `--jit` / `--block-count` の
いずれも無い場合）に、Mega プリセットを自動適用する。Mega は Eager 専用
なので、このとき Eager も自動選択される。これが「最強」の既定プロファイルである。

- 無効化: `--no-mega` または `XYO_JIT_MEGA=0`（`--no-mega` は
  環境変数 `XYO_JIT_MEGA=1` より優先する）
- 明示モード: `--jit=hotspot` や `XYO_JIT_MODE=hotspot` は従来経路
  （A/B ハーネスや計測が意図せず Mega 化されないための境界）
- `play` / `gui` / `edit` は renderer 経路なので対象外（HotSpot のまま）
- `--block-count` は正確なカウントのため対象外

実測（3D2, 90 frames, headless, 2 回）: Mega 既定 1327–1364ms /
`--no-mega` 1259–1300ms。Eager 化の起動コストは約 5%。


## プリセットの契約

- `XYO_JIT_MEGA=1` / `--mega` は、**未設定の** Mega 機能フラグだけを `1` にする。
  明示した `XYO_JIT_MEGA_<NAME>=0` は常に優先する（個別に切り分け可能）。
- `XYO_JIT_MEGA=0` でプリセットは無効。
- Mega の lowering は **Eager LLVM のみ**なので、`XYO_JIT_MODE` も
  `XYO_JIT_DISABLE` も未設定なら `eager` を選ぶ。明示したモードは上書きしない。
- 診断フラグ（`*_TRACE`, `*_BULK_STATS`, `*_JOIN_PROFILE` など）は
  プリセットに含めない（挙動ではなくログだけを変えるため）。
- 実装: `src/engine/mega_preset.{hpp,cpp}`。`main()` と
  `Sequencer` コンストラクタで適用する（テストや埋め込みが `Runtime` を
  直接作る経路でも、フラグの関数ローカルキャッシュより前に反映させる）。
  静的初期化では適用しない。`--no-mega` が継承した `XYO_JIT_MEGA=1` を
  取り消せるようにするためである。

### 有効になるフラグ

| 分類 | フラグ |
|---|---|
| スクリプト融合 | `MEGA_BATCH`, `MEGA_BATCH_YIELD`, `MEGA_DISPATCH`, `MEGA_PARTIAL`, `CORO_GENERAL` |
| 送受信の融合 | `MEGA_SINGLE_SENDER`, `MEGA_BCAST_RECEIVERS`, `MEGA_SINGLE_RECEIVER` |
| Sequencer 一括呼出し | `MEGA_SCALAR_BATCH`, `MEGA_DIRECT_SCALAR`, `MEGA_MULTI_PC_BATCH`, `MEGA_SINGLE_DISPATCH`, `MEGA_SINGLE_FAST_SCAN`, `MEGA_SINGLE_PREPROVE`, `MEGA_SINGLE_SPECIALIZE` |
| 反復の融合 | `MEGA_REPEAT_BURST`, `MEGA_MULTI_REPEAT_BURST` |
| 条件分岐 | `MEGA_IF_ELSE_BATCH`, `MEGA_BRANCH_PAIR` |
| broadcast / until | `MEGA_UNTIL_BCAST`, `MEGA_UNTIL_OUTER`, `MEGA_UNTIL_POST_SET`, `MEGA_UNTIL_OUTER_POST_SET`, `MEGA_NORMALIZE_YIELD`, `MEGA_FAST_BCAST_LATCH`, `MEGA_FAST_VAR_PAIR_JOIN` |
| ScratchBench 専用 | `MEGA_UNTIL_BENCH_ROUND`, `MEGA_UNTIL_BENCH_BEGIN`, `MEGA_UNTIL_BENCH_END`, `MEGA_UNTIL_VAR_PAIR` |
| スケジューラ前段 | `MEGA_SCHED_PREFILTER` |

`XYO_JIT_CORO_GENERAL`（一般スクリプトの LLVM コルーチン化）は、`MEGA_BATCH_YIELD` の source proof が成立する形状には適用しない（既存バッチが優先）。詳細は `mega-general-coro-2026-11-19.md`。

`MEGA_DISPATCH`（自作ブロックの全融合）と `MEGA_PARTIAL`（大きいグラフ用の
部分融合）は同時に有効でよい。全融合が成立したスクリプトでは
`MEGA_DISPATCH` が優先され、成立しなかったスクリプトでは `MEGA_PARTIAL` だけが動く。

## 対応形状の拡張（Stage 1）

同期バッチ `XYO_JIT_MEGA_BATCH` は、これまで `CompiledScript::procedures`
が空のスクリプトしか受け付けなかった。現在は emit 後に
`mega_goto::fuse(..., force=true)` で到達可能な手続きグラフをスクリプト
エントリへ融合する（emitter が既に小さな手続きをインライン化済みの場合は、
未使用になった手続きシンボルを削除するだけ）。

これにより **yield しないスクリプトなら自作ブロック付きでも**
switch(pc) バッチに参加できる。テストは
`testMegaBatchProcedureDispatch`（2 ハット + 手続き、`definitions=1`）。

まだ対象外: yield するスクリプト（非 warp ループ）、`wait` /
`broadcast and wait` / `ask and wait` などの不透明サスペンド、clone・
broadcast 受信ハット（sync バッチは緑旗のみ）。

## 検証

### 1. 単体テスト（プリセットのみ）

```sh
XYO_JIT_MEGA=1 build/xyo-tests
```

`scheduler: 897 checks, 0 failures`（Mega 無効時の通常構成は 474 checks）。
内訳は既存の scheduler テストに加え、`XYO_JIT_MEGA_*` を見て実行される
Mega 系テスト（yield switch、broadcast 継続、multi-PC、repeat burst、
if/else、branch pair、until、goto、partial など）が全部走った状態だ。

Mega フラグを 1 つずつ明示して全 ON にした場合も同じ 897 checks / 0 failures。
プリセットと手動全 ON が一致することを確認した。

### 2. fixture パリティ（interpreter vs プリセット）

`tests/mega/run_mega_suite.py` が 9 個の sb3 fixture を生成し、
`XYO_JIT_MODE=off` と `XYO_JIT_MEGA=1` の Stage 変数ダンプを完全一致で比較する。

```
yield / batch / goto / ifelse / two_scalar / receiver / broadcast /
broadcast_repeat / ifelse_pair     → 9/9 一致
```

### 3. 機能別 A/B ハーネス

`tests/bench/algos/bench_mega_*.py` を `--runs 1` で実行（各ハーネスが
期待する Scratch 状態を厳密に検査する）。13 本すべて PASS。

| ハーネス | 速度比（1 ペア、参考） |
|---|---:|
| `bench_mega_yield` | 1.29× |
| `bench_mega_batch` | 1.91× |
| `bench_mega_goto` | 1.46× |
| `bench_mega_ifelse` | 1.06× |
| `bench_mega_two_scalar` | 1.55× |
| `bench_mega_receiver` | 1.31× |
| `bench_mega_broadcast` | 1.34× |
| `bench_mega_multi_pc` | 1.21× |
| `bench_mega_branch_pair` | 1.04× |
| `bench_mega_multi_repeat_burst` | 4.21× |
| `bench_mega_single_dispatch` | 1.17× |
| `bench_mega_outer_until` | （時間依存の探索値。速度主張なし） |
| `bench_mega_single_receiver` | （同上） |

速度は 1 ペアの参考値で、マシン負荷で変わる。ここでの主目的は**正しさ**で、
各ハーネスは ON/OFF 双方の最終 Scratch 状態が期待値と一致しなければ失敗する。

### 4. 実プロジェクト（ScratchBench scheduler 8 本）

`bench_mega_burst_real_sched.py --pairs 2` を
`debug/projects/performance.sb3` に対して実行した。

- `multi_burst=0`: 実 `sched_*` カーネルはまだ source-certified 形状に
  一致せず、Mega は発火しない（既知の制約）。
- 速度比は 0.98〜1.06×（誤差）。クラッシュ・最終状態の不一致は無し。
- つまり現状の Mega は「対応形状の fixture」では効くが、実 ScratchBench
  の scheduler スイートはまだ取り込めていない。ここを広げるのが次の課題。

### 5. 再実行

```sh
# 単体 + パリティ + 機能別 A/B（約 2〜3 分）
python3 tests/mega/run_mega_suite.py --binary build/xyo

# 速い確認（単体を除く）
python3 tests/mega/run_mega_suite.py --binary build/xyo --skip-unit
```

CTest には `xyo-mega-preset`（ユニット）と `xyo-mega-fixtures`（fixture 層）
を追加した。

## 制約（未完了の設計課題）

- **Thread 選択まで LLVM に移す「完全グローバル goto」は未実装**。
  現状は 1 個の LLVM 関数を複数スクリプトで使い回す段階で、次に走らせる
  Thread を選ぶのは C++ Sequencer のまま。`mega_goto.hpp` の冒頭コメント
  にあるとおり、任意の継続を再開する ABI が別途必要。
- **HotSpot では動かない**（Eager 専用）。プリセットは Eager を選ぶが、
  `--jit=hotspot` を明示すると Mega は何もしない。
- **renderer 接続時と block count 計測時は対象外**（`installMegaDispatch`
  が早期 return）。`xyo play` や `--block-count` では無効。
- **AOT / WASM は対象外**。
- ScratchBench の `suite_sched` 全体はまだ Mega 化できない（`bench_end`
  までは source-certified で対応済み、上位の呼出し階層が残る）。
- 固定作業の `sched_pingpong` は Mega でも Fiber より遅い場合があり、
  既定 ON にはしていない（本プリセットは opt-in のまま）。

## 実装ファイル

- `src/engine/mega_preset.hpp` / `.cpp` — プリセット本体
- `src/engine/sequencer.cpp` — `Sequencer` 構築時の適用と Eager 選択
- `src/shell/main.cpp` — `--mega` と usage
- `tests/mega/run_mega_suite.py` — 3 層の検証ランナー
- `tests/unit/test_scheduler.cpp` — 両融合同時有効時の期待値修正
- `tests/CMakeLists.txt` — `xyo-mega-preset` / `xyo-mega-fixtures`
