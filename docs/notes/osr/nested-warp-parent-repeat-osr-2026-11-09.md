# OSR-005: 外側の counted repeat を保持した LLVM 継続

2026-11-09

## 変更範囲

既存 OSR-004 の静的な warp→counted-repeat 選択器に、外側の `control_repeat` の実行中フレームを保持する経路を追加した。

```text
root
  └─ control_repeat（本来の残り回数は Interpreter に保存）
      └─ procedures_call（warp）
          └─ control_repeat（残り部分だけ LLVM で実行）
              → 元の warp / 外側 repeat の継続へ
```

実験的な `XYO_JIT_OSR_NESTED_PARENT_REPEAT=1` と `XYO_JIT_OSR_NESTED_DEEP=1` の両方が必要。既定では無効。呼出し経路中の親 `repeat` を最大2つまで許可し、warp 呼出し8個・IFフレーム4個と合わせて最大14個の親フレーム（+ LLVM 化する内側 repeat 1個）に制限する。内部用の source-arm `3` は「ループ本体への制御辺」を意味し、`1`/`2`（if then/else）、`0`（warp call）と区別する。

## 実装

- `src/jit/nested_warp_osr.hpp`: 既存の `scanStatements` に root の直列ブロック列も入力し、親 `control_repeat` の SUBSTACK から warp 呼出しを探索する。親 TIMES は `safeNestedValue` の純粋 reporter whitelist で事前に制約する。ネストは最大2つに制限。
- `src/engine/runtime.cpp`: source revision、一致する親スタック ID、sourcePath が裏付ける実行中のループ親子関係と、各親フレームの `isLoop`・`hasLoopCounter`・有限・非負・整数の実カウンターを照合。外側 TIMES は**再評価しない**。内側 repeat の checkpoint もこれまで通り確認。
- `src/engine/sequencer.cpp`: ネイティブ完了後、すべての空の非 loop 制御フレームは巻き戻すが、親フレームが `isLoop` の場合は**終了させず**、元の Interpreter にループ判定を任せる。親の既存カウンターを変更しない。
- `src/jit/rt.cpp`: native 読出しでは実際の最内側呼出しフレーム位置を使うため、親 `repeat` フレームを挟んでも引数を取り違えない。最大15個の実スタックフレームに対応。
- LLVM の共通 CallFrame ABI / RT-001 のハンドオフには手を加えていない。親の StackFrame と実カウンターを**元の Interpreter が保持したまま**、LLVM は内側のループの残りだけを処理する。

## 実行

```bash
XYO_JIT_MODE=hotspot \
XYO_JIT_ASYNC=1 \
XYO_JIT_OSR=1 \
XYO_JIT_OSR_NESTED=1 \
XYO_JIT_OSR_NESTED_DEEP=1 \
XYO_JIT_OSR_NESTED_PARENT_REPEAT=1 \
XYO_JIT_OSR_NESTED_ARG_READS=1 \
XYO_JIT_OSR_NESTED_DIRECT_ARGS=1 \
XYO_JIT_OSR_NESTED_HOIST_ARGS=1 \
XYO_JIT_OSR_TIMING=1 \
    ./board/osr-parent-repeat-verified-xyo play project.sb3
```

分岐内の親ループと warp の組合せを対象にする場合は `XYO_JIT_OSR_NESTED_BRANCH=1` も併用。

## 実テスト結果

`tests/differential/jit_nested_warp_parent_repeat_osr.py`（CTest #58、**1/1 PASS、102.27秒**）は全 Scratch 変数を Interpreter と比較し、実際の `[jit-osr-nested] promoted` / `restored caller` ログの件数を照合する。以下の初回実行で全 PASS。

| 入力と検証 | 結果 |
|---|---|
| root `repeat(3)`、TIMES変数を本体で17に変更 | reporter / 直接 / SSA の各モードで実OSRが3回、最終状態一致 |
| root `repeat(2) × repeat(2)` | 実OSRが4回、最終状態一致 |
| warp 手続き内の親 `repeat(3)` | 実OSRが3回、最終状態一致 |
| root 親 `repeat(3)` と IF 制御フレーム | 実OSRが3回、全状態一致 |
| 親 repeat フラグ OFF | native 移行拒否、Interpreter と同じ結果 |
| 実行ブロック数 | Interpreter / reporter / 直接 / SSA で **120,000,043** |
| 文字列・UTF16 引数の強制 GC | 直接 / SSA で各 **1,033回**の GC、最終状態一致 |

凍結一貫ビルド:

```text
board/osr-parent-repeat-verified-xyo
Binary SHA256: 2c1796db501665eb162c53b7c8d4145cf0152f9ea53775e769c2d10a7b985540
Source SHA256: 7b6b653e60f161a1958cdae98c9941313759bd769590435196a453aed74aa740
```

## 未対応

一般的な source PC からの再開、非 warp yield、再帰、SSA 内部位置での rollback、任意の深さ・種類の制御フレーム、外側ループ本体の JVM/LLVM 全面実行は対象外。今回は既存のインタプリタから LLVM に限定ループだけを貸し出す方式であり、携帯可能な CallFrame の復元とは別経路。実行中のソース編集に対する一般 deopt も未実装で、移行時のソース revision 検証だけを維持する。

共有ツリーのため commit / push は行わない。

## 互換性回帰と選択器の修正

元の2フレーム OSR（root → warp → repeat）を DEEP=0 で走らせたところ、共通スキャナー化で root の procedures_call を DEEP=1 時しか探索しなくなった退行を発見。`scanStatements` の root 呼出しに限って DEEP=0 でも `scanCall` を許すよう修正した。従来の数値引数 OSR は reporter / direct / SSA 各実移行を確認し、日本語引数も reporter / direct の実移行を確認した。最後の日本語引数+direct の1実行は JIT 非同期完了前にループが終了して移行ログがなく、テストが停止した（Scratch出力差分ではない）。**旧2フレームの全プロモーションが成功したとは主張しない**。

non-warp 内側手続きのネイティブ移行拒否も新バイナリで直接確認した。root 動的 TIMES を含む最新の CTest は追って記載。

互換性修正後の整合ビルド:

```text
board/osr-parent-repeat-verified-v2-xyo
Binary SHA256: cd16cccba547e5e9484d62c7410d726a12c566bbdc3e7671fe2fa7a00090842f
Source SHA256: 5ef8ad00cc704840be1bc5fe0b64aebe7522ba68d6545a82b3bb92aeb39d62b3
```
