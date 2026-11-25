# LLVM induction-range proof across safe ordered IFs (2026-11-10)

## Summary

The existing xyo-cpp JIT could prove exact integer intervals for a
`control_for_each` index and pass them through pure arithmetic/list reporters
into LLVM SSA. It discarded the interval across *all* conditional controls.
A yield-free `if (round(i) < 250) { change sum by list[i] }` can nevertheless
read the exact same `i`: the `control_if` itself does not write the induction
variable and the entire condition/branch body can be statically checked.

`src/jit/codegen.cpp` now permits **ordered** `control_if` and
`control_if_else` inside the induction proof when:

- The condition directly uses `operator_lt` or `operator_gt`.
- All reachable reporter expressions and branch bodies are recursively
  verified by `finiteIndexBody()` and `strictInductionBody()`.
- No source branch writes the induction variable by ID **or name alias**,
  performs a yield, calls a procedure, mutates a list, accesses an unverified
  reporter, or enters an unrecognized control construct.
- Each list reporter resolves by exact ID to an existing list on this target.
  Other values remain on the full legacy conversion, bounds and comparison
  paths; no list length or list element type is assumed constant.

The proof does not fold Scratch comparison itself. It only retains the
existing verified immutable integer interval so LLVM O3 can use the integer
induction PHI for later list address calculations and bounds checks.

## A/B flag and decision

`XYO_JIT_COMPARE_PROVEN_NUM` supports the following modes:

- **unset** or `3` (default): only verified ordered `<` / `>` IF conditions.
- `0`: original conservative induction behavior (no conditional proof).
- `2`: experimental generic pure IF/IF-ELSE, including equality.
- `1`: same broad branch proof plus an independent experimental numeric
  compare/truthy shortcut for canonically boxed known-integral numbers.

Only the narrow ordered-IF proof is enabled by default. The broad modes
remain opt-in because equality-heavy workloads regressed in paired tests.
No manual loop unrolling, fast-math or speculative type assumption is used.

## Real LLVM O3 IR evidence

Standalone generated Scratch projects in
`tests/differential/jit_proven_compare_induction.py` show:

| Project | conversion calls before | with default ordered proof |
|---|---:|---:|
| `if (round(i) < 250) { list[i] }` | 1 | **0** |
| `if (round(i) = 250) { list[i] }` | 1 | 1 |
| `if (...) { set i = -100; list[i] }` | 0 | 0 |

`call i64 @llvm.x86.sse2.cvttsd2si64` was counted in the optimized
script's LLVM IR, not in unoptimized IR. The index-write fixture never
qualifies for the proof. Compared IR files:
`build/multi-bench-20261110/proven-compare/{lt_round,eq_round,index_write}-{0,3,default}.ll`.

## Performance (paired synthetic Scratch project runs)

`tests/bench/algos/bench_jit_proven_compare_induction.py` generates
projects that repeatedly evaluate a 150,000-iteration Scratch `for each`
inside a warp procedure with list reads and conditional increments. The
script runs 100 warm procedure calls per timer interval; each project has
three timed intervals. The same native executable was used throughout.

Eight interleaved comparisons (modes 0, 2 and 3) using `--modes 0 2 3`; CPU core 8 fixed, Eager JIT release. The
original Scratch program records elapsed time from `daysSince2000()`, with
approximately **1 ms clock resolution**. Ratios below are geometric means
of paired median-duration ratios, not a single-run claim:

| Benchmark | Default ordered-proof speedup vs OFF | Faster pairs |
|---|---:|---:|
| `lt_round` | **1.07111×** | 8/8 |
| `gt_floor` | **1.08853×** | 8/8 |
| `if_else` | **1.05882×** | 8/8 |
| `nested` | **1.07323×** | 8/8 |
| `eq_round` (not eligible) | 0.98816× | 0/8 |

For `eq_round`, the medians were identical at about 10 ms and the difference
is at the measurement-resolution scale. The equality path's generated IR
remains identical in structure to the OFF mode. By contrast, enabling all
pure IF structures without restricting equality (mode 2) reduced the speed
of the equality-heavy benchmark in an earlier 8-pair run. Therefore the
narrow ordered selection is the default.

Raw per-run results:
`build/multi-bench-20261110/proven-compare/benchmark-mode3.{json,log}`;
older broad comparison:
`build/multi-bench-20261110/proven-compare/benchmark.{json,log}`.

## Unrelated benchmark cross-check

Six alternating ON/OFF pairs were run on the unchanged diagnostic `.sb3`
workloads `app_life`, `app_raycast`, `algo_primes`, `algo_sieve`,
`list_append` and `math_mandel`. The rates had a large amount of host noise;
paired geometric-mean ON/OFF ratios were **1.054×**, **0.988×**,
**1.005×**, **1.000×**, **1.020×** and **1.056×**, respectively.
Their paired medians were substantially closer than the geometry suggests,
and there is no reliable regression claim for these unrelated workloads.
All checksum comparisons passed. Raw data:
`build/multi-bench-20261110/proven-compare/unrelated-bench-ab.{json,log}`.

## Correctness

`tests/differential/jit_proven_compare_induction.py` checks eight projects:
ordered/equality comparisons, nested IF, IF-ELSE, fractional bound, mixed
string/number list elements, and an attempted index mutation. Interpreter,
Eager JIT and HotSpot JIT give identical final lists under flags 0/1/2/3
and default. Optimized IR count assertions confirm the restricted proof is
absent for equality and mutated indices. Raw JIT pointer constants differ
between processes, so tests compare relevant instruction signatures rather
than comparing pointer-bearing LLVM textual modules byte-for-byte.

Official GUI ScratchBench parity (source `d64a28bfd605ed0c`) under the
new default passed **2656/2656** in all four modes: Interpreter, HotSpot,
Eager and AOT. Results:
`scratch-bench/results/llvm-ordered-if-range-parity-20261110/`.

No commit/push; shared working-tree changes unrelated to this feature were
not modified or reverted.
