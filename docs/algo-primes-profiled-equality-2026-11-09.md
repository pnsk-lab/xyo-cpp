# ScratchBench algo_primes: guarded numeric-profile equality cache selection (2026-11-09)

## Goal

Continue PERF-001: exceed the best of MistWarp, TurboWarp, NitroBolt, Scratch Everywhere! and Scratch on **all 47 original ScratchBench performance cases**. This batch targets `algo_primes`, whose inner loop repeatedly tests the Scratch scalar `k_n == 0` and computes integer modulo by another dynamically updated scalar.

All performance comparisons on `algo_primes` use the same unchanged, source-derived `primes.sb3` fixture. ScratchBench source and competitor reports have **not been modified**.

## Diagnosis

The original Scratch routine increments `k_i` from 2 to 601, walks possible divisors `k_j`, checks `k_i % k_j == 0` and sets `k_n = 0` on a hit; its until predicate is `k_j*k_j > k_i || k_n == 0`.

An isolated experiment with the unchanged runtime but altered *diagnostic source clones* (never used for the official performance comparison) found these 8-run medians, ops/ms:

| Diagnostic source | Median |
|---|---:|
| Original `k_n == 0`, `k_i % k_j == 0` | 48,468.38 |
| Equivalent floor/division-based remainder | 53,449.18 |
| Replacing `k_n == 0` with `k_n < 0.5` | 53,138.36 |
| Changing both comparisons to numeric inequalities | 55,263.18 |
| Replacing `k_n == 0` with `!k_n` | 31,119.79 |

All returned the same checksum **110**, but these source rewrites are **diagnostic only** and are NOT proposed as a runtime optimization.

A separate six-repeat, same-binary, CPU8-pinned switch matrix found the largest benefit from disabling an existing optional string-cache optimization:

| JIT flag experiment | ops/ms median |
|---|---:|
| Baseline | 50,548.02 |
| `XYO_JIT_EQ_STRING_CACHE=0` | **58,820.37** (+16.36%) |
| `XYO_JIT_MOD_OUTLINE_WIDE=0` | 46,715.00 |
| `XYO_JIT_MOD_NAN_SPLIT=0` | 48,018.59 |
| `XYO_JIT_MOD_SIGNED_I32=0` | 50,031.34 |
| `XYO_JIT_INDUCTION_RANGES=0` | 50,512.26 |
| `XYO_JIT_INTEGRAL_FACTS=0` | 50,519.21 |
| `XYO_JIT_NUMERIC_UNTIL_SSA=0` | 50,530.31 |

Other tested range/SSA switches were near baseline. All 66 observations retained checksum **110**; no run failed. Raw `build/perf-primes-flags.{json,log}`.

**Root cause:** `Codegen::emitEq` already reuses a promoted numeric-shadow value for numeric literal comparison. It can additionally insert a branch, type check, SStr cache loads and PHIs to read parsed-numeric *string* state when that numeric shadow becomes invalid. Such a string-cache probe helps projects containing dynamically typed numeric strings (e.g. `app_life`), but it is pure overhead for variables that have **only been observed as numbers** by the JIT compile time.

## General fix

In `src/jit/codegen.cpp`, `Codegen::emitNumericLiteralEq` now tests a **direct `data_variable` source field** with `profiledNumber()`. When observed-number-only is true, it skips the *optional string numeric-cache re-probe*. The existing `knownNumOk` shadow-valid test remains active. If a previously numeric Scratch variable **later becomes a string, boolean, color or any other type**, the invalid-shadow branch still calls the full `xyo_eq` helper. No assumption that the variable stays numeric is made, and comparisons preserve their original reporter evaluation order.

The path applies to any numeric-literal equality on a directly referenced scalar, independent of project name, variable ID, numeric literal value or workload shape. It does not globally disable `XYO_JIT_EQ_STRING_CACHE`, protecting applications with string-heavy equality.

The old route is restored with `XYO_JIT_EQ_NUMERIC_PROFILE_CACHE=0`; native default is **enabled**. In AOT/disabled-profile paths `profiledNumber` returns false, preserving the original cache behavior.

A new regression `testNumericVariableBecomesString` in `tests/unit/test_equality_literal.cpp` starts with a numeric variable, warms native compilation, then changes that same variable to multiple spellings of zero, arbitrary strings, booleans, canonical NaN, undefined and color. It verifies both operand orders against `cast::compare` with interpreter and Eager LLVM JIT.

## Controlled performance

Frozen native Release executable `build/perf-primes-profile-frozen-xyo`, SHA256 `49c7b14ce91b8c15b2119bfc18ad2aad948cd24f0dfc1fc60b48a9082eb0fcfe`; i7-12700 CPU8 pinned; 16 alternating paired runs, hotspot native JIT, `--fps 30`, original isolated `primes.sb3`.

| Mode | ops/ms median | Relative | Paired wins |
|---|---:|---:|---:|
| `XYO_JIT_EQ_NUMERIC_PROFILE_CACHE=0` | 50,544.54 | 1.000× | — |
| `XYO_JIT_EQ_NUMERIC_PROFILE_CACHE=1` | **58,714.98** | **1.162×** | 16/16 |

All 32 runs exited successfully and returned the exact **110** checksum. Raw `build/perf-primes-eqprofile-ab.{json,log}`.

A single run each of the **original 47-case performance suite**, same frozen executable, source hash `e8bd5be3e7345088`:

| Metric | OFF | ON |
|---|---:|---:|
| `algo_primes` | 50,959.28 | **59,834.11** (+17.42%) |
| `app_life` | 14,871.10 | **15,976.37** (+7.43%) |
| Whole-suite weighted score | 108,668 | **109,987** (+1.21%) |
| Unweighted 47-case geomean ON/OFF | — | 1.01189× |
| Peak RSS (one run) | 225.1 MB | 235.9 MB |

Results: `scratch-bench/results/perf-primes-profile-{on,off}-20261109/performance-xyo-cpp.json`. Relative to stored *older* rival reports, ON was **30 wins / 17 losses**, `scratch-bench/results/perf-primes-profile-rivals-20261109.{md,json}`. `algo_primes` is still only **0.664×** the saved MistWarp **90,087.43 ops/ms**. Other gaps are substantial. These broad scores are one run each and cannot prove the source change caused performance differences for unrelated cases or RSS.

## Correctness and incident

- `cmake --build build-perf-fan-only-20261108 --target xyo xyo-tests -j4`: PASS.
- `XYO_JIT_EQ_NUMERIC_PROFILE_CACHE=1 ctest ... -R '^xyo-tests$'`: **1/1 PASS (41.66 s)**, including new numeric-to-string mutation test; log `build/perf-primes-eqprofile-ctest-on.log`.
- ScratchBench GUI parity ON: **2123/2123 (100%)**, log `build/perf-primes-profile-parity-on.log` and results directory `scratch-bench/results/perf-primes-profile-parity-on-20261109`.
- Default and explicit OFF unit CTest: `build/perf-primes-profile-ctest-{default,optout}.log` (inspect final log for exact results).
- `git diff --check` clean.

**Rejected alternative:** An experimental direct IEEE bit-zero equality path was tried first. The trial binary sometimes aborted with a bad free/SIGSEGV **even when its opt-in flag was OFF**, and its ON run was much slower. Cause was not established, potentially including concurrently edited shared ABI/build artifacts. The entire trial change was rolled back and the resulting rebuild again exited 0 and returned checksum **110**. No part of that failed optimization was retained.

No commits or pushes. Other agents' in-progress IR, scheduler, renderer and math changes in the shared working tree were preserved. **PERF-001 remains in progress because 47/47 is not met.**

## Final default-enabled release checkpoint

The option was subsequently made **default ON**, with `XYO_JIT_EQ_NUMERIC_PROFILE_CACHE=0` as the full legacy fallback. Rebuilt and frozen native Release binary `build/perf-primes-default-frozen-xyo` has SHA256 `7c11c4ff6fadf03ef8051d9008cedcc60029f8d0b2bff54e4b2564911de0bd23`.

- `ctest --test-dir build-perf-fan-only-20261108 --output-on-failure -R '^xyo-tests$'`: **1/1 PASS (40.48 s)** using default ON, log `build/perf-primes-profile-ctest-default.log`.
- `XYO_JIT_EQ_NUMERIC_PROFILE_CACHE=0 ctest ...`: **1/1 PASS (40.31 s)**, log `build/perf-primes-profile-ctest-optout.log`.
- Default GUI ScratchBench parity: **2123/2123 (100%)**, log `build/perf-primes-default-parity.log` and result `scratch-bench/results/perf-primes-default-parity-20261109/`.
- Default full original 47-case suite (one run): **overall 107,701**; `algo_primes` **58,838.80 ops/ms**; `app_life` **15,898.74 ops/ms**; peak RSS **237.5 MB**. Output: `scratch-bench/results/perf-primes-default-final-20261109/performance-xyo-cpp.json`.
- Relative to stored, not simultaneous, five-rival scores: **31 wins / 16 losses**. `algo_primes` remains **0.653x** saved MistWarp **90,087.43 ops/ms**. Ranked results: `scratch-bench/results/perf-primes-default-rivals-20261109.{md,json}`.

Single-run aggregate score fluctuations are not a controlled estimate of optimization benefit; use the 16 alternating pairs to support the **+16.2%** isolated gain. The task **remains in progress**.
