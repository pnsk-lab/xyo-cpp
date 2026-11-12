# ScratchBench app_life: numeric-string cache fast path for list readers

2026-11-10. `PERF-001`, independent Release build `build-perf-life-20261110/xyo`.
The original ScratchBench project and all checksums were left unchanged.

## Generic JIT optimization

`src/jit/codegen.cpp`: when a `data_itemoflist` reporter is consumed as a
number, try a tagged `SStr` cache read before the generic `toNumber` cascade.
An SStr with `numState!=0` has a published numeric cache. The acquire load of
`numState` pairs with the existing cache publication; state `2` is converted
to zero as Scratch `toNumber` requires. For a non-string or an unparsed SStr,
the original full conversion is emitted in the fallback block. The original
boxed `Value` is preserved and no assumption is made about list contents,
index bounds, benchmark shape or source variable names.

**Enabled by default**. `XYO_JIT_LIST_STRING_NUM_FAST=0` disables the change.
The option is decided during code generation, so there is no environment
lookup in a hot loop. It applies to HotSpot/Eager native JIT and LLVM AOT code generation.
The final Release binary passes full ScratchBench parity with all four modes.

## Controlled experiments

Machine i7-12700, CPU 8 fixed, native HotSpot JIT, same executable, 14
alternating A/B pairs, isolated *unmodified* `app_life` source-derived fixture:

- Old path: median **27.353** diagnostic ops/ms
- Fast path: median **27.8455** diagnostic ops/ms
- Paired geometric-mean speed ratio **1.018034x**; fast path won **14/14**
- Every run: exit 0, DONE printed, checksum **53**.

These diagnostic units are not the official benchmark ops count. Raw logs and
samples: `build/perf-life-string-ab.{log,json}`.

Original unmodified 47-workload ScratchBench, one run per mode with same
Release executable and CPU 0–15 affinity (source hash
`e8bd5be3e7345088`):

| Indicator | OFF | ON |
|---|---:|---:|
| app_life (official ops/ms) | 15,846.68 | 16,157.56 |
| overall weighted score | 110,734 | 111,027 |
| app_life checksum | 53 | 53 |

Original full-suite outputs:
`scratch-bench/results/perf-life-20261110-{off,on}/performance-xyo-cpp.json`.
The aggregate score change and other case changes are only one sample each;
**do not interpret them as causal improvements/regressions**.

Controls, 8 paired runs each on unchanged source-derived workloads, no
checksum mismatches:

| Workload | ON/OFF geometric mean | ON wins |
|---|---:|---:|
| algo_sieve | 1.00067x | 3/8 |
| algo_primes | 1.00080x | 6/8 |
| list_append | 1.00297x | 6/8 |
| app_raycast | 1.00086x | 4/8 |

No clear reproducible loss was found in these targeted controls; their small
measured differences should not be claimed as speedups.

## Correctness

`tests/differential/jit_list_string_numeric_fast.py` runs 14 Scratch projects:
numeric values, numeric strings, booleans, nonnumeric strings, mixed arrays,
'last' and fractional indices, absent/out-of-range entries, list mutations,
clear and other-script modifications. For all 14, Eager and HotSpot with the
feature both ON and OFF agree, and all deterministic cases agree with the
interpreter. All 14 **PASS** using the final Release build.

Full ScratchBench GUI parity passed **2656/2656 (100%)** in all four
modes: Interpreter, HotSpot JIT, Eager JIT, and AOT. Results are in
`scratch-bench/results/perf-life-20261110-parity`.

No commits or pushes. Other agents' uncommitted work was preserved.
