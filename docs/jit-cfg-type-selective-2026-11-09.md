# PROF-007: selective CFG numeric-shadow hints (2026-11-09)

## Summary

PROF-006 identified approximately 20–30 million named typed.ssa.convert fallback
entries in the Mandelbrot workload. CFG numeric-shadow analysis proved k_x to
have Number-only writes despite the dynamic expression k_x = k_z. Enabling
XYO_JIT_CFG_TYPE_HINTS=1 avoids these conversions, but enabling it for every
loop showed inconsistent performance outside Mandelbrot.

PROF-007 adds an **optional compile-time profitability filter**:

    XYO_JIT_CFG_TYPE_HINTS=1
    XYO_JIT_CFG_TYPE_HINTS_SELECTIVE=1

Both flags must be set. The default remains OFF. This is an experiment,
NOT a v1.2.x release default.

## Implementation

New header: src/analysis/cfg_type_hint_profitability.hpp
Small scoped change: src/jit/codegen.cpp, beginLoopPromotion()

For each scalar with uncertain writes, the cheap filter requires:

- At least three statically counted numeric uses in the scanned loop.
- Numeric uses >= twice the number of uncertain write sites.
- A real scalar read and a resolved, unambiguous variable identity.

If no variable meets this condition, skip the expensive target-aware
analyzeTypeFlow CFG/dataflow pass entirely. If a variable passes, the
**unchanged** conservative type-flow analysis must still prove that every
reachable uncertain write leaves its storage kind Number, with no truncated
CFG or aliases. This filter is purely a profitability heuristic; it never
proves a type, suppresses a Scratch conversion needed for correctness, or
relaxes a yield/GC/OSR guard. The division comparison avoids arithmetic
overflow with large user-controlled CFGs.

Without XYO_JIT_CFG_TYPE_HINTS_SELECTIVE, the original CFG hints path is
unchanged. The new header has no LLVM dependency and a standalone unit test.

## Guard telemetry verification

Fixed, newly built headless Release LLVM binary:

- Binary: board/.prof-007-cmake/xyo
- SHA256: be95c9f68539df56a0760750ca925f808d57ce3d271198c6ec46a3e28c637e5e

On board/prof-007/fixtures/math_mandel.sb3 with guard telemetry and verbose
JIT logging enabled:

| Mode | Additional shadow promoted | typed.ssa.convert sites | Checksum |
| --- | --- | ---: | --- |
| OFF | none | 1, with approximately 29 million entries | 1957 |
| Full CFG | k_x | 0 | 1957 |
| Selective CFG | k_x | 0 | 1957 |

All three runs exit 0 and produce DONE. Runtime guard counters perturb
throughput; do not use those runs to infer performance improvements.

The small generated numeric-copy differential fixture illustrates the filter:
the full CFG path promotes a low-demand variable but the selective path skips
it. Both match interpreter output. A mixed String|Number fixture is never
incorrectly specialized.

## Isolated clean A/B (CPU14, same binary, Guard OFF)

Six cases, four runs per mode, balanced/reversed ordering:

| Original case | Full CFG / OFF | Selective / OFF |
| --- | ---: | ---: |
| math_mandel | 1.0313 | 1.0341 |
| loops_repeat | 1.0001 | 1.0001 |
| app_ast | 0.9936 | 1.0009 |
| algo_ack | 0.9827 | 1.0041 |
| algo_fib | 1.0018 | 1.0073 |
| sched_clonechurn | 1.0338 | 1.0626 |

Raw: board/prof-007/isolated-ab/results.json

The first five cases have stable checksums across modes and runs.
The scheduler workload's checksum varies by execution time, so it is
inappropriate to assert that its observed change is causal.

Four supplementary isolated cases, four runs per mode:

| Case | Full CFG / OFF | Selective / OFF |
| --- | ---: | ---: |
| str_cast | 0.9607 | 1.0181 |
| math_strcompare | 1.0048 | 1.0002 |
| algo_matmul | 1.0010 | 1.0058 |
| list_replace | 1.0146 | 1.0077 |

All four checksums remained identical. Raw:
board/prof-007/validation-ab/results.json

Most effects besides Mandelbrot are small and not independently significant.

## Full 47-case A/B (same binary, Guard OFF)

Original built project: scratch-bench/.cache/build/performance.sb3
Order: OFF, full, selective, selective, full, OFF (CPU14).

| Run | Mode | Process wall time (ms) | Benchmark rows |
| --- | --- | ---: | ---: |
| 0 | OFF | 34,192 | 47 |
| 1 | Full | 34,279 | 47 |
| 2 | Selective | 34,137 | 47 |
| 3 | Selective | 34,115 | 47 |
| 4 | Full | 34,551 | 47 |
| 5 | OFF | 34,376 | 47 |

All six runs exited 0 and printed DONE. All 39 deterministic benchmark
checksums matched; eight scheduler checksums are time-dependent.

Unweighted geometric mean of per-case median ON/OFF rate ratios:

- Full CFG: 0.98257; faster on 18/47 cases.
- Selective CFG: 1.01596; faster on 47/47 cases.
- Selective / full: 1.03399.

**Do not report these as confirmed speedups.** There are only two full-suite
samples per mode. It is suspicious that all 47 heterogeneous workloads
appear faster with the selective mode, even those it would not alter.
Shared CPU interference, execution order, clock/scheduler variability
or changes in other concurrently running tasks may explain part of this.
The per-case geometric mean is not the official weighted ScratchBench
overall score, and the full-suite benchmark does not replace the
Scratch parity suite.

Raw: board/prof-007/full-ab/results.json and indexed stdout/stderr logs.

## Safety and build checks

- Standalone C++ unit: jit_cfg_hint_profitability_smoke.cpp compiled with
  g++ -std=c++20 -Wall -Wextra -Werror and passed.
- New differential fixture: jit_cfg_type_hints_selective.py:
  12/12 cases, warp and non-warp, eager/hotspot, numeric and mixed types.
- Bun: 6/6 tests across jit-cfg-selective-ab.test.ts and
  jit-cfg-type-ab.test.ts.
- Full headless Release LLVM executable built successfully.
- git diff --check on src/jit/codegen.cpp passed.
- No changes to runtime comparison/NaN semantics, CallFrames, OSR state,
  interpreter or rendering source.

A concurrent DLSS/PEN change introduced an undeclared
render::DlssOpaqueCoverage reference in sdl_renderer.hpp. For this
diagnostic build only, the build-local generated Ninja rule for
sdl_renderer.cpp.o forced the existing dlss_opaque_coverage.hpp include.
No renderer source was changed. Regenerating CMake files may remove
this workaround; PEN should fix the source include separately.

## Reproduction

Build and run unit tests:

    g++ -std=c++20 -O2 -Wall -Wextra -Werror \
      tests/unit/jit_cfg_hint_profitability_smoke.cpp \
      -o board/.prof-007-candidate-smoke
    board/.prof-007-candidate-smoke
    bun test tools/jit-cfg-selective-ab.test.ts \
      tools/jit-cfg-type-ab.test.ts
    python3 tests/differential/jit_cfg_type_hints_selective.py \
      board/.prof-007-cmake/xyo

Three-way isolated A/B:

    bun tools/jit-cfg-selective-ab.ts \
      --bin board/.prof-007-cmake/xyo \
      --fixtures board/prof-007/fixtures \
      --names math_mandel,loops_repeat,app_ast,algo_ack,algo_fib,sched_clonechurn \
      --reps 4 --cpu 14 \
      --out board/prof-007/isolated-ab

Source SB3s were generated from the existing ScratchBench Fractch
source using tools/jit-guard-isolate.ts; original benchmark files were
not mutated.

## Decision and follow-up

Keep both CFG hint flags OFF by default and submit PROF-007 for review,
not immediate release. Prefer further independent paired runs on a
less-contended host, startup-inclusive compilation measurements, and
targeted parity/GC/OSR tests. The filter is a conservative profitability
gate, not a general solution for type specialization. No source names,
variable IDs or benchmark identifiers are hard-coded into the optimizer.

No commit or push has been made.
