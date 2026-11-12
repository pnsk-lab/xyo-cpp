# PROF-004 — LLVM JIT guard slow-path entry counters

This diagnostic extends PROF-001/002/003 with **native execution counts**
for a curated set of LLVM guard slow-path basic blocks. It does not change
the Scratch interpreter, CallFrame/OSR state, or the semantics of a branch.

## Usage

Build with the PROF-004 source modifications, then:

```sh
XYO_JIT_GUARD_STATS=1 ./build/xyo path/to/project.sb3 2>guard.log
grep '^\[jit-guard\]' guard.log
```

The counter is opt-in: absent or any value other than exactly `1` means
no IR instrumentation is emitted. The presence of
`XYO_JIT_COMPILE_STATS` or `XYO_JIT_DEOPT_STATS` does **not** enable
guard instrumentation.

The report is printed at process shutdown, for example:

```text
[jit-guard] kind=numeric_profile sites=1 hits=2
```

Here `sites` is the number of named pre-optimization LLVM basic blocks
instrumented during this process. LLVM may later remove unreachable code;
`sites` is therefore *not* necessarily the number of machine-code callsites.
`hits` is the number of times generated native execution entered these
blocks and called the counter. It is NOT the number of Deopts: all the
selected branches remain within generated machine code.

To aggregate the results across runs:

```sh
bun tools/jit-guard-analyze.ts --json guard.log
```

This sums category counters without confusing them with the runtime Deopt
events from PROF-003. Interpret output by project/workload, not across
different Scratch programs without normalization.

## Categories

| Label | Exact named LLVM block | Interpretation |
| --- | --- | --- |
| `numeric_profile` | `profile.other` | Speculative numeric value required Scratch coercion |
| `truthy_profile` | `truthy.profile.other` | Profiled boolean fast path fell back |
| `numeric_list` | `list.numeric.fallback` | Numeric-list guard failed; use generic conversion |
| `numeric_equality` | `eq.numeric.slow` | Numeric equality fast path rejected an operand |
| `plain_comparison` | `cmp.plain.scratch` | Direct comparison deferred to Scratch comparison |
| `induction` | `induction.fallback` | Warp-induction fast path not applicable |
| `typed_ssa_cache_miss` | `typed.ssa.convert` | Typed SSA numeric cache miss (not necessarily a Guard failure) |

A name with a numeric LLVM suffix (e.g. `profile.other.1`) is also
recognized, but arbitrary prefix lookalikes are excluded.

## Design

- `src/jit/guard_fallback_instrument.hpp` visits the generated LLVM
  module *after codegen and before the LLVM optimization pipeline*.
- `src/jit/guard_fallback_telemetry.hpp` keeps atomic per-category
  counters with no allocation or printing in the measured native path.
- `src/jit/backend_llvm.cpp` registers the C symbol
  `xyo_jit_guard_slow(uint32_t)` with both eager and lazy ORC engines.
- A synchronous JIT module is instrumented once after `jitEmitScript`.
  Async JIT modules, including generated OSR continuation functions,
  are instrumented once before handing the module to the worker.
- LLVM basic-block names are retained **only when diagnostics are on**
  (or when existing dump/name debug flags request them). In normal builds
  the prior discard-value-names behavior remains unchanged.
- AOT modules and the independent numeric-CFG backend are not
  instrumented.

### Performance / interpretation limits

The opt-in helper introduces one function call and atomic increment
for every selected slow-path entry and retains LLVM value names during
codegen. Consequently, **guard profiling runs are not suitable for
comparing runtime throughput**. Use uninstrumented A/B benchmarks for
performance measurement. The counters are best for identifying which
fast paths are missed and which alternative lowering is worth exploring.

This is not exhaustive. Slow blocks without one of the audited names,
guard failures taken entirely within helper functions, and real
Deoptimization to the interpreter (reported by PROF-003) are distinct.

## Validation

```sh
g++ -std=c++20 -O2 tests/unit/jit_guard_fallback_telemetry_smoke.cpp -o /tmp/guard-t
XYO_JIT_GUARD_STATS=1 /tmp/guard-t
XYO_JIT_GUARD_STATS=0 /tmp/guard-t

# Link the LLVM tests using llvm-config --cxxflags/--libs;
# tests/unit/jit_guard_fallback_instrument_test.cpp checks IR insertion,
# tests/unit/jit_guard_fallback_native_test.cpp checks execution in LLJIT.
```

The PROF-004 development session validated the standalone telemetry,
the LLVM IR insertion test, and the real ORC-generated function
(`profile.other` reached twice: `sites=1 hits=2`) on LLVM 23.
An isolated `backend_llvm.cpp` object compile also passed.
A full xyo-cpp build and Scratch parity check remain separate tasks,
and existing PEN/DLSS concurrent work should not be overwritten.
