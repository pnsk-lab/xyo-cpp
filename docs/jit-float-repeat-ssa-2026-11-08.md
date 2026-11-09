# Guarded floating-point repeat SSA (2026-11-08)

Task: MATH-005. Goal: improve the numerical recurrence in two-body
simulation without weakening Scratch arithmetic or scheduler semantics.

## Scope

New implementation: src/jit/numeric_repeat_float_ssa.inc

Existing integration: src/jit/numeric_repeat_ssa.inc

The primary LLVM code generator (src/jit/codegen.cpp) was not edited for
MATH-005. This avoids conflicts with IR-001, RT-001 and other agents.

The existing pure repeat parser admits scalar set/change statements, native
numeric literals, variables and side-effect-free +, -, *, / and sqrt. The
new float specialization is selected only if a statically eligible body:

1. Has at least five update statements and includes sqrt.
2. Has no modulo or other reporter with potentially different semantics.
3. Executes under a warp script with an exact i32 repeat countdown.
4. Has a runtime entry guard that all referenced values are genuine
   non-NaN Scratch Number values, not strings, booleans or tagged values.

Otherwise the original repeat body is left intact, or an earlier restricted
integer SSA path remains available.

## Native execution

- Carry every numerical scalar in an LLVM f64 PHI. Repeated Scratch value
  boxing/tag inspection and memory writes are removed from the loop body.
- Generate IEEE-754 +, -, *, / and the same LLVM sqrt intrinsic used by the
  ordinary JIT. Reassociation/unsafe fast-math is not enabled.
- Track NaN at EVERY intermediate arithmetic result, not just final values:
  Scratch may canonicalize an intermediate NaN and use it as zero in a later
  operation.
- If any intermediate expression produces NaN, reject the *current*
  iteration, commit only completed PHI iterations, adjust the native private
  repeat countdown and yield count, and execute the failed iteration through
  the original JIT code. The ordinary yield and countdown then execute once.
- After up to 64 successful native iterations, publish every written
  variable as an actual Scratch Number, update the original countdown and
  run the same warp deadline check as the ordinary JIT.
- If the warp entry guard fails or exact block counting/profiling is active,
  fall back before executing any native iteration.

The specialization handles Infinity and signed zero without converting them
to an integer or discarding their IEEE-754 bit pattern.

## Configuration

- XYO_JIT_NUMERIC_REPEAT_FLOAT=0: disable this sqrt/multi-update
  specialization (return to the previous compiler).
- XYO_JIT_NUMERIC_REPEAT_FLOAT=1: allow this structure, still subject to
  static and runtime safety restrictions (default).
- XYO_JIT_NUMERIC_REPEAT_SSA=0: disable both existing integer and new float
  repeat SSA paths.

Environment variables are read when compiling, not inside the native loop.

## Results

The unmodified ScratchBench N-body source was executed on the same compiled
native executable with CPU 12 pinned, eight shuffled ON/OFF pairs.

| Benchmark | Earlier JIT | Float SSA | Median speed ratio |
| --- | ---: | ---: | ---: |
| Math / Two-body simulation | ~40.3k ops/ms | ~60.1k ops/ms | 1.4909x |
| Math / Arithmetic | ~243k ops/ms | ~243k ops/ms | 1.0009x |

Checksums matched: N-body 51200; Arithmetic 620203352.
All eight N-body pairs improved (speed ratio 1.4899..1.4939).

Raw paired samples: build/math005-float-paired.json
Sample trace: build/math005-float-paired.log

An additional attempt to omit NaN checks based on local algebraic
properties caused a repeatable 1.7% N-body slowdown (median 0.9834x,
eight shuffled pairs) and was removed. The original conservative
per-intermediate check is retained.

## Compatibility

- tests/differential/jit_float_repeat_ssa.py: 23 independently generated
  N-body configurations, 208 observed variables, warp/non-warp and
  interpreter/Eager/HotSpot. Includes 0/1/63/64/65/127/128/129/255/512/2000
  iteration boundaries, strings, fractions, Infinity, NaN, negative zero,
  very large/small numbers and singular trajectories. An additional
  130-iteration test generates NaN *after* the 64-tick boundary and verifies
  the current iteration is rolled back rather than executed twice.
- tests/differential/jit_numeric_repeat_ssa.py: existing 178-output
  integer/f64 cases, warp/non-warp, Eager/HotSpot/interpreter.

Other regression results are in build/math005-final-differential.log
and build/math005-final-legacy-differential.log.

## Next work

Math Exponential & sqrt, Math Trigonometry, and generalized mixed numeric
loops may require separate structurally safe lowering. Optimizing arbitrary
sqrt loops without the NaN rollback would violate Scratch semantics.

## Final verification

- Full ScratchBench parity with the new path enabled by default:
  HotSpot JIT, Interpreter, Eager JIT, AOT each **2123/2123 (100%)**.
  Log: build/math005-final-parity.log.
- CTest xyo-tests: **1/1 PASS** (47.06 s), see build/math005-ctest.log.
- New independent floating N-body corpus: 208 observed outputs in
  23 input configurations, for warp/non-warp and Eager/HotSpot, all matched
  the interpreter. The 65th-iteration NaN rollback corpus also passed.
  Log: build/math005-final-differential.log.
- Existing 178-output integer/f64 numeric SSA differential: all checked
  modes passed, see build/math005-final-legacy-differential.log.
- The complete 47-kernel unmodified ScratchBench performance project
  completed under both feature-off and default-on settings, and every
  non-scheduler checksum matched. The N-body observation was 32,886 to
  50,176 ops/ms (1.526x) in this **sequential** full-suite run.
  Unrelated kernels also shifted by ~20% due to machine-load/clock drift;
  therefore those unrelated differences are NOT claimed as regressions
  or improvements caused by the float feature. The eight shuffled
  paired samples remain the primary causal benchmark.
  Logs: build/math005-full-off.log, build/math005-full-default.log.

This change is left uncommitted to preserve unrelated multi-agent
working-tree edits. The existing src/jit/codegen.cpp changes owned by
other agents were not modified for this task.
