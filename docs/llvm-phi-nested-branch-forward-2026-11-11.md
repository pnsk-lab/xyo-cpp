# LLVM exact-integer SSA range forwarding through nested Scratch IF controls

Date: 2026-11-11. Component: `xyo-cpp/src/jit/codegen.cpp`.

## Problem

After `control_if_else` joins two verified, bounded integer SSA producers,
`typed.merge.number` carries a correct range that LLVM O3 can use for later
list indexing. The preceding change permitted one more read-only `if` or
`if else`, but discarded the proof if that arm itself contained an `if`.
That caused LLVM to emit a new general double-to-i64 conversion for
`list[temp]` inside the inner conditional.

## Change and proof limits

`readonlyPhiBranch()` now performs a **bounded recursive scan of nested
`control_if` and `control_if_else`** in addition to its previously supported
straight-line reporters and unrelated scalar assignments.

It admits a nested conditional only when its `CONDITION` passes the existing
`readonlyCondition()` proof. It follows both `SUBSTACK` and `SUBSTACK2`
branches, not just the branch likely to execute. The complete subtree must
be within 256 unique Scratch block IDs and may not contain a cycle, unknown
reporter, procedure or event call, yield, loop, list mutation, protected
scalar write (including another variable ID resolving to the same slot), or
unrecognized control. The scan must actually find a read of the protected
variable. Each contained branch still goes through the ordinary dominance,
branch-local and merge-local SSA facts; none of the generated Scratch
arithmetic or condition semantics are changed.

The compiler only forwards *already established*, immutable, canonical
numeric SSA values with exact signed-i32 ranges into safe branches. LLVM
owns subsequent `InstCombine`, `SimplifyCFG` and integer index lowering.
No source-level loop unrolling, speculative profiling, or custom arithmetic
rewrites were added.

**Default ON after verification.** Set
`XYO_JIT_PHI_NESTED_BRANCH_FORWARD=0` to disable only the recursive proof.
The older proven-index, unary range, PHI range and ordered IF lowering remain
independently gated. The targeted fixtures enable the older optional
`XYO_JIT_LAZY_TYPED_SSA=1` and `XYO_JIT_LOCAL_VAR_FORWARD=1` to ensure
numeric PHIs exist; this patch does not enable them globally.

## Optimized LLVM IR evidence

Generated actual Scratch projects in
`tests/differential/jit_phi_nested_branch_forward.py` were run with the
feature OFF/ON/default. All project outputs match the Interpreter in Eager
and HotSpot. Call count refers to actual optimized LLVM IR, not the
unoptimized builder IR.

| Fixture | OFF calls to `cvttsd2si64` | ON/default |
|---|---:|---:|
| A nested `if` reading `list[temp]` | 1 | **0** |
| A nested `if else` reading in both arms | 2 | **0** |
| Three levels of `if` | 1 | **0** |
| Nested read after another variable write | 1 | **0** |
| Mutates `temp` inside nested IF (reject) | 1 | 1 |
| Nested `repeat` (reject) | 2 | 2 |
| Mixed string/numeric PHI (reject) | 2 | 2 |
| No relevant read (reject) | 0 | 0 |

The compiler's exact bounded proof is the condition for replacing the
general f64-to-i64 conversion with integer SSA; no assumption is made about
numeric strings or dynamically written Scratch variables.

## Performance

`tests/bench/algos/bench_jit_phi_nested_branch_forward.py`: generated
Scratch warp procedure, 120,000 populated list elements, 120 repeated
procedure calls per clock interval, 3 timed intervals per program run, 10
alternating OFF/ON pairs, CPU8 pinned, Eager JIT release. Same executable,
identical project and checksum for each comparison. Speed ratios are the
**geometric mean of paired median-duration ratios**. The Scratch clock has
approximately 1 ms resolution.

| Case | ON/OFF speed ratio | Faster pairs | OFF median | ON median |
|---|---:|---:|---:|---:|
| Nested `if` | **1.2862×** | 10/10 | 28 ms | 22 ms |
| Nested `if else` | **1.2164×** | 10/10 | 36 ms | 30 ms |
| Three-level nested `if` | **1.3333×** | 10/10 | 36 ms | 27 ms |
| Nested unrelated scalar write | **1.2451×** | 10/10 | 33 ms | 27 ms |
| Protected scalar write (ineligible) | 0.9887× | 1/10 | 34 ms | 34 ms |
| Nested repeat (ineligible) | 1.0027× | 1/10 | 36 ms | 36 ms |
| Mixed string and numeric (ineligible) | 1.0015× | 2/10 | 64.5 ms | 64.5 ms |

The small ratios for ineligible fixtures are measurement noise rather than
an effect of changed list-index lowering. Raw benchmark samples:
`build/multi-bench-20261111/phi-nested-forward/benchmark.{json,log}`.

Six unchanged ScratchBench-derived diagnostics, 6 alternating pairs:

| Workload | ON/OFF paired geometric mean |
|---|---:|
| `app_life` | 0.9985× |
| `app_raycast` | 1.0006× |
| `algo_primes` | 0.9994× |
| `algo_sieve` | 0.9975× |
| `list_append` | 0.9944× |
| `math_mandel` | 0.9971× |

All final checksums matched. This is no evidence of a general performance
improvement outside the eligible nested controls, nor evidence of a
reproducible large regression. Raw results:
`build/multi-bench-20261111/phi-nested-forward/other-bench-ab.{json,log}`.

## Correctness and build verification

- Eight targeted Scratch projects: Interpreter/Eager/HotSpot final-state
  parity with flag `0`, `1` and unset; IR asserts the intended four
  reductions, four excluded cases, and default/ON equality.
- Full ScratchBench GUI Parity (source `d64a28bfd605ed0c`) under explicit
  `XYO_JIT_PHI_NESTED_BRANCH_FORWARD=1`:
  **2656/2656 PASS in each** Interpreter, HotSpot, Eager, AOT.
  Results: `scratch-bench/results/llvm-phi-nested-forward-parity-20261111/`.
- Other regressions: `jit_phi_ifelse_forward`, `jit_phi_branch_forward`,
  `jit_phi_integer_range`, `jit_proven_compare_induction`,
  `jit_list_proven_i32_index`, `jit_unary_integer_range` and `jit_if_phi`.
- Full parity with the **normal CMake/Ninja rebuilt executable, default ON**:
  `scratch-bench/results/llvm-phi-nested-default-parity-20261111/`.

At the beginning of the test, an unrelated concurrently edited IR001 header
prevented a full build. The new `codegen.cpp.o` compiled, and a separate
`xyo-nested-optin` executable was linked from existing objects to run the
initial differential, A/B and Parity tests without modifying that header.
After the unrelated header was repaired, **the standard CMake/Ninja build
completed successfully**, and default-ON differential/regression tests were
run against its ordinary `build-perf-life-20261110/xyo` executable.

All changes remain uncommitted and unpushed. No unrelated concurrent source
edits were reverted or overwritten.
