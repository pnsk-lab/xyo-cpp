# Arithmetic/Variables: guarded numeric repeat SSA (2026-11-08)

## Scope

The existing compiler hook for pure conditional repeat is reused, but the
primary src/jit/codegen.cpp stays unchanged. New implementation:
src/jit/numeric_repeat_ssa.inc, included by src/jit/conditional_ssa.inc.

A recognized control_repeat with an exactly known native i32 trip count can
execute a **pure numeric** body as one SSA micro-loop. Supported operations:
set/change scalar and side-effect-free +, -, *, /, %, and sqrt reporters.
Other blocks, ambiguous variable IDs or side effects are rejected at compile
time. The executor keeps exact Scratch boxed Value bits alongside the numeric
shadow; the ordinary emitArithNumeric and emitMathop paths canonicalize NaN
and preserve signed zero.

The native loop publishes all written variables before the same 64-iteration
scheduler/deadline checkpoint as the existing conditional-repeat optimizer.
Other scripts never observe its private state before a scheduler handoff.
The original loop remains the guard failure path.

## Integer carrier specialization

For eligible variables only, use native unsigned i64 SSA induction values
rather than a float SSA shadow.

- The source must contain an integer-modulo recurrence: nonnegative integer
  literal operands and variable dependencies connected exclusively through
  additions, multiplications, positive constant modulo and nonnegative
  change-by (or set from such expressions).
- Classification is a fixed-point over **every assignment** of every
  referenced scalar, so a variable that might be written as a float in any
  branch or sequence is excluded.
- At **every chunk entry**, eligible variables must be plain numeric Values,
  integral, in [0, 2^21], and not negative zero. String, boolean, NaN,
  fractional, negative, infinity and out-of-range values fall back to the
  normal JIT.
- Conservatively propagate upper bounds through every intermediate for all
  64 potential iterations. If any might exceed 2^48, the integer
  specialization is rejected. Every accepted integer is thus exactly
  representable by IEEE-754 double, without rounding differences.
- Integer add/multiply and positive constant modulo lower directly to
  LLVM i64 add/mul/urem. If a mixed-float expression consumes the result, the
  integer is converted to double exactly. The independent float variables
  stay as IEEE-754/boxed SSA values.

In Arithmetic, k_i and k_a use integer SSA; k_b stays floating. The same
optimization benefits the eight-variable modulo/accumulation benchmark.
For nonnegative exact operands, unsigned remainder implements Scratch's
floored remainder without a sign-fix branch. No project name or benchmark ID
appears in the optimizer.

## Defaults and rollback

Safe integer-modulo numeric repeats are enabled by default after successful
parity validation. Pure floating-only SSA is **not enabled by default**, as
it did not materially improve n-body and could introduce untested regressions.

- XYO_JIT_NUMERIC_REPEAT_SSA=0 disables this optimizer entirely.
- XYO_JIT_NUMERIC_REPEAT_SSA=1 explicitly enables the pure floating SSA path
  even when no safe integer recurrence was proven.
- XYO_JIT_NUMERIC_REPEAT_INT=0 disables i64 carriers when experimenting with
  pure floating SSA. The default is enabled *only* on proven integer shapes.
- Exact block counting or XYO_PROF also forces the original path.

This is a code-generation selection: no environment check is emitted into
the native hot loop.

## Paired performance

All comparisons were made from the unmodified ScratchBench source, using
isolated real Scratch SB3s, the same native executable, pinned CPU 12, and
shuffled ON/OFF order. Every compared checksum matched.

| Benchmark | Control | Optimized | Paired speedup |
| --- | ---: | ---: | ---: |
| Arithmetic (6 pairs) | ~122,600 ops/ms | ~243,700 ops/ms | **1.9884x median** |
| Many variables (6 pairs) | ~80,300 ops/ms | ~194,600 ops/ms | **2.4225x median** |
| Arithmetic: i64 vs float SSA (8 pairs) | ~125,100 | ~243,500 | **1.9464x** |
| N-body: i64 vs float SSA (8 pairs) | ~40,400 | ~40,400 | **0.9996x** |

Raw machine-readable samples:
- build/math003-default-paired.json
- build/math003-int-paired.json

The 47-kernel complete performance.sb3 also finished under both conditions.
All **non-scheduler** output checksums were identical. Arithmetic increased
from 121,866 to 243,262 ops/ms; vars_many from 80,494 to 194,484 and
vars_local from 80,480 to 195,161. Timing-sensitive scheduler results are
not parity comparisons. Other categories did not show a comparably strong
gain. Full output is retained in build/math003-full-off.log and
build/math003-full-default.log.

## Rejected experiments

- Boxed-value-only SSA lost ~21% on Arithmetic. The paired numeric shadow
  eliminated per-operation tag/conversion work, recovering +2% on Arithmetic.
- Reducing the proven unsigned remainder to i32 had a repeatable **0.9965x**
  ratio (8 paired runs), so the i32 path was entirely removed.
- More general if-conversion previously regressed and was also removed.

## Compatibility

- Custom independently generated 178-output numerical corpus compares
  interpreter, ordinary Eager JIT, float SSA, integer SSA, and HotSpot,
  for warp/nonwarp programs. Tests include 0/1/63/64/65/129/257 loop
  counts, -0, a negative-zero literal, strings, fractions, out-of-range
  integers, Infinity, NaN, runtime-generated NaN and sqrt.
  Test: tests/differential/jit_numeric_repeat_ssa.py
- Existing conditional-repeat SSA test:
  tests/differential/jit_conditional_repeat_ssa.py
- ScratchBench parity, explicitly enabled and default-enabled:
  **2123/2123 (100%)**. Logs in build/math003-parity.log and
  build/math003-default-parity.log.

The next major work is a general guarded SSA for nested until/while and
mixed IEEE-754 recurrence loops, to address Collatz, Mandelbrot and N-body.
These were deliberately not specialized by benchmark-name patterns.

## Final validation

- Same-binary paired **six** ON/OFF sequences, CPU12 pinned:
  Arithmetic speedup median **1.9884x**, checksum 620203352.
  Many variables speedup median **2.4225x**, checksum 1588830.
  Raw data: build/math003-default-paired.json.
- Signed-zero constant support was corrected during implementation.
  The extended independent corpus validates 178 output variables in all
  eight warp/nonwarp × Eager/HotSpot/default/f64/int routes:
  build/math003-extended-parity.log.
- xyo-tests completed successfully (CTest 1/1, 43.37s):
  build/math003-ctest.log.
- All runtime variants (default enabled) passed the full ScratchBench parity:
  **HotSpot 2123/2123, Interpreter 2123/2123, Eager 2123/2123,
  AOT 2123/2123**. Log: build/math003-all-modes-parity.log.
- No changes were made to the IR-001 owner's primary codegen.cpp or to
  other agents' working files; this shared worktree was not committed/pushed.
