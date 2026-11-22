---
id: MLIR-001
title: MLIR前段（xyo dialect）追加とwarp数値ループの最適化
status: review
owner: agent-mlir-frontend
priority: P1
updated: 2026-11-19
---

## Scope

- LLVM の前段に MLIR 風の SSA IR（xyo dialect: region / block argument /
  verifier / pass pipeline / LLVM lowering）を追加する。MLIR 本体はこの
  環境に無いため、差し替え可能な境界（`src/mlir/ir.hpp` の dialect と
  `lowerToLLVM`）を持つ小型実装として用意する。
- Scratch ブロック木 → xyo dialect → LLVM IR の経路を `XYO_MLIR=1`
  （opt-in）で有効化し、失敗時は LLVM モジュールに触れず既存 codegen へ
  フォールバックする（意味論の後退経路を作らない）。
- warp 手続きを IR の関数（`func.func {warp}`）へ落とし、
  `isWarp` が静的に 1 のループから型変換・変数 load/store・不要な yield を
  削減する。純粋な定数ステップ本体は 64 反復チャンクに畳む。

## Acceptance criteria

- [x] `XYO_MLIR=1` で warp 手続き入りの sb3 が interpreter と同じ最終状態
- [x] 既存ユニット全体が MLIR 有効でも 0 failures
- [x] warp 数値ループで既存 JIT に対して測定可能な改善（50M で約 2.1×、
      200M で約 4.4×）
- [x] `tests/mlir/xcheck.py`（CTest `xyo-mlir-frontend`）eager/hotspot 19/19
- [x] コーパス 45 プロジェクト一致

## Files / dependencies

- 追加: `src/mlir/{ir.hpp,ir.cpp,passes.cpp,lower_llvm.hpp,lower_llvm.cpp,
  scratch_build.hpp,scratch_build.cpp,frontend.hpp,frontend.cpp}`、
  `tests/unit/test_mlir.cpp`、`tests/mlir/xcheck.py`、
  `docs/mlir-frontend.md`
- 最小差分: `CMakeLists.txt`（glob に `src/mlir/*.cpp`）、
  `src/jit/codegen.cpp`（`jitEmitScript` 先頭の 1 フック + include）、
  `tests/CMakeLists.txt`（CTest 1 個）、`tests/unit/test_cast.cpp`
  （テスト登録）、`tests/unit/test_parfor.cpp` / `test_danger.cpp`
  （MLIR 有効時だけ legacy 固有の最適化検査を外す 2 箇所）
- 依存: 既存の `jit/rt.cpp` ヘルパ、`JitCtx::varSlotData` のスロット契約、
  `Blocks::getProcedureDefinition` / `getProcedureParamNamesIdsAndDefaults`
- scheduler / thread / deps / pool には触れていない
