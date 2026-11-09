# Guarded numeric and integer affine repeat-until SSA (2026-11-08)

## Motivation and architecture

Pure numerical Mandelbrot and Collatz inner loops performed expensive
Scratch value boxing, scalar loads/stores and per-iteration scheduler
steps. A private native SSA chunk can execute pure expressions in
registers and publish results only before the regular 64-tick warp
scheduler checkpoint.

The implementation is structurally recognized; no benchmark names
or project IDs are hardcoded. Added:
- src/jit/numeric_until_ssa.inc: pure numeric repeat-until SSA
- src/jit/affine_until_ssa.inc: guarded integer affine/parity SSA
- src/jit/numeric_repeat_ssa.inc: exact variable-copy support in the
  existing numeric body parser (only for guarded until)
- src/jit/conditional_ssa.inc: includes new modules
- src/jit/codegen.cpp: a minimal hook in the existing until body
  basic block; the original slow body is left intact

## Floating point semantics

The compiler recognizes only side-effect-free scalar assignments and
pure arithmetic with a Boolean predicate built from numeric comparisons,
AND/OR/NOT. Initial inputs must be plain Number values; tagged strings,
booleans or NaN boxes use the regular JIT loop. Non-warp scripts are
always handled by the original execution path.

Optimistic native f64 math preserves operation order and IEEE-754
semantics; no fast-math reassociation is permitted. Every intermediate
is checked for NaN. If any operation generates NaN, the entire
speculative CURRENT iteration is discarded: the previous completed
SSA state and tick count are published, and the unchanged slow body
executes the rejected iteration exactly once. That preserves Scratch's
special NaN canonicalization and subsequent conversions.

If a pure predicate becomes unsafe or indicates completion, all
completed state is published and the original condition is rechecked
at the ordinary header. A 64-step poll invokes the normal warp deadline
check. Impure reporters and floating modulo are not accepted.

## Integer affine semantics

Recognized recurrence, not benchmark-specific:
- until scalar n equals nonnegative literal stop;
- if n modulo positive constant equals zero:
  set n = n / divisor;
- else set n = n * multiplier + offset;
- change separate scalar accumulator by a nonnegative literal.

Every referenced field must be a real scalar with matching ID and name.
The input guard requires plain positive integral n and a nonnegative
integral accumulator (negative zero excluded), both <= 2^40.
The multiplier is bounded to <=1024. Before committing an iteration,
both new values must be <=2^48, which guarantees exact IEEE-754 integer
representation and no i64 intermediate overflow. Otherwise the
current iteration is rejected and executed with the original body,
after publishing all previously completed native steps.

Unsigned remainder, integer division, multiplication and additions
execute directly on i64 loop-carried PHIs. The scheduler is checked at
the same 64-iteration boundary as the original JIT.

## Configuration

These proven, narrowly restricted forms are now enabled by default.
For rollback or comparison:

- XYO_JIT_NUMERIC_UNTIL_SSA=0 disables pure numerical until SSA.
- XYO_JIT_NUMERIC_UNTIL_FAST=0 retains the old boxed arithmetic lowering
  within numeric SSA instead of optimistic f64.
- XYO_JIT_AFFINE_UNTIL=0 disables affine integer until SSA.

Exact per-block counting or XYO_PROF disables both optimizations.

## Measured results

Eight shuffled ON/OFF pairs on CPU 12, same executable, unchanged
ScratchBench SB3s and matching checksums:

| Workload | Baseline | Optimized | Median speed ratio |
| --- | ---: | ---: | ---: |
| Mandelbrot | about 155k ops/ms | about 320k ops/ms | 2.0568x |
| Collatz | about 279k ops/ms | about 858k ops/ms | 3.0772x |

Mandelbrot checksum: 1957; Collatz checksum: 3142.
Raw measurements are preserved in build/math004-fast-paired.json and
build/math004-affine-paired.json.

## Correctness

- tests/differential/jit_numeric_until_ssa.py: 24 independent cases
  with 145 observed outputs, warp/non-warp, baseline interpreter,
  Eager and HotSpot, SSA/f64 modes. Conditions include 0/63/64/65/129
  iterations, numeric strings, Infinity, NaN, fractional values,
  negative zero and overflowing expressions.
- tests/differential/jit_affine_until_ssa.py: 21 independent cases,
  85 observed outputs, warp/non-warp, baseline interpreter, Eager
  and HotSpot, including multiple 64-iteration boundaries and
  fallback for non-integer accumulator values.
- ScratchBench explicit feature-on parity:
  HotSpot, Interpreter, Eager and AOT all 2123/2123 (100%).
  Results: build/math004-all-parity.log.
- xyo-tests CTest 1/1 passed.
  Results: build/math004-ctest.log.

## Next

General mixed floating-point N-body and arbitrary control-flow loops
need broader guarded SSA and safe deoptimization rather than these
structure-specific lowering paths. Do not speculate across impure
Scratch reporters or observable scheduling boundaries.

## Final full-suite and default-on validation

Both optimizers enabled *by default* on the rebuilt executable:
HotSpot JIT, Interpreter, Eager JIT and AOT each completed the
full ScratchBench parity suite at **2123/2123 (100%)**.
See build/math004-default-parity.log.

The same binary ran all 47 performance kernels with both until
optimizations OFF and default ON. All non-scheduler checksums matched:
- Mandelbrot: 141,552 -> 297,464 ops/ms (2.101x in this sequential run).
- Collatz: 256,789 -> 709,905 ops/ms (2.765x in this sequential run).
Other unrelated workloads showed clock/CPU-load drift; the interleaved
eight-pair ratios above provide the more reliable causal comparison.
Logs: build/math004-full-off.log and build/math004-full-default.log.

No commit/push was performed; the worktree contains unrelated in-flight
codegen changes owned by other agents and must not be staged wholesale.

The final default-enabled tree was re-linked against xyo-tests and
passed the full CTest target (1/1, 54.57 seconds).
See build/math004-final-ctest.log.

Both independent differential corpora were rerun on the final
default-enabled executable; every expected output matched:
build/math004-default-numeric-diff.log,
build/math004-default-affine-diff.log.
