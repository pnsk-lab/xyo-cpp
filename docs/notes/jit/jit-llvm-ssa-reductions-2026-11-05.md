# LLVM SSA reductions: repeat / for each (2026-11-05)

## Overview

Instead of attempting to optimize an opaque per-iteration Scratch program,
`src/jit/codegen.cpp` statically checks for two side-effect-free arithmetic
patterns and emits canonical integer SSA loops. The regular LLVM O2/O3 loop
pipeline optimizes the loops; we do **not** require a custom ScalarEvolution
transformation. The existing dynamic Scratch path remains available whenever
the proof or runtime guards fail.

### 1. Counted repeat with constant increment

`repeat N { change acc by C }` or the equivalent
`repeat N { set acc to acc + C }` / `set acc to acc - C`, where the body is
exactly one supported arithmetic update and `C` is an integer in [-65536,65536], excluding zero. This is only
active when the existing fixed-literal `repeatIntegerCounter` proof succeeds.
The accumulator is guarded at runtime for Scratch-numeric, integral, and
absolute value <= 2^48. A bounded SSA recurrence is generated for at most
64 iterations. LLVM folds its integral recurrence rather than processing
individual Scratch values. `XYO_JIT_REPEAT_SSA=0` disables this optimization.

### 2. for-each with independent linear sum

`for each i up to N { change acc by i }`, including `change acc by i*C`
(or `C*i`) and equivalent `set acc to acc + i*C` forms, with integral
nonzero `|C| <= 1024`. Requires a single statement
body, the counter to be proven by the existing fixed-literal for-each i32
counter, distinct accumulator and index variables (both id and name checked),
and no side-effecting reporters. The accumulator is guarded the same way.
The SSA induction variable is 1..N, with an integer accumulator recurrence.
`XYO_JIT_FOREACH_SSA=0` disables this optimization.

### Correctness and scheduling

Both optimizations calculate no more than the next original 64-iteration
warp timeout poll. The thread's yield-counter is advanced for all but the
last iteration; `emitLoopYield` checks the remaining iteration. All
Scratch-visible values are stored before the yield. If another thread changes
the accumulator during the yield, a later iteration rechecks the numeric
and range guards and executes the ordinary Scratch loop from its **current
private counter**, without restarting completed iterations. Non-warp mode,
non-integral/unknown values, imprecise large values, and unsupported bodies
always use the regular implementation.

All i64 `add nsw` instructions are protected from signed overflow: accumulator
starts within ±2^48; ≤64 terms × (2^31 index × 1024 factor) keeps every
partial sum within the exactly representable integer range of IEEE f64.
The fixed-count repeat version has the tighter upper bound 64 × 65536.
This prevents reassociation from changing floating-point rounding.

### Regression coverage

`tests/differential/jit_repeat_ssa.py` and `jit_foreach_ssa.py` generate real
Scratch SB3 and compare interpreter, Eager JIT, HotSpot, O2, and the original
per-iteration path. They cover 0, 1, 63, 64, 65, 129, larger counts, fractional
bounds, nonnumeric strings, numeric strings, fractional accumulators, negative
steps, near-2^48 values, rejected extra statements and rejected multipliers.
The current tests do not force a simultaneous write by another thread at
an actual warp-timeout yield; that concurrency edge remains future coverage.
CTest entries: `xyo-jit-repeat-ssa` and `xyo-jit-foreach-ssa`.

### Performance

Four alternating local measurements with the **same binary**, comparing the
optimization toggles ON and OFF (medians, ops/ms). The source of truth is
`build/llvm-scev-loop/paired-ssa.json`.

| ScratchBench | OFF | ON | Speedup | checksum |
|---|---:|---:|---:|---:|
| `loops_foreach` | 1,854,454 | 14,380,363 | **7.76x** | `200010000` |
| `loops_repeat` | 2,223,974 | 18,848,346 | **8.48x** | `20000` |
| `loops_nested` | 2,174,951 | 11,767,209 | **5.41x** | `19600` |
| `loops_until` | 11,588,940 | 11,614,968 | 1.00x | `20000` |
| `loops_while` | 11,591,530 | 11,629,008 | 1.00x | `20000` |
| `app_ast` | 36,577 | 36,952 | 1.01x | `403` |
| `app_brainfuck` | 133,769 | 132,769 | 0.99x | `931125` |

Additional checks:

- Optimized IR dumps with `XYO_JIT_DUMP2=1` contain the fast/slow dispatch
  but no `rep.ssa.header` or `fe.ssa.header` blocks: stock LLVM loop passes
  simplified the internal integer SSA recurrence.
- AOT emit (`xyo aot ... --obj`) and AOT execution (`xyo run ... --aot`)
  pass for `loops_repeat`, `loops_foreach`, `loops_nested`; checksum matches
  the interpreter/JIT: `20000`, `200010000`, `19600`.
- CTest 8 selected tests, including the full `xyo-tests` unit suite, pass.
  Logs: `build/llvm-scev-loop/ssa-ctest.log` and differential test logs.

The numbers reflect steady-state local throughput, not JIT startup cost.
The optimizations are general pattern-based reductions, not keyed to any
ScratchBench name, opcode ID, benchmark size or specific variable names.
