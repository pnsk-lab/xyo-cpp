---
id: MLIR-005
title: MLIR の残存制限解消: 再帰手続きコルーチン化 + verify-failure fallback ゼロ化
status: review
owner: agent-dsh
priority: P1
updated: 2026-11-22
---

## Scope

MLIR 前段（`src/mlir/`）の残存制限の解消と最適化。Round 241 追加分:

- ループ運搬 f64 の `numFacts`（non-NaN/finite）不動点解析と heal 省略
- 書き込み側 NaN 正規化（`healCarryOnWrite` / `markArgNonNaN`）
- `indexF` の定数畳み込み（文字列定数索引で fast path が消えるバグ）
- `xyo.list.item.num` のブロック順序、`XYO_MLIR_NOFUSENUM` A/B ゲート
- scratch-bench スイート差の切り分け（heal / リストヘッダ巻き上げ /
  バッチ / 昇格 / fusion / コードサイズをすべて実測で棄却）

## Acceptance criteria

- [x] `XYO_JIT_MEGA=1 xyo-tests` 0 FAIL（Round 241 で再確認 / 非 Mega も 0 FAIL）
- [x] `micro_loop`（change i by 1 50M）が legacy と同等（111ms → 32ms、legacy 29ms）
- [x] scratch-bench スイートが回帰なし（回帰は math_expsqrt -2.7% のみ、
      他は +0.5〜+15.9%）
- [ ] ネストループ昇格（P2）は次ラウンド

## Files / dependencies

- `src/mlir/lower_llvm.cpp`（numFacts / evalFacts / healCarryOnWrite /
  markArgNonNaN / isPairCarry / indexF / list.item.num ブロック順）
- `src/mlir/passes.cpp`（`XYO_MLIR_NOFUSENUM` ゲート）
- `docs/mlir-opt-status.md`（Round 241 節）
