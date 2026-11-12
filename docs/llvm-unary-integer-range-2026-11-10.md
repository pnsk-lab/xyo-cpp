# LLVM SSA range transfer through Scratch unary math reporters

2026-11-10. Repository: xyo-cpp. Uncommitted experimental working tree.

## Purpose and exact effect

The previous `XYO_JIT_LIST_PROVEN_I32_INDEX` improvement taught LLVM the
exact integer range of a `control_for_each` index. However, an expression
such as `list[round(i)]`, `list[floor(i)]`, or `list[ceiling(i)]` lost the
integer interval at the unary reporter and consequently had to call the
x86-64 `cvttsd2si64` intrinsic and perform 64-bit bounds arithmetic.

`src/jit/codegen.cpp` now records a verified `IntInterval` for the *result*
of `operator_round`, `operator_mathop` floor and ceiling if, and only if,
`intInterval(x)` proved an **immutable exact-integer f64 SSA input** already
bounded in signed-i32 range. Round/floor/ceiling return the same integer
for any such input. `recordIntInterval` conservatively annotates only the
result SSA producer, never a mutable Scratch value or a profile prediction.
The numeric reporters themselves are **not rewritten**: JS Math.round half
ties, signed zero, NaN-box canonicalization, and Scratch comparisons retain
the original lowering. LLVM O3 performs later simplification and induction
analysis. No fast-math flags, source pattern matching, or manual unrolling.

The improvement is **ON by default**. Set `XYO_JIT_UNARY_INTEGER_RANGE=0`
to restore the original unknown-range lowering for side-by-side A/B. This
flag affects only a proven typed interval, not all round/floor calls.

## Concrete post-O3 evidence

`tests/differential/jit_unary_integer_range.py` generates Scratch projects
with a populated list read via `operator_round`, `floor`, `ceiling` or control
variants. For each of these three proven cases:

- Prior IR: **one** real `call i64 @llvm.x86.sse2.cvttsd2si64(...)` in the
  relevant procedure plus 64-bit index correction and bounds check.
- New/default IR: **zero** conversion calls. LLVM can operate on the
  canonical loop induction value and its bounds.
- `abs(i-1200)`, `floor(i/2)`, `ceiling(i/3)` and fractional numeric sources
  do **not** trigger this exact-integer proof. Their existing conversion
  remains and Scratch-visible semantics are preserved.

Captured actual O3 IR:
`build/multi-bench-20261110/unary-int-range/{round,floor,ceil}-{0,1,default}.ll`.

## Measured performance

`tests/bench/algos/bench_jit_unary_integer_range.py` uses ordinary Scratch
procedures with a 150,000-element list. It warms the JIT, invokes the
for-each/list kernel 100 times per measured interval, and records three
inside-project clock intervals per run. 8 randomized paired ON/OFF blocks,
i7-12700 CPU8 pinned, same binary, eager JIT, no changed benchmark data.
The project checksum was **150000** for both modes in every measurement.

| List index | OFF median | ON median | Paired speed ratio (geometric mean) | ON wins |
|---|---:|---:|---:|---:|
| `round(i)` | 22 ms | 20 ms | **1.12665×** | 8/8 |
| `floor(i)` | 22 ms | 20 ms | **1.12569×** | 8/8 |
| `ceiling(i)` | 23 ms | 20 ms | **1.15296×** | 8/8 |
| `abs(i - 150001)` (rejected candidate) | 25 ms | 26 ms | **0.97082×** | 0/8 |

Clock resolution is approximately 1 ms; use eight paired ratios rather than
claiming single-run precision. Raw results:
`build/multi-bench-20261110/unary-int-range/benchmark.{json,log}`.

The `abs` interval transfer was correct but **regressed ~2.9%** on its kernel.
It was removed from the code (not merely disabled by default).

A separate exact-power-of-two division interval transfer for
`ceiling(i/2)`, `floor((i+1)/2)` and `ceiling(i/256)` eliminated the
conversion call in O3 IR but reduced end-to-end speed: geometric means
**0.97444×**, **0.89423×**, and **0.97378×**, respectively, across eight
pairs. It was fully **rolled back** from the code; raw results are in
`build/multi-bench-20261110/unary-int-range/benchmark-pow2.{json,log}`.
This demonstrates why removing an instruction is not sufficient evidence of
runtime speedup: changing the data dependency/branch layout can cost more.

Six-pair alternating measurements of unchanged diagnostic workloads
(`app_life`, `app_raycast`, `algo_primes`, `algo_sieve`, `list_append`,
`math_mandel`) are in `unary-int-range/other-bench-ab.{json,log}`. The
workload-to-workload variance on this shared machine was large; these results
**do not establish an improvement or regression of unrelated workloads**.
There was no checksum mismatch.

## Correctness and scope

- Differential suite: **13 Scratch projects** (round, floor, ceiling,
  negative/zero/large indices, absolute-value controls, integer subtraction,
  fractional rounding, exact and inexact divisions), all matching Interpreter
  under Eager and HotSpot, with the setting explicitly OFF, ON and unset.
  Post-O3 assertions confirm three positive cases and five negative cases.
- Full GUI ScratchBench parity suite (`d64a28bfd605ed0c`), all four modes:
  **2656/2656 PASS** per mode with the new default enabled. Initial result
  path: `scratch-bench/results/llvm-unary-integer-range-parity-20261110/`.
  Final verified build path:
  `scratch-bench/results/llvm-unary-integer-range-final-parity-20261110/`.

Related sources:
- `src/jit/codegen.cpp`
- `tests/differential/jit_unary_integer_range.py`
- `tests/bench/algos/bench_jit_unary_integer_range.py`

No commit/push; other agents' unrelated working-tree edits preserved.
