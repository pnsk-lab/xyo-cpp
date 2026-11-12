# PROF-005: ScratchBench native JIT guard fallback survey (2026-11-09)

## Summary

The opt-in `XYO_JIT_GUARD_STATS=1` instrumentation from PROF-004
was exercised against the original ScratchBench 47-case project and a
complementary collection of isolated cases. Every profiled run produced
`DONE` and exited with code 0.

**These are counts of entries into named LLVM native fallback basic blocks,
not runtime Deoptimizations and not timings for comparing performance.**
The counter incurs a function call and atomic increment when hit; LLVM
also retains IR names while profiling.

### Full 47-case project: one diagnostic run

- Input: `scratch-bench/.cache/build/performance.sb3`
- Mode: Release, LLVM HotSpot, `--fps 30`, `--print-list output`
- Result: 47 benchmark data rows, `DONE`, exit code 0
- Instrumented process wall time: 34.378 seconds (**not a performance score**)
- Frozen executable: `board/.prof-005-cmake/xyo`
- Binary SHA256: `991126e31c98f37c84775008463db7951c9575fd1d7945c53fd5de919ae70117`

| LLVM slow-path category | Preoptimization IR sites | Native entries |
| --- | ---: | ---: |
| `typed_ssa_cache_miss` | 4 | 29,806,331 |
| `numeric_profile` | 65 | 574,232 |
| `numeric_equality` | 5 | 0 |
| `plain_comparison` | 1 | 0 |
| `induction` | 2 | 0 |

Other audited categories had no reported instrumented site in this run.
This instrumentation is selective: not all fast paths, comparison guards,
helper-function fallbacks or Deoptimizations were counted.

Raw data: `board/prof-005/full.stdout.log`,
`board/prof-005/full.stderr.log`,
`board/prof-005/full.summary.json`.

### Standalone case isolation: 47 unique original workloads

First, 26 existing isolated projects under
`build/scratchbench-cause-probes/projects` were measured. They cover
20 unique canonical ScratchBench workload IDs plus alternative
diagnostic variants. All 26 passed.

To cover the remaining 27 workload IDs, `tools/jit-guard-isolate.ts`
copied the checked-in Fractch source into
`board/prof-005/isolated/src/<case>`, retained the original case body and
operations-per-unit, and compiled case-specific .sb3 files with the local
`fractch` CLI. The original `scratch-bench/performance/project` files
were never changed. All 27 isolated projects passed.

**Across the combined observations, all 47 canonical workload IDs are
represented.** 39 workload checksums matched the full run exactly.
The remaining 8 were the time-dependent `sched_*` benchmarks; their
execution counts/checksums differ between isolated and full suite by
design. This is a case-output consistency check, not the full Scratch
parity suite.

Nonzero fallback counts in the isolated corpus:

| Case | Category | Count |
| --- | --- | ---: |
| `math_mandel` (`mandel`) | `typed_ssa_cache_miss` | 20,778,648 |
| `app_ast` | `numeric_profile` | 452,760 |
| `algo_ack` | `numeric_profile` | 46,291 |
| `algo_fib` | `numeric_profile` | 27,295 |
| `app_raycast` | `numeric_profile` | 56 |
| `algo_matmul` | `numeric_profile` | 7 |
| `algo_sort` | `numeric_profile` | 7 |
| `list_search` | `numeric_profile` | 7 |
| `sched_clonechurn` (`clonechurn`) | `numeric_profile` | 4 |
| `sched_pingpong` (`pingpong`) | `numeric_profile` | 4 |

The remaining case-level observations registered zero entries in the
audited named fallback blocks.

**Interpretation:**
- The `typed.ssa.convert` hot fallback appears concentrated in
  Mandelbrot. The full suite runs 168,347,011 Mandelbrot operations
  versus 117,187,117 in the `mandel` isolated run (about 1.44x);
  the typed SSA hit counts likewise scale by about 1.43x.
  Thus the additional ~9M hits in the full run are consistent with
  different Mandelbrot work volume, and need not originate in
  unrelated kernels.
- The `numeric_profile` fallback is concentrated in AST,
  Ackermann and Fibonacci, which often exercise mixed-type/value
  handling and procedure boundaries.
- A high fallback-entry count is **not automatically a performance
  regression**. Check whether values are intentionally invalidated
  at yields/writes, inspect optimized LLVM output, measure the
  actual conversion/helper call cost, and preserve Scratch coercion
  semantics before attempting changes.
- The isolated and full runs have different timings/iteration counts.
  Their raw hit counts are not directly additive.

Raw data: `board/prof-005/results.json` (existing 26),
`board/prof-005/isolated-results/results.json` (27 missing);
each fixture also has stdout/stderr logs beside its result file.

## Reproduction

Build an instrumented headless binary from the current working tree:

```sh
cmake -S . -B board/.prof-005-cmake -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DXYO_JIT=ON \
  -DXYO_JIT_BACKEND=llvm -DXYO_SDL2=OFF \
  -DXYO_BUILD_TESTS=OFF -DXYO_DISABLE_RT_PROF=ON
cmake --build board/.prof-005-cmake --target xyo -j4
```

At the time of this diagnostic run, concurrent PEN-002 work introduced an
unresolved `render::DlssOpaqueCoverage` reference in
`src/render/sdl_renderer.hpp`. The isolated build was unblocked by
injecting `-include
/home/kicky/dev/cpp/xyo-cpp/src/render/dlss_opaque_coverage.hpp`
**only into the `sdl_renderer.cpp.o` rule in the generated
`board/.prof-005-cmake/build.ninja`.** This is an ephemeral
diagnostic build workaround, not a source fix and not a normal build
procedure. Regenerating CMake files will erase the workaround; the PEN
owner should resolve the missing include separately.

Run the full project and existing probes:

```sh
XYO_JIT_MODE=hotspot XYO_JIT_GUARD_STATS=1 \
  board/.prof-005-cmake/xyo run \
  scratch-bench/.cache/build/performance.sb3 \
  --print-list output --fps 30 \
  >board/prof-005/full.stdout.log \
  2>board/prof-005/full.stderr.log

bun tools/jit-guard-profile.ts \
  --bin board/.prof-005-cmake/xyo \
  --fixtures build/scratchbench-cause-probes/projects \
  --out board/prof-005
```

Build additional isolated fixture cases:

```sh
bun tools/jit-guard-isolate.ts \
  --names app_ast,algo_ack,algo_fib,app_raycast,list_read \
  --out board/prof-005/isolated

bun tools/jit-guard-profile.ts \
  --bin board/.prof-005-cmake/xyo \
  --fixtures board/prof-005/isolated \
  --out board/prof-005/isolated-results

bun test tools/jit-guard-profile.test.ts tools/jit-guard-analyze.test.ts
```

## Next steps

1. Investigate the one frequently entered `typed.ssa.convert` BB in
   Mandelbrot. Check whether `p->lazyValid` is correctly invalidated
   per write/yield, whether conversion is mandatory on each iteration,
   and whether LLVM hoists or inlines helper calls. No unsafe
   assumption about mutable Scratch values is permitted.
2. Map `numeric_profile` slow entries to AST, Ackermann and Fibonacci
   variables and inspect whether an alternative type-specialized
   execution tier would reduce *actual* time without degrading parity.
3. Run paired **uninstrumented** performance tests after any candidate
   optimization. Include Scratch parity, OSR/yield/GC tests, startup
   and memory measurements; do not report these instrumented
   timings as speedups.
4. Do not extend the instrumentation indiscriminately to every LLVM BB:
   extra calls, barriers and IR name retention can perturb compilation
   and execution, as earlier profiling experiments demonstrated.
