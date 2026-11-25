# app_life: guarded small-modulus fast path (2026-11-10)

## Implementation

`src/jit/codegen.cpp`, `Codegen::emitArithNumeric` (`AOp::Mod`), now defaults
`XYO_JIT_MOD_SMALL_WRAP` to ON; set `XYO_JIT_MOD_SMALL_WRAP=0` to restore the
prior path. Applies to **any** exact positive integral constant divisor
`2 <= m <= 65536`, and dynamically proves:

- `fa > 0` (rejects both positive zero and negative zero, safely retaining
  legacy signed-zero behavior)
- `fa <= 2*m` (a bounded finite value)
- `trunc(fa) == fa` (an exact integer, no speculative assumptions)

On this branch, remainder is `fa>=2*m ? +0 : fa>=m ? fa-m : fa` in f64 SSA.
The `2*m` boundary is explicitly handled (unlike an incorrect single-wrap
rewrite). Both comparisons and subtractions are exact for this domain; no
FP-to-int and int-to-FP conversion, signed integer remainder, or `fmod` call
is needed on this path. The original general arithmetic lowering remains
unchanged on all rejected values: fractional, negative, signed zero, outside
range, NaN/Infinity, etc. The candidate uses new `BasicBlock`s and PHI
inputs, with `modNan=false` in its proven exact-numeric branch.

No source-pattern or workload-name matching is involved. The guard applies
only to a constant positive integer modulus, never to arbitrary dynamic
modulus semantics.

## Measurement

- Same source-derived `life.sb3`, same Release binary, CPU8 pinned, 30fps,
  HotSpot JIT, 14 interleaved OFF/ON pairs, **checksum 53** in every run.
- OFF diagnostic median **27.850 ops/ms**; ON median **30.2625 ops/ms**.
- Paired geometric mean ON/OFF **1.085837**, **14/14 wins** (~8.58%).
- Raw `build/life-codecompare-20261110/smallwrap-ab.{json,log}`.
- Original full ScratchBench 47-workload suite (unchanged source hash
  `e8bd5be3e7345088`), one run per configuration:
  baseline `app_life` **16,157.564 ops/ms**, optimized **17,514.98 ops/ms**
  (**+8.40%**). Baseline overall score 111,027; optimized overall score
  110,542 (single-run other workloads noisy; no aggregate gain established).
  Optimized results:
  `scratch-bench/results/perf-life-smallwrap-20261110/`.
- Baseline comparison reference: MistWarp **22,910.307 ops/ms** (measured
  earlier on unchanged suite): xyo reaches about **76.45%** of MistWarp's
  `app_life` rate, up from ~70.53%. This is not a new MistWarp run.

## Correctness

- Dedicated `tests/differential/jit_mod_small_wrap.py`: 10 divisors
  (`1,2,3,4,7,12,256,65536,65537,2147483647`), 23 inputs for each,
  **230 ordinary Scratch cases**, Interpreter vs Eager JIT vs HotSpot JIT,
  opt-out and opt-in results exactly equal. Cases cover zero, negatives,
  decimals, multiples including `2*m`, overflow, numeric/nonnumeric strings,
  NaN and infinity.
- Full unmodified ScratchBench GUI Parity suite (source hash
  `d64a28bfd605ed0c`) with flag explicitly ON:
  **2656/2656** in Interpreter, Eager JIT, HotSpot JIT, **and AOT**.
  Results `scratch-bench/results/perf-life-smallwrap-parity-20261110/`.
- Rebuilt with default ON and verified flag absent ≈ flag `1` and flag `0`
  restores baseline. Example diagnostic:
  default `30.272`, explicit ON `30.268`, OFF `27.858` ops/ms;
  all checksums `53`.
- Further regression tests: `jit_arith_range.py` passed **20 cases × 4 modes**;
  `jit_induction_ranges.py --quick` passed **12 fixtures**.
- LLVM optimized module confirms **three** `mod.wrap.fast` sites in
  `app_life` procedure (`build/life-codecompare-20261110/smallwrap-optimized-ir.txt`).
  Object dump program size increases from `0x7d41` to `0x8072` bytes;
  common hot-path work decreases despite the cold fallbacks still being emitted.

## Scope and follow-up

This is a **guarded specialization** of constant modulo, not yet a full
rewrite of every loop's carried values into integer LLVM SSA. The latter
would need cross-iteration proofs of type, index ranges, no observable
intervening writes, and safe deoptimization/scheduler resume. The gain here
comes from avoiding conversions and lengthy remainder lowering when values
fall into the guarded hot range, not from removing the existing cold helpers.

The working tree was not committed or pushed; existing other-agent changes
were preserved.
