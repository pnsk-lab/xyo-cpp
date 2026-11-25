# Conditional SSA: nonnegative remainder specialization (2026-11-08)

## Scope

This implements a narrowly guarded optimization of the pure conditional
control_repeat integer-SSA path in src/jit/conditional_ssa.inc. There is no
benchmark-specific condition or Scratch project rewrite.

Only pure branch conditions and an existing statically eligible integer repeat
can enter the native SSA chunk. At runtime, every referenced scalar must be
a finite, integral, plain number within +/-2^40, and at most 64 iterations
execute before publishing the Scratch variables and checking the scheduler.

For the nonnegative modulo specialization:

1. All writes in all branches are monotone changes by nonnegative integer
   literals. Set and negative changes prohibit this optimization.
2. Every referenced input variable is nonnegative at the native chunk entry.
   Negative inputs take the original JIT path.
3. The specific modulo dividend is proven nonnegative by induction over
   variables, nonnegative constants, pure addition and modulo. Subtraction
   does not preserve that proof.
4. The modulo divisor is the already restricted positive integer literal
   from 1 through 65536.

LLVM may then emit native unsigned i64 remainder, removing signed-rem
correction and branches. Unproven cases use the existing signed remainder
with Scratch correction. A negative-zero bit pattern is now rejected at
the guard; an unchanged variable must preserve its original sign.

Debug executions that require exact per-block counts or XYO_PROF tracing
use the original path because a native chunk bypasses Scratch reporter calls.

## Configuration

- XYO_JIT_CONDITIONAL_SSA=0 disables the whole restricted integer SSA repeat
  specialization. It is now enabled by default after parity validation.
- XYO_JIT_CONDITIONAL_UREM=0 uses signed remainder inside the eligible plan.
  Unsigned remainder is enabled by default only when proven legal.

Both options support independent A/B measurement and immediate rollback.

## Paired benchmark

Unmodified isolated ScratchBench math_compare, CPU 12 pinned, same native
binary, six shuffled ON/OFF pairs, exactly the same checksum 5738:

- Unsigned vs signed remainder, both with conditional SSA enabled:
  speedup median 1.3750x. Example unsigned 451,763 ops/ms and signed 326,068.
- Whole SSA versus ordinary JIT, unsigned enabled:
  speedup median 1.7845x across four shuffled pairs; checksum 5738.

Results: build/math002-urem-paired.json and build/math002-ssa-paired.json.
An initial alternative based on branchless select regressed by roughly 16% in
Number comparisons; it was removed rather than deployed.

Arithmetic, Collatz and Mandelbrot have no confirmed material improvement
from this restricted repeat optimization.

## Correctness

- tests/differential/jit_conditional_repeat_ssa.py: warp/nonwarp,
  interpreter/Eager/HotSpot, LLVM O2, negative/fractional/string inputs,
  nested branches, and 63/64/65 yield boundaries.
- tests/differential/jit_conditional_ssa_zero.py: negative zero retained across
  an eligible repeat where its variable is not updated. The reciprocal
  remains negative infinity, matching interpreter.
- ScratchBench full parity with feature explicitly enabled: 2123/2123.
- ScratchBench full parity with feature on by default: 2123/2123.
  The log is build/math002-default-parity.log.

## Remaining work

The primary Arithmetic kernel contains mixed floating arithmetic and modulo,
Collatz has a nested until loop, and Mandelbrot has nested floating-point
recurrences. None matches this strict integer-repeat grammar. Those require
broader numeric-loop lowering with safe guards, observable writeback,
deoptimization and correct scheduler resume state.

## Full-suite validation

The standard 48-benchmark ScratchBench performance project completed in both
default-on and forced-off configurations on the same pinned CPU. All
non-scheduler checksums matched. In that unpaired sequential observation,
math_compare was 209,461.0 ops/ms with the old path and 430,918.9 ops/ms
with the new default path (2.057x); checksum 5738. Other Math workloads
also increased by 14-19% despite being outside this specialization. This
indicates run-to-run CPU/environment drift; it must not be claimed as a
causal speedup from the modulo change. The more reliable matched small
benchmark establishes the ~1.78x median improvement in number comparisons.

Full outputs: build/math002-full-off.log and
build/math002-full-default.log.

CTest: xyo-tests 1/1 passed (48.59 seconds), see
build/math002-ctest.log.
