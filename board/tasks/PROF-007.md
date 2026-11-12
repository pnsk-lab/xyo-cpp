---
id: PROF-007
title: CFG型ヒントの選択的適用とコンパイル負荷評価
status: done
owner: agent-prof-gpt6
priority: P1
updated: 2026-11-12
---

## Scope

Add an opt-in static profitability filter for CFG numeric-shadow type
hints so low-demand loop variables do not trigger full dataflow
analysis, while retaining all previous type/alias/yield safety proofs.

## Acceptance criteria

- [x] Standalone profitability predicate, overflow-safe and LLVM-free.
- [x] Selective flag requires existing CFG_TYPE_HINTS flag; defaults unchanged.
- [x] Skip full CFG analysis if no candidate variable is profitable.
- [x] Only candidate variables may be promoted by the original full CFG proof.
- [x] Standalone C++ -Werror unit passed.
- [x] Headless Release LLVM executable compiled in a separate build dir.
- [x] Mandelbrot k_x promoted, typed.ssa.convert site removed in selective mode; checksum 1957 unchanged.
- [x] Low-demand CFG fixture skipped, interpreter parity retained; mixed String|Number fixture not specialized.
- [x] Differential tests for warp/non-warp and eager/hotspot: 12/12 passed.
- [x] Bun A/B regression tests: 6/6 passed.
- [x] Isolated A/B: six original cases x four runs per mode, plus four supplementary cases, all successful.
- [x] Full 47-case performance project: six runs, 47 rows and DONE each, deterministic checksums matched.
- [x] No unrelated JIT/OSR/renderer source edits; git diff --check passes.
- [ ] Cross-host replication / official Scratch parity suite / adoption as default not completed.

## Results

- Original 47-case project: geometric mean full CFG/OFF 0.98257,
  selective/OFF 1.01596. Only 2 measurements per mode; do not
  treat this as a proven or official overall performance difference.
- Isolated Mandelbrot: full/OFF 1.0313, selective/OFF 1.0341,
  same checksum in all runs.
- Type safety: 12/12 differential passes.
- Output and logs: board/prof-007/full-ab/,
  board/prof-007/isolated-ab/, board/prof-007/validation-ab/.
- Detailed report: docs/jit-cfg-type-selective-2026-11-09.md.

## Files

- src/jit/codegen.cpp (small scoped filter insertion)
- src/analysis/cfg_type_hint_profitability.hpp (new)
- tests/unit/jit_cfg_hint_profitability_smoke.cpp (new)
- tests/differential/jit_cfg_type_hints_selective.py (new)
- tools/jit-cfg-selective-ab.ts (new)
- tools/jit-cfg-selective-ab.test.ts (new)

## Decision

Review only. Keep CFG hint and selective hint opt-in.
No commit or push.
