# app_life further optimization: positive REAL modulo wrap (2026-11-10)

## Summary

Extend the previous guarded constant-divisor remainder shortcut from exact
integers to finite positive **real numbers**, default ON. In
`src/jit/codegen.cpp` → `emitArithNumeric` → `AOp::Mod`, an original `m`
that is an exact positive integer in `[2,65536]` permits the identity:

- For `0 < x < m`: `x % m == x`.
- For `m <= x < 2*m`: `x % m == x-m`.
- For `x == 2*m`: `x % m == +0`.

All of these are exact binary64 operations: `x-m` is exact in the middle
range by **Sterbenz's lemma** (`m <= x <= 2m`), and the positive zero output
at `2*m` is explicit. Thus the integer-only `trunc(x)==x` guard can be
omitted, removing the runtime `vroundsd` comparison and a CFG edge for the
common `app_life` path. The unchanged existing fallback applies to all
negative values, zero (including `-0.0`), nonfinite, and out-of-range
values. No unsafe floating-math assumption or benchmark-name matching.

Flags:

- `XYO_JIT_MOD_SMALL_WRAP=0`: disable the entire small-modulus shortcut.
- `XYO_JIT_MOD_REAL_WRAP=0`: retain previous **integer-only** guarded
  shortcut, for controlled A/B tests.
- Both flags unset: **enable positive-real guarded wrap**, default.

A separate candidate using one `fsub` with a three-way `0/m/2m` selected
subtrahend measured **32.995** versus **33.549** diagnostic ops/ms for the
current two-select formulation; it was rolled back.

## Repeated A/B performance

Original source-derived `life.sb3`, same native executable, i7-12700 CPU8
fixed, HotSpot JIT, 14 alternating pairs, original integer-only shortcut vs
new real shortcut:

- Integer-only median **30.260 ops/ms**.
- Real shortcut median **33.6215 ops/ms**.
- Per-pair geometric-mean ratio **1.1113633 (+11.14%)**; **14/14 wins**.
- Every project checksum remained **53**.
- Before *both* modulo optimizations, integer-only OFF median **27.850**;
  resulting cumulative improvement is approximately **20.7%** for this
  diagnostic metric.

`build/life-codecompare-20261110/real-wrap-ab.{json,log}`.

Official unchanged 47-case ScratchBench source hash `e8bd5be3e7345088`,
CPU0-15 fixed; clean, non-overlapping re-run after GUI Parity finished:

| Source configuration | app_life ops/ms |
|---|---:|
| Previous baseline (small-mod shortcut disabled) | 16,157.564 |
| Guarded integer-only small-mod shortcut | 17,514.983 |
| New default guarded real shortcut, clean run | **19,401.549** |

An earlier concurrent run also gave `19,416.833 ops/ms`, but ran alongside
GUI parity and should not support aggregate comparisons. The clean run gives
**+20.08%** from previous `16,157.564` baseline and about **84.68%** of
previously measured MistWarp `22,910.307 ops/ms` in this benchmark.

The 47-case overall score in the clean run was **110,616** vs prior baseline
**111,027** (single-run aggregate difference: do not claim a verified
regression or improvement without paired repeats). The optimization's
`app_life` improvement is strongly repeatable in the targeted 14 paired runs.

Official output: `scratch-bench/results/perf-life-realwrap-clean-20261110/`.

## Correctness

- Dedicated `tests/differential/jit_mod_real_wrap.py` covers 10 divisors,
  45 inputs per divisor and two expressions per input: **900 Scratch results**.
  This includes positive subnormals, fractional inputs, values immediately
  adjacent to `m` and `2*m` using `nextafter`, negative values, zero,
  infinities, strings, and large values. Interpreter, Eager JIT, and HotSpot
  JIT results were all identical with integer-only and real shortcuts.
- Full original ScratchBench GUI Parity suite source hash
  `d64a28bfd605ed0c`: Interpreter, HotSpot, Eager and AOT all passed
  **2,656/2,656** with `XYO_JIT_MOD_REAL_WRAP=1`.
  `scratch-bench/results/perf-life-realwrap-parity-20261110/`.
- No ScratchBench source was changed. Existing concurrent source edits
  preserved. No git commit or push.

## Further work

`app_life` is still below MistWarp. Target generic loop-carried numeric
representation and index range facts, not merely static external-helper
call counts. Neither the xyo code generator nor the V8 generated code
completely removes all type guards. Full integer SSA requires an explicit
proof or guard with safe fallback at all scheduler handoffs.
