# MATH-009 — latest-head integration, memory and startup evaluation

Date: 2026-11-08.

## Previous JIT teardown crash and current status

The MATH-008 latest binary built around 21:20 failed with SIGSEGV
during JitEngine ScriptEntry / gpu::ParallelLoop destruction even with
all mathematical SSA disabled. A previous stable binary passed.

A new coherent shared worktree build was present at 21:38. It was
copied to build/math009-stable-xyo to avoid concurrent relinks. This
binary passed the exact previously failing log differential tests and
the entire ScratchBench parity suite:

- HotSpot JIT: 2123/2123 (100%)
- Eager JIT: 2123/2123 (100%)
- Interpreter: 2123/2123 (100%)
- AOT: 2123/2123 (100%)

See build/math009-current-log-parity.log and
build/math009-head-parity.log. The old JitEngine SIGSEGV and AOT
project-hash mismatch did not reproduce in this current snapshot.
The responsible RT/IR agents own the underlying change; no claim
about the precise fix is made here.

Expanded independent differential suites all passed with the same
snapshot: log 295 outputs, exp 241 outputs, pow10 241 outputs and
four Math reporter families each 129 outputs, with warp/non-warp
and Eager/HotSpot against the interpreter. Logs:
build/math009-exp-diff.log, build/math009-pow10-diff.log,
build/math009-math-diff.log.

## Latest full performance suite

Using the same fixed binary and CPU12, all three math tables
ln / exp / pow10 were enabled by default or explicitly disabled.
Both runs completed the unmodified 47-kernel ScratchBench performance
project. All 47 kernel keys matched; all non-scheduler checksums
were identical (zero mismatches).

Math Exponential & sqrt:
- OFF: 47,947.024 ops/ms
- ON: 91,674.945 ops/ms
- Sequential ratio: 1.9120x
- Both checksums: 718102

Other unrelated Math kernels reported 7-16% lower rates in the second
run, demonstrating that shared-host CPU load/clock drift confounds
sequential comparisons. This is a corroborating performance result,
not a sufficiently controlled estimate of the isolated causal gain.

Raw: build/math009-full-off.log, build/math009-full-on.log.

## JIT startup CPU time and peak RSS

Added tools/bench_math_codegen_cost.py. It benchmarks the same
independently generated warp projects in alternating ABBA groups,
preserves the exact output digest, measures per-process wall time,
user+system CPU time via os.wait4 and peak ru_maxrss. This is a
*whole-process launch+compile+execution* test, not an isolated LLVM
codegen microbenchmark.

Eight ON and eight OFF runs for each table (four ABBA groups), pinned
to CPU12. All per-project output hashes matched.

With the existing per-LLVM-module 8193-element ln table:
- Groupwise ON/OFF CPU ratios: 1.124, 1.059, 1.108, 1.212.
- Median within-group peak RSS increase about 4.91 MiB.
- The underlying raw constant data is only about 64 KiB. Extra
  memory and CPU are LLVM module optimization and process-level costs.

With the 51-element exp table:
- Groupwise ON/OFF CPU ratios: 1.272, 1.146, 1.134, 1.292.
- Median within-group peak RSS increase about 3.00 MiB.

These are synthetic *compilation-heavy* projects with many similar
math reporter instances. They do not imply that a typical Scratch
project increases startup by these percentages. Concurrent shared-host
load varied dramatically over the run; wall times are noisy.
Raw: build/math009-codegen-cost.json and .log.

A single ScratchBench parity run with the new tables enabled had
startup about 4.39 s HotSpot, 39.98 s Eager, 4.77 s Interpreter and
6.83 s AOT, but ran concurrently with differential tests and severe
host contention. Do NOT ascribe the 39.98 s Eager figure to MATH-008.

## Experimental shared process-lifetime ln table

Created an optional native-JIT-only alternative in
src/jit/numeric_repeat_float_ssa.inc:
- Setting XYO_JIT_LOG_SHARED_TABLE=1 switches from an immutable
  per-LLVM-module global to an immutable, process-lifetime vector
  populated from the exact jsnum::jsLog implementation.
- The compiled native JIT module embeds that host pointer using
  an int-to-pointer LLVM constant and then indexes it.
- Only allowed for 64-bit pointers and non-AOT native code.
  AOT continues using the original runtime function.
- The heap allocation intentionally remains alive until process
  termination so JIT code cannot outlive the referenced data.

This avoids emitting the 64 KiB LLVM global in each module. It comes
with process-local native pointer assumptions, so it is OFF by default.
The original per-module table remains default.

Added passthrough of XYO_JIT_LOG_SHARED_TABLE to
tests/differential/jit_log_table_ssa.py. All six differential
configurations (295 output values each) passed on the experimental
compiled binary build/math009-shared-xyo:
build/math009-shared-parity.log.

Also added a shared-table mode to tools/bench_math_abba.py and to
tools/bench_math_codegen_cost.py.

The intended four-group A/B startup benchmark was stopped after
the first complete group because shared-host saturation made the
measurements impractically slow (roughly 15-24 seconds per test run).
The available samples do not establish a memory/compilation benefit,
so this option has NOT been enabled by default.
Partial data: build/math009-shared-cost.json, .log.

## Sources and ownership

Modified math-owned native SSA include and differential/benchmark
harnesses only. src/jit/codegen.cpp timestamp was touched to trigger
recompilation, but no new control-flow edits were introduced by MATH-009.
Shared RT/IR work remains owned by its agents.

No commit or push was made. All in-flight modifications from other
agents were retained.
