---
id: PROF-006
title: MandelbrotのTyped SSA冗長変換と型推論の安全性・性能評価
status: done
owner: agent-prof-gpt6
priority: P1
updated: 2026-11-12
---

## Scope

Investigate PROF-005's 29.8M typed SSA slow-path hits with source-level attribution, CFG proof, uninstrumented A/B and differential tests. Do not change concurrent IR/OSR/JIT implementation code.

## Acceptance criteria

- [x] Identified `k_x = k_z` as unknown producer; verbose CFG proof promotes `k_x` to eager numeric shadow.
- [x] CFG flag removes the `typed.ssa.convert` site; checksum 1957 preserved.
- [x] Clean CPU14 Mandelbrot A/B 10 runs per setting after 4 warmups: ratio 1.0707, noisy.
- [x] Full 47-case OFF ON ON OFF A/B complete: four 47/47 runs, DONE, exit 0, 39 deterministic checksums match.
- [x] Full-suite unweighted geometric ON/OFF ratio 0.9172; significant regressions observed but unconfirmed outside full-suite context.
- [x] Rechecked four standalone fixtures at four runs per setting; Mandel 1.0338, loops_repeat 1.0000, app_ast 0.9871, sched_clonechurn 0.9868.
- [x] Type-hint differential safety test passed 10 paths, type-flow safety test passed 12 paths.
- [x] New TypeScript A/B harness and unit tests (3/3).
- [x] Report and raw data saved; no shared source or default flag changed.
- [ ] Full independent GUI parity and multi-host replication are future tasks.

## Files

- `tools/jit-cfg-type-ab.ts`, `tools/jit-cfg-type-ab.test.ts`
- `docs/jit-cfg-type-guard-ab-2026-11-09.md`
- `board/prof-006/flags.json`, `mandel-clean-ab.json`, `full-ab/results.json`, `isolated-ab/results.json`
- `board/prof-006/mandel-cfg-verbose.stderr.log`

## Decision

Keep `XYO_JIT_CFG_TYPE_HINTS` opt-in. Do not claim universal speedup or attribute the full-suite slowdown to CFG alone. Next candidate: selective profitability gate and source-level CFG analysis caching after clean A/B evidence.
