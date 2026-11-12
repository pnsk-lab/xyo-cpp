# LLVM repeat SSA / lazy post-optimization IR (2026-11-10)

## Correction: the previous IR dump was *not* always post-O3

The original `XYO_JIT_DUMP2=1` was printed after `jitEmitScript` but **before
O3** when the script selected lazy materialization. Such modules include the
`xyo.lazy.optimize` module flag and are optimized later by the lazy ORC IR
transform (`configureLazyIR`). Thus an earlier `app_life` dump with many
`rep_i32` allocas was not evidence of inefficient *final* native IR.

Confirmed with the original dump: `build/multi-bench-20261110/repeat-ssa-inspect/current.ll`
contains 14 `alloca i32`; LLVM 23 `opt -passes=mem2reg` eliminates all 14.
Using `XYO_JIT_LAZY_MODE=off` to force eager O3 produces actual optimized
IR with zero `alloca i32` and 68 integer PHIs across the Life module.

### Fix

In `src/jit/backend_llvm.cpp`, `XYO_JIT_DUMP2=1` for a lazy unit now prints
inside `configureLazyIR`, **after** the O3 pass pipeline, matching the
non-lazy/prepared JIT behavior. When lazy IR is queued, it is no longer
printed as though optimized. `XYO_JIT_DUMP_PREOPT=1` is a separate flag for
examining the initial deferred module. Both flags together print both
versions (raw first, optimized second).

The actual O3 files from this turn:

- `build/multi-bench-20261110/repeat-ssa-inspect/after-fix-on.ll`
- `build/multi-bench-20261110/repeat-ssa-inspect/after-fix-off.ll`

Both contain no integer allocas and 68 integer PHI occurrences; both produce
Life checksum 53. The throughput measurements on this busy host should not
be compared as a performance A/B of lazy vs eager, because compile and
execution timings differ.

`tests/differential/jit_dump2_postopt.py` generates an independent
2-function Scratch program, checks identical output with lazy ON/OFF,
checks promoted loop counters in optimized IR, and verifies explicit raw
plus optimized dumping via `XYO_JIT_DUMP_PREOPT=1` + `DUMP2=1`.

## Safe exact-repeat range information for LLVM

Optional `XYO_JIT_REPEAT_COUNTER_NOWRAP=1` adds `nuw` and `nsw` properties
to a **private** i32 countdown only if the existing generator already
proved that the initial literal Scratch `repeat` trip count is within
`[0, INT32_MAX]`. The loop header only enters the body if the count is
nonzero, and the private counter has no Scratch-visible writers. The
properties apply to subtraction of one on that guarded path, not to general
f64 or dynamically sourced repeat counts.

LLVM's O3 IR confirms the property is consumed: the latch changes from
`add i32 %rep_i32.0, -1` to `add nsw i32 %rep_i32.0, -1`, an equivalent
machine operation with stronger scalar-evolution information. LLVM is still
responsible for selecting further loop transformations; no source-level
loop expansion has been added.

### Correctness and performance

- Dedicated `tests/differential/jit_repeat_counter_nowrap.py`: **11** Scratch
  fixtures, including negative/fractional trip counts, 0/1, counts above
  64, and nested repeats, matched Interpreter, Eager and HotSpot with
  no-wrap enabled and disabled.
- Unmodified ScratchBench GUI parity (`d64a28bfd605ed0c`) with opt-in
  enabled: **2656/2656** PASS in Interpreter, HotSpot, Eager and AOT;
  `scratch-bench/results/llvm-repeat-nowrap-parity-20261110/`.
- Six interleaved A/B pairs across 10 ScratchBench-derived workloads gave
  inconsistent speed ratios: geometric means `life` **0.986**, `primes`
  **1.061**, `collatz` **0.933**, `list_append` **0.915**, `expsqrt`
  **1.069**. The host load average was roughly 7–8 and individual runs
  swung by tens of percent. **These numbers do not establish a repeatable
  speed improvement or regression.** Raw results are in
  `build/multi-bench-20261110/repeat-nowrap-ab.{json,log}`.
- Therefore `XYO_JIT_REPEAT_COUNTER_NOWRAP` remains **OFF by default**.

An independent experiment applying branch-frequency weights to the guarded
constant-modulo path did not show a repeatable benefit under the same noisy
machine conditions, and was **fully rolled back**. The earlier, tested
`XYO_JIT_MOD_SMALL_WRAP` and `XYO_JIT_MOD_REAL_WRAP` optimizations remain.

## Next meaningful optimization boundary

The standard fixed-repeat integer counter is already converted to LLVM SSA
by O3. Duplicating this optimization in C++ would not help. For the next
larger gain, the relevant *unproved* facts are instead the Scratch variable
and list-item types/ranges spanning each yield-free loop body. Exposing only
**verified** facts to LLVM (without crossing list writes, GC, yield, or
source mutation) is the remaining compiler-front-end task. Use the
corrected post-O3 dumps and machine objects to distinguish existing
LLVM optimizations from genuinely missed opportunities.

No commit or push; other agents' working-tree edits were not reverted.
