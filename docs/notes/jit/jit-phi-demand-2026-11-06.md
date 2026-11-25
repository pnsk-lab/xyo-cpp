# Demand-driven branch PHI emission (2026-11-06)

## Why

Earlier mixed-type LLVM SSA forwarding created a boxed i64 PHI at every eligible
`if` / `if-else` join, then a double PHI when both producers had proven
`toNumber` and comparison-eligibility values. Some PHIs only linked values
that were overwritten before the next read. Other PHIs carried a numeric
shadow for variables next used exclusively as strings.

## Implementation

`src/jit/codegen.cpp` now provides a bounded per-branch cost analysis:
`phiDemandAfter(branch)`. It inspects the linear stack chain following the
branch (at most **32 statements**), in normal Scratch evaluation order:
input reporters are evaluated *before* the assignment to that block's target.
The analysis follows reporter graphs, caps recursion at **32 levels** and
the total reporter walk at **256 nodes**, and stops at control, procedure,
broadcast or unknown-effect boundaries. It tracks which variable IDs have
been overwritten so only reads before the next write count.

A variable used before its next write needs a boxed PHI; a variable used
numerically (add/subtract/multiply/divide/modulo/round/mathop/comparison and
change-by operands) may also profit from the f64 PHI. A purely string
consumer can reuse the boxed producer without paying the numeric PHI cost.

This analysis changes **profitability only**. Skipped PHIs fall back to
canonical variable loads and guarded Scratch coercion. It never treats a
variable as permanently numeric or changes type-check correctness.

### A/B controls

- `XYO_JIT_PHI_DEMAND=1`: Enable the demand filter for eligible existing
  if-else merges and the experimental ordinary-if false-edge forwarding.
- `XYO_JIT_PHI_DEMAND=0` or unset: Preserve the previous unconditional
  eligible-PHI generation.
- `XYO_JIT_IF_PHI=1`: Independently enable ordinary-if false-edge PHIs.
  This remains OFF by default.

## Validation

The regression fixture `tests/differential/jit_if_phi.py` now includes
a mixed-type variable that is overwritten before its next read, plus another
variable read only in a string concatenation after the join. It asserts
interpreter parity for warp/nonwarp, eager/hotspot, default flag/explicit
flags, JIT O2, and dumped IR. With `XYO_JIT_IF_PHI=1`:

| PHI type (unoptimized LLVM IR) | Demand OFF | Demand ON |
| --- | ---: | ---: |
| i64 (boxed) | 25 | 20 |
| double (numeric) | 20 | 10 |
| total | 45 | 30 |

Both warp and nonwarp fixtures give these counts. This is a **33% reduction
in the counted PHIs of this fixture**, NOT a 33% runtime speedup.

`tests/differential/jit_type_flow_fuzz.py` now includes two more
configurations, extending 24 generated mixed-type projects from 96 to 144
interpreter-vs-JIT comparisons, including eager and hotspot with demand ON.

## Startup-inclusive A/B timing

Three modes (`if` PHI disabled, PHI always, demand-driven PHI); two
wall-time measurements per mode, in a forward/reverse crossover order.
All application correctness checks matched.

| Workload | no if-PHI | all if-PHI | demand-driven |
| --- | ---: | ---: | ---: |
| dedicated warp mixed-type | 94.79 ms | 94.04 ms | 93.92 ms |
| quicksort_4000 | 153.95 ms | 153.47 ms | 151.22 ms |
| mergesort_4000 | 127.89 ms | 125.01 ms | 125.17 ms |
| sieve_100000 | 102.14 ms | 103.14 ms | 100.83 ms |
| fib_27 | 87.21 ms | 85.77 ms | 86.14 ms |
| micro_loop_1000000 | 29.52 ms | 30.77 ms | 30.60 ms |

These timings include project loading, JIT compile, value serialization and
runtime. Two measurements per mode are not enough to distinguish small
speedups from noise; this does not establish a universal performance gain.
The feature is still opt-in.

## Final regression result

After the final bounded-reporter implementation, the full
`ctest --test-dir build --output-on-failure -j4` run passed **40/40**
tests in 167.35 s. This includes the widened 144-case differential fuzz,
the PHI-IR count assertions, OSR, event scheduler, Binaryen backend,
numeric comparisons and all unit tests.

## Next

Measure compilation and machine-code size separately, expand real SB3
coverage, and consider whole-function liveness when useful. Importantly,
a demand scan must never remove PHIs needed after external writes or scheduler
handoffs: this filter is local to the same post-join stack span.
