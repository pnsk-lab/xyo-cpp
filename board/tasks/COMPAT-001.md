---
id: COMPAT-001
title: WASM/Binaryen backend の LLVM 互換検証と回帰ハーネス
status: done
owner: agent-binaryen-gpt6
priority: P1
updated: 2026-11-16
---

## Scope

LLVM ネイティブ JIT と Emscripten WASM/Binaryen の**実行互換**（同一プロジェクトの
最終値・スケジューラに見える副作用順序・未対応時の安全な Interpreter フォールバック）
を、既存の差分スイート横断で一括検証できるようにする。WASM で全 opcode を
コンパイルすることは目標にしない（docs/jit-backend-compatibility.md の契約）。

## Acceptance criteria

- [x] LLVM と WASM/Binaryen を突き合わせる一括ハーネスを追加する
- [x] 既存の全パリティ/ファズ/副作用スイートが一致することを実測する
- [x] コンパイル成否の誤検出（selecting emitter を compiled と誤認）を修正する
- [x] Emscripten ビルドから再実行できる CMake ターゲットを用意する
- [x] 既知の非等価（結果一致だがコンパイル被覆が違う箇所）を文書化する

## Files / dependencies

tests/differential/llvm_wasm_compat_sweep.py (new),
tests/differential/wasm_warp_procedure.py, tests/differential/wasm_list_extensions.py,
CMakeLists.txt (check-wasm-compat), docs/jit-backend-compatibility.md,
docs/wasm/wasm-hotspot.md. Dependencies: RT-001 / IR-001 for full continuation parity.

## Validation

- Sweep (full): 13 suites PASS (hotspot, compat_fuzz 128 seeds, string,
  list_ext, tagged_list, string_clock, warp_proc 8/8, return_pc_stack 10/10,
  nested_cont, return_stack_fuzz 32, return_pc_fuzz 32, tail_recursion 32,
  nested_tree 48).
- Effect-ordering suites: broadcast / broadcast_wait / looks / pen / clone 5/5.
- `wasm_scratchbench_backend_parity.py` on tests/fixtures/scratch-parity-test.sb3:
  interpreter/builtin/Binaryen all 1899/2123 with byte-identical output and
  94 scripts actually compiled.
- Eager mode (`--jit eager`): simple/conditionals/numeric_list/string/wait
  identical to native LLVM.
- CLI flags `--danger` / `--list-limit` behave identically.
- Known coverage difference: dependent warp recursion
  (`prefix; if (n>0) call self(n-1)`) is compiled by LLVM but stays a full
  interpreter fallback in WASM; final values match. OSR / async compilation /
  native AOT / parallel scheduler / native machine code remain unavailable in
  WASM by design.

## Runtime compatibility improvement (2026-11-16)

Whole-script lowering could succeed while standalone emission failed on a
bounded recursion construct (`WarpTailRecurse` / `WarpReturnStack` /
`WarpReturnPcStack`), and `JitBackend::compile` then returned no code at all:
the entire script ran in the interpreter even though the straight-line numeric
regions around the recursive call were compilable.

- `src/jit/backend_wasm.cpp`: a failed whole-script plan now retries with the
  hybrid planner (`whole-script lowering failed; retrying hybrid regions`)
  instead of returning empty.
- The hybrid `safeRegion` predicate now rejects bounded continuation
  statements (they are emitted only with the fuel flag, which hybrid does not
  set) and walks `Stmt::returnSites[].before`, so one un-emittable recursive
  call becomes an interpreter boundary instead of collapsing the whole
  top-level chain into one interpreted region.
- Result: `prefix; if (n>0) call self(n-1)` scripts now compile the numeric
  regions before/after the call (regions=3, compiled=2) and interpret only the
  recursive procedure. Final values remain identical to LLVM.

Re-verified after the change: `wasm_warp_procedure.py` 8/8,
`wasm_effect_fallback_parity.py` 8/8, `wasm_hotspot_integration.py`,
`wasm_compat_fuzz.py --cases 128`, full 2123-check parity
(interpreter/builtin/Binaryen 1899/2123 byte-identical, 94 compiled), and the
18-suite compatibility sweep.

