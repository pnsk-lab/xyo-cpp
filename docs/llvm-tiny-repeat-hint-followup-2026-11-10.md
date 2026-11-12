# LLVM-native tiny-repeat unrolling and IR numeric-path investigation (2026-11-10)

## Goal

Improve the **IR fed into LLVM O3**, rather than manually expand Scratch
instructions or replicate LLVM numerical optimization in C++.

## Candidate trials

1. `sensing_timer`: an opt-in native `emitCanon` of the existing `xyo_timer`
   f64 helper eliminated **two `xyo_num` call sites** in the post-O3 IR of a
   synthetic loop; Interpreter/Eager/HotSpot numerical output invariants
   passed. However 12 interleaved short-run pairs (50k iterations) had a
   whole-process speed ratio of **1.010**, only **5/12 wins**; 8 pairs with
   500k iterations gave **0.959**, **3/8 wins**. This path was **rolled back**
   without modifying the previous default behavior. Evidence:
   `build/multi-bench-20261110/timer-numeric-ssa/{ab.json,ab-long.json}`;
   The temporary synthetic test fixture was deleted when the candidate
   was rolled back; the raw results and captured IR remain for analysis.
2. Existing opt-in `XYO_JIT_DAYS_FINITE_SSA=1`: 8 alternating pairs across
   Life/loop-tick/raycast/primes had performance ratios **0.997**, **1.004**,
   **1.008**, **0.972** (respectively). The prime benchmark lost every
   pair, so the flag remains **disabled by default**. Time-budgeted
   scheduler checks are not deterministic: their end-of-run counters cannot
   be required to equal between runs. Evidence:
   `build/multi-bench-20261110/days-finite/ab.json`.
3. Narrow `llvm.loop.unroll.enable` hint: `XYO_JIT_LLVM_REPEAT_HINT=2` tells
   LLVM's own unroll analysis about **compile-time known 2-4 iteration**
   standard `control_repeat` loops. There is **no** Scratch-level instruction
   duplication. Legacy flag values remain: `0`/unset = no hint, `1` =
   previous 2..64 experiment. Source of truth: `src/jit/codegen.cpp`.

## Performance of tiny hint

Same xyo-cpp native binary, i7-12700 CPU8 affinity, eight interleaved
triples cycling `0→2→1` / `1→2→0`. ScratchBench-derived projects, checked
identical final checksums within each triple. Geometric mean of pair ratios:

| Workload | mode 1 vs OFF | new mode 2 vs OFF | mode 2 wins |
|---|---:|---:|---:|
| app_life | 1.0756× | **1.1045×** | 8/8 |
| algo_primes | 1.0410× | **1.0337×** | 6/8 |
| math_mandel | 1.0082× | **1.0140×** | 6/8 |
| algo_sieve | 0.9908× | 1.0021× | 5/8 |
| app_ast | 0.9883× | 0.9920× | 4/8 |
| list_append | 1.0085× | 0.9902× | 5/8 |
| app_raycast | 1.0126× | **0.9739×** | 2/8 |

The full raw results are at `build/multi-bench-20261110/tiny-loop-hint/ab.json`
and reprocessed summary at `summary.json`.

The 8-pair Life increase is robust, but `app_raycast` loses 2.6%. Machine
load fluctuated during measurement. The new mode remains **opt-in**, and
it would be premature to switch the global default.

## Correctness

- Extended `tests/differential/jit_llvm_repeat_hint.py` compares 11 Scratch
  fixtures (0/1/2/3/6/12/31/64/65, fractional and nested) under Interpreter,
  Eager, HotSpot for **all three flag values 0/1/2**. All passed.
- Full ScratchBench GUI parity results are logged at
  `build/multi-bench-20261110/tiny-loop-hint/parity.log` and
  `scratch-bench/results/llvm-tiny-unroll-hint-parity-20261110/`.
  **2656/2656** passed in Interpreter, HotSpot, Eager and AOT.

No commit or push. Other agents' modifications to this shared working tree
were left untouched.
