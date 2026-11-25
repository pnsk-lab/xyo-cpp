# MATH-007 — measured math reporter SSA and exact pow10 table (2026-11-08)

## Reproducible benchmarking

Created \`tools/bench_math_abba.py\` for paired ON/OFF performance
comparisons using the original ScratchBench source and pinned CPU
affinity. Each group consists of OFF→ON→ON→OFF or its inverse
ON→OFF→OFF→ON, alternating by group. Ratio calculation uses geometric
means of the two ON and two OFF runs; group-to-group medians are
reported separately for all groups and stable groups. Drift within
either ON or OFF replicates beyond an 8% ratio makes that group unstable.
Each run records the before/after CPU frequency. A/B comparisons always
use the same binary and check that every checksum matches.

After the high host load from MATH-006 subsided (load average ~7, CPU12
sampled ~20% busy while idle), the ABBA comparison produced:

| Workload | Median stable speedup | Valid groups | Checksum |
| --- | ---: | ---: | ---: |
| Math Rounding | 1.1330x | 8/8 | 47241333 |
| Math Trigonometry | 1.0923x | 7/8 | 421876317 |
| Math Exponential & sqrt | 1.0568x | 6/8 | 718102 |

These are ON/OFF ratios for the MATH-006 pure f64 math SSA compared
with the previous compiler. Samples:
\`build/math007-abba.json\`, \`build/math007-abba.log\`.

## Extended corner-case parity

The independent
\`tests/differential/jit_transcendental_repeat_ssa.py\`
now includes additional negative and positive rounding tie cases via
\`round_ties\` (round/floor/ceiling on i/2 or i/3), negative zero, extreme
double values and trigonometric inputs around ±90/270 degrees.
Each of four categories (round, round_ties, trig, expsqrt) evaluates
32 input cases, 129 observed outputs, warp/nonwarp, reference
interpreter, baseline Eager, optimized Eager and HotSpot. All matched.
Log: \`build/math007-expanded-differential.log\`.

## Exact constant-table specialization for 10^x

The Math Exponential & sqrt inner loop repeatedly evaluates
\`10^(k/20)\` for integers k in 1..50. The original JIT performs a
general-purpose libm pow call for every iteration.

The JIT now emits a constant LLVM table of pow(10, k/20) for the 51
inputs k=0..50, evaluated through the host libm during JIT compilation.
The fast path:
1. Accepts a plain IEEE-754 x only within [0, 2.5].
2. Converts x*20 to a candidate table index, avoiding undefined FPToUI
   conversion using a safe select when x is out of range.
3. Reconstructs candidate/20 in IEEE f64 and tests exact equality to x.
4. Selects the table value only on an exact hit. Otherwise it branches
   to the original libm pow(10,x) call, including signed zero, NaN,
   infinity, strings and values near but not equal to a grid point.

The constant table is read-only and safe for parallel native threads.
The generated code never skips a Scratch evaluation side effect because
the math operation is pure and all input reporting is already performed.

The table is enabled only for JIT; AOT always retains libm pow because
cross-platform build/target libm implementations can differ. The
environment setting \`XYO_JIT_POW10_TABLE=0\` disables the table.
It is evaluated during code generation, not inside the native loop.

An 8-group ABBA comparison of pow10-table ON vs OFF, with the *same*
floating SSA path on both sides, returned a median 1.1974x with 8/8
stable groups; checksum 718102 matched. Data:
\`build/math007-pow10-abba.json\`,
\`build/math007-pow10-abba.log\`.

The independent
\`tests/differential/jit_pow10_table_ssa.py\` evaluates all 51 nominal
table inputs, adjacent floating-point values for selected indices,
negative and out-of-range inputs, negative zero, NaN, infinity and
numeric/non-numeric strings. There are 241 observable outputs, with
warp/nonwarp and interpreter/baseline Eager/table Eager/table HotSpot
all matching. Log: \`build/math007-pow10-parity.log\`.

The guarded table is specialized by input value, NOT by Scratch project
name or benchmark ID. It can benefit any eligible pure native SSA loop
that repeatedly evaluates such arguments.

## Integration

MATH-007 owns \`src/jit/numeric_repeat_float_ssa.inc\` (the MATH-005
floating-repeat specialization), the benchmark script, independent test
harnesses and documentation. The main shared \`src/jit/codegen.cpp\`,
renderer, scheduler, and other agents' source changes were untouched.
No commit or push was performed.

The existing unrelated scheduler test failure reported during MATH-006
remains assigned to SCHED-001; it reproduced with all math SSA off.

## Final default-on results

The constant-table fast path was restricted to JIT code generation with
an explicit \`!aot\` guard and made default ON for qualifying JIT float
SSA expressions. \`XYO_JIT_POW10_TABLE=0\` restores the original libm
pow for every expression. AOT always uses libm regardless of the flag.
The exact x == k/20 runtime guard remains mandatory.

On the final default-on binary:
- Independently generated pow10 parity: every one of the 241 observed
  values matched on warp/nonwarp, baseline Eager, optimized Eager,
  optimized HotSpot and interpreter.
  \`build/math007-final-pow10-diff.log\`
- Expanded MathOp/round-ties corpus (four families with 129 observed
  values each): all Eager/HotSpot warp/nonwarp comparisons matched.
  \`build/math007-final-math-diff.log\`
- ScratchBench parity: **2123/2123** for each of HotSpot JIT, Eager
  JIT, Interpreter and AOT.
  \`build/math007-final-parity.log\`
- Complete 47-kernel performance suite finished in both table-OFF and
  default table-ON modes. All NON-SCHEDULER checksums matched (zero
  mismatches). In this sequential run Exponential & sqrt increased
  48062.9 → 57789.7 ops/ms (1.2024x), while N-body, Rounding and
  Trigonometry remained practically unchanged.
  \`build/math007-full-pow10-off.log\`,
  \`build/math007-full-pow10-on.log\`.
  The eight-group paired measurement (median 1.1974x) remains the more
  reliable quantitative comparison.

No changes were made to scheduler/renderer or the main shared
src/jit/codegen.cpp. CTest's previously reported scheduler-only
failure remains assigned to SCHED-001.
