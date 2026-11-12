---
id: PROF-005
title: ScratchBenchでのGuard低速経路実測と優先順位分析
status: done
owner: agent-prof-gpt6
priority: P1
updated: 2026-11-12
---

## Scope

- Run PROF-004 guard telemetry against the real 47-case ScratchBench project.
- Generate isolated copies of missing cases without modifying the original Fractch project.
- Identify frequently executed slow paths; do not confuse instrumented time with performance improvement.

## Acceptance criteria

- [x] Isolated Release LLVM binary built from the current local working tree.
- [x] Renderer missing include bypassed in isolated Ninja rule only; PEN-002 source untouched.
- [x] Full 47-case ScratchBench finished with exit 0 and DONE.
- [x] Native hits: typed_ssa_cache_miss 29,806,331; numeric_profile 574,232; other named categories zero.
- [x] Existing 26 probes passed; 27 missing canonical cases isolated and all passed.
- [x] Together covered all 47 canonical workload IDs; 39 deterministic checksums matched, eight scheduler checksums differed because the workloads are time-dependent.
- [x] Root hotspot hypotheses: math_mandel typed SSA cache misses; app_ast, algo_ack, algo_fib numeric profile fallbacks.
- [x] TypeScript harness and log analysis tests 5/5 passed.
- [x] Reproduction and performance caveats documented.
- [ ] Full parity suite and uninstrumented A/B: future work, not a result of profiling.

## Files / dependencies

- Built binary: board/.prof-005-cmake/xyo (SHA256 991126e31c98f37c84775008463db7951c9575fd1d7945c53fd5de919ae70117)
- Full logs: board/prof-005/full.stdout.log, full.stderr.log, full.summary.json
- Existing cases: board/prof-005/results.json
- Added cases: board/prof-005/isolated-results/results.json
- New tools: tools/jit-guard-profile.ts, tools/jit-guard-profile.test.ts, tools/jit-guard-isolate.ts
- Detailed report: docs/jit-guard-scratchbench-profiling-2026-11-09.md

## Next

Inspect typed.ssa.convert with math_mandel and numeric_profile fallbacks in AST/Ack/Fib; any speed claim requires uninstrumented paired benchmarks and parity tests. No commit or push.
