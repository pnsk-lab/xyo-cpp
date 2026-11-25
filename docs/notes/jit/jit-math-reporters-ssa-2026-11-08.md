# MATH-006 — Math reporter SSA (2026-11-08)

## Scope

Extend src/jit/numeric_repeat_ssa.inc to parse side-effect-free pure math
reporters inside numeric repeat:
- operator_round through the existing emitJsRound implementation;
- operator_mathop abs, floor, ceiling, sqrt, sin, cos, atan, ln, e ^, 10 ^;
- modulo via the existing exact Scratch emitArithNumeric lowering.

The pure floating SSA path in src/jit/numeric_repeat_float_ssa.inc keeps
variables as native f64 LLVM PHIs, and writes real Scratch boxed Numbers
only before the ordinary 64-step warp scheduler checkpoint. Input guards
exclude tagged strings, booleans and NaN boxes. Every intermediate NaN
causes rollback of the CURRENT speculative iteration and normal JIT
execution of that iteration exactly once. Non-warp, exact block-counting
and profiling modes retain original behavior.

The shared numeric-until SSA emitter in src/jit/numeric_until_ssa.inc
was extended for the new MathOp and Round nodes as well, so it does not
try to access their absent right operands. The old Math-004 until numeric
fast path remains eligible only for pure, side-effect-free expressions.

The new optimized MathOp/Scratch-round path is enabled by default for
pure multi-statement warp repeat loops. Set
XYO_JIT_NUMERIC_REPEAT_FLOAT=0 to disable just the float specializations,
or XYO_JIT_NUMERIC_REPEAT_SSA=0 to disable all repeat numeric SSA.

No main src/jit/codegen.cpp edits were made for MATH-006; this file is
being edited concurrently by other agents.

## Performance tests

Unmodified ScratchBench source, fixed CPU12, same binary within each
shuffled ON/OFF pair. Early, relatively low-load two-pair observation:

| Kernel | Median ON/OFF | Checksum |
| --- | ---: | ---: |
| Math Rounding | 1.1227x | 47241333 |
| Math Trigonometry | 1.0874x | 421876317 |
| Math Exponential & sqrt | 1.0426x | 718102 |
| Math Two-body control | 1.4867x | 51200 |

Raw early records: build/math006-initial.json and .log.

Follow-up 8-pair comparisons were unreliable due to high host contention:
load average >30 on 20 cores and CPU12 ~99% occupied. Per-pair ratios
varied from strong regressions to implausibly large improvements even
when no other task launched in this sandbox. Do NOT use these runs to
claim precise speedups. Records:
build/math006-paired.json, build/math006-paired-isolated.json.

Complete 47-kernel performance.sb3 finished both with the new feature
disabled and default enabled. All NON-SCHEDULER output checksums matched,
including 47241333, 421876317 and 718102. The sequential speed numbers
shifted for many unrelated kernels because of concurrent host load;
do not treat them as a causal ON/OFF comparison.
Logs: build/math006-full-off.log, build/math006-full-on.log.

A proposed integer-only lowering for nonnegative floor/round/abs was
slower than floating SSA by about 17% in the initial A/B test and was
removed. Other existing integer SSA optimizations remain unchanged.

## Differential / parity

- tests/differential/jit_transcendental_repeat_ssa.py:
  each of round/trig/expsqrt tested with 22 input configurations,
  89 observable outputs, warp/nonwarp, interpreter, old Eager, new Eager
  and HotSpot. All outputs matched.
- tests/differential/jit_transcendental_until_ssa.py:
  nine independent until-loop configurations, 37 outputs, warp/nonwarp,
  old Eager, new Eager and HotSpot. All matched.
- Existing float repeat, integer repeat and numeric until differential
  suites all passed on the intermediate build.
- Intermediate full ScratchBench parity:
  HotSpot/Eager/Interpreter/AOT all 2123/2123.
  See build/math006-parity.log.
- Final build parity: build/math006-final-parity.log.

## Outstanding unrelated test failure

CTest xyo-tests has one scheduler failure:
tests/unit/test_scheduler.cpp:892,
"pure wait-until receivers exercise direct fiber chain".

The same failure was reproduced with all MATH SSA families disabled:
XYO_JIT_NUMERIC_REPEAT_FLOAT=0
XYO_JIT_NUMERIC_REPEAT_SSA=0
XYO_JIT_NUMERIC_UNTIL_SSA=0
XYO_JIT_AFFINE_UNTIL=0

Thus it is not caused by enabling this numerical SSA path.
Logs: build/math006-ctest.log,
build/math006-ctest-allmath-off.log.
Notified the scheduler owner in tools/board.ts SCHED-001.

During the task, unrelated renderer edits made the CLI target temporarily
fail to compile on an undefined renderScale_ member. The xyo static
library compiled; the previously compiled CLI object files were relinked
using the CMake-generated link script for a math validation executable,
build/math006-final2-xyo. Renderer code was not modified.

Shared worktree modifications from other agents were preserved;
no commit or push was performed.
