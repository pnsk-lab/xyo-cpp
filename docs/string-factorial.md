# String optimization factorial experiment

This experiment separates three compile-time factors in a randomized complete-block
`2^3` design. The CMake options default to `OFF`; each executable is built with a
fixed configuration so the runtime does not branch on experiment flags.

## Factors and configurations

| Bit | Factor | `OFF` | `ON` |
|---|---|---|---|
| A (bit 0) | Cons allocation | Cons and flat strings share the existing small-string slab pools. | Cons cells use a distinct pool domain, class-specific TLS caches and available lists. |
| B (bit 1) | Dynamic join lowering | `operator_join` uses the generic tagged-value helper. | JIT checks both operands for string tags / the exact empty-string immediate, branches to a string-only helper, and preserves the generic tagged-value fallback. |
| C (bit 2) | Optional profiling | Profiler code is present and gated by runtime enable checks. | Runtime profiler timers, counters and reporting are omitted from the relevant translation units at compile time. |

Variants are named `V0` through `V7` in bit order. The allocator’s mark/sweep
reclaimer remains one physical pass: while flat strings and cons cells share slabs,
a sweep-only cons treatment cannot be measured independently. A isolates allocation
and reclamation only after segregating slab ownership; it does not add a second sweep.

## Build and measurement protocol

[run_string_factorial.py](../tests/bench/algos/run_string_factorial.py) expects eight executable paths in
`V0...V7` order, each beside the `CMakeCache.txt` used to build it. It checks the
factor options and common build/compiler/JIT settings before running. Workload order
is randomized inside every complete block; CPU affinity is pinned when requested;
all inherited `XYO_*` diagnostics are cleared. Each workload’s existing warmup and
three project-timed runs are used, and its checksum is checked before a row is
accepted. Full stdout/stderr, project hashes, executable hashes, order and timing
provenance are retained in the output directory.

[analyze_string_factorial.py](../tests/bench/algos/analyze_string_factorial.py) computes paired speedups against
`V0` and log-linear orthogonal factorial contrasts. For singleton factors the
reported multiplier is the high/low duration ratio; pairwise terms are ratios of
ratios, and the three-way term is the alternating three-factor ratio. Confidence
intervals resample complete blocks. Per-workload intervals are pointwise (not
adjusted for multiple comparisons). A one-block smoke run is only a functionality
check and cannot provide meaningful uncertainty estimates.

Suggested staged commands after building the eight fixed variants:

```sh
python3 tests/bench/algos/run_string_factorial.py \
  --binaries build/string-factorial/V0/xyo build/string-factorial/V1/xyo \
             build/string-factorial/V2/xyo build/string-factorial/V3/xyo \
             build/string-factorial/V4/xyo build/string-factorial/V5/xyo \
             build/string-factorial/V6/xyo build/string-factorial/V7/xyo \
  --out build/string-factorial/smoke --blocks 1 --scale 80 --cpu 12

python3 tests/bench/algos/analyze_string_factorial.py \
  build/string-factorial/smoke/raw.json \
  --out build/string-factorial/smoke/analysis.json
```

The three-block pilot was exploratory. The approved 15-block main experiment and
its final analysis are recorded under Measurements below.

## Correctness and generated-code evidence

- A-specific unit checks verify live allocations and reserved capacity by pool,
  including mixed multithreaded flat/cons allocation and reclamation.
- B’s JIT IR should contain the `join.str` / `join.generic` split and the registered
  `xyo_join_strings` helper for a dynamic string/string join; numeric and other
  tagged operands must still reach the generic helper.
- C is checked both by compilation of all guarded translation units and by running
  with `XYO_PROF=1`: the C build should not emit optional profiler output. This is
  distinct from ordinary timing runs, which always clear diagnostics.
- Project checksums are required to match in every variant. Unit/differential tests
  remain separate from performance evidence.

## Measurements

The one-block smoke covered all 10 workloads × 8 variants (80 checksum-valid rows);
the three-block pilot covered 240 rows. Their raw artifacts are
[smoke/raw.json](../build/string-factorial/smoke/raw.json) and
[pilot3/raw.json](../build/string-factorial/pilot3/raw.json). The approved main run
completed **15 randomized complete blocks** (1,200 checksum-valid rows), scale 80,
CPU 12, seed 20260620, diagnostics disabled. Full provenance and per-run output are
in [main15/raw.json](../build/string-factorial/main15/raw.json); the 10,000-resample
per-workload analysis (bootstrap seed 918273) is in
[main15/analysis.json](../build/string-factorial/main15/analysis.json).

Ratios below are **ON/OFF duration ratios**: below 1 favors enabling the factor.
Per-workload intervals resample the 15 complete blocks for that workload.

| Workload | A: dedicated cons pool | B: specialized join | C: profiler compiled out |
|---|---:|---:|---:|
| `join` | 1.121 [1.082, 1.166] | 0.967 [0.908, 1.035] | 0.938 [0.898, 0.977] |
| `join_number` | 1.101 [1.071, 1.134] | 1.016 [0.984, 1.047] | 0.891 [0.872, 0.908] |
| `join_observe` | 0.987 [0.949, 1.020] | 0.990 [0.967, 1.014] | 0.994 [0.960, 1.033] |
| `join_observe_last` | 1.019 [0.989, 1.053] | 0.964 [0.932, 0.995] | 0.864 [0.825, 0.899] |
| `short_join` | 1.003 [1.000, 1.009] | 1.003 [1.000, 1.009] | 1.003 [1.000, 1.009] |
| `short_dynamic` | 1.007 [0.991, 1.022] | 1.022 [1.008, 1.039] | 0.963 [0.951, 0.975] |
| `prefix_fraction` | 1.001 [0.993, 1.008] | 0.997 [0.985, 1.010] | 0.986 [0.980, 0.994] |
| `cast` | 1.006 [0.998, 1.013] | 0.993 [0.983, 1.001] | 0.993 [0.982, 1.003] |
| `letter` | 0.993 [0.985, 1.001] | 1.064 [1.050, 1.079] | 0.829 [0.818, 0.840] |
| `contains` | 1.000 [0.998, 1.002] | 0.995 [0.993, 0.997] | 1.000 [0.999, 1.002] |

Equal-weighting the ten workload kinds (geometric mean; not a real-world duration
mix) gives main-effect ratios A **1.0228 [1.0157, 1.0299]**, B **1.0007
[0.9922, 1.0095]**, and C **0.9441 [0.9366, 0.9512]**. On this suite, A is about
2.3% slower overall, B is neutral on average, and C is about 5.6% faster. This
macro-average and its 10,000-resample block-bootstrap intervals (seed 20260622) weight
each kernel equally; use the per-workload results above for workload-specific claims.

The equally weighted geometric-mean speedups versus V0 for V1…V7 were respectively
0.982 [0.967, 0.998], 0.995 [0.978, 1.011], 0.976 [0.961, 0.990], 1.059
[1.045, 1.073], 1.030 [1.012, 1.048], 1.063 [1.046, 1.081], and 1.035
[1.021, 1.049]. These aggregate variant intervals use 10,000 block-bootstrap
resamples with seed 20260621. V4 (C alone) and V6 (B+C) lead this equal-kernel
summary; adding A reduces the aggregate benefit. This is descriptive for this fixed
suite, not a global runtime speedup claim.

C is consistently beneficial for several string-heavy cases (about 6% on `join`,
11% on `join_number`, 14% on `join_observe_last`, and 17% on `letter`) and its
per-workload interval excludes 1 in six workloads. A's regression concentrates on
`join` and `join_number` (12% and 10% slower); it is close to neutral elsewhere. B is
workload-dependent: about 3.6% faster on `join_observe_last`, but 6.4% slower on
`letter` and 2.2% slower on `short_dynamic`, where runtime values take the generic
fallback. Four workload-level B intervals exclude 1. The aggregate interaction
ratios (AB 1.000 [0.981, 1.019], AC 1.009 [0.993, 1.025], BC 0.990 [0.973, 1.005],
ABC 0.999 [0.965, 1.032]) do not show a consistent suite-wide interaction; there are
some workload-specific terms, detailed in the analysis JSON.

Correctness and mechanism checks passed: V7 `ctest` 6/6 and differential repros 19/19;
A's GC diagnostic showed separate live populations (about 15.2k cons vs 174 general
small objects in the sampled collection); B's generated IR contained the guarded
`join.str`/`join.generic` split and checksum 40,000; with `XYO_PROF=1`, V0 emitted
16 optional profiler tags while V4 emitted none. The working `build/` is restored to
the all-default-off baseline. No runtime defaults were changed: C is a strong
candidate for the normal non-profiling build, but A and B should remain workload-aware
rather than being described as uniform wins.

## References

- [CMakeLists.txt](../CMakeLists.txt) — default-off experiment options and source-specific definitions.
- [gc.cpp](../src/core/gc.cpp) — dedicated cons slab pool and GC diagnostics.
- [codegen.cpp](../src/jit/codegen.cpp) — guarded specialized join lowering.
- [rtprof.hpp](../src/jit/rtprof.hpp) — compile-time profiler stubs.
- [bench_strings.py](../tests/bench/algos/bench_strings.py) — workload generation and checksums.
- [run_string_factorial.py](../tests/bench/algos/run_string_factorial.py) — randomized execution/provenance.
- [analyze_string_factorial.py](../tests/bench/algos/analyze_string_factorial.py) — factorial effects and bootstrap.
- [string-profiling.md](string-profiling.md) — diagnostic-only profiler observations.
- [string-performance.md](string-performance.md) — earlier string implementation measurements.
