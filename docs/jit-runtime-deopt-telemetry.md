# PROF-003 — Direct runtime deoptimization counters

This experiment extends PROF-001/002 with **actual, instrumented** JIT
deoptimization outcomes. It does not change Scratch execution semantics,
the CallFrame ABI, or any guard decision.

## Enable

```sh
XYO_JIT_DEOPT_STATS=1 ./build/xyo path/to/project.sb3 2>jit.log
bun tools/jit-deopt-analyze.ts --json jit.log
```

The `XYO_JIT_DEOPT_STATS` flag is independent of
`XYO_JIT_COMPILE_STATS`. Without a deoptimization event there is no
`[jit-deopt]` report. The counters are process-local, atomic, and
printed on normal process shutdown.

### Directly instrumented paths

| path | Counted after | Successful outcome |
| --- | --- | --- |
| `native_handoff` | Source handoff capture or adoption attempted | Native continuation accepted into interpreter |
| `numeric_cfg` | Numeric-kernel rollback selected | `restoreNumericInterpreter` succeeded |
| `nested_outer` | Post-yield source proof failed at outer repeat | Outer counted-loop frame restored |
| `nested_inner` | Post-yield source proof failed at inner repeat | Inner counted-loop frame restored |
| `nested_triple` | Post-yield source proof failed at third-level repeat | Inner and deepest loop frames restored |

`attempts` includes **refused** transfers and therefore is not the count
of successful deoptimizations. The report includes `resumed`, `refused`,
and `rate` (= `resumed/attempts`). A refused nested transfer may retire a
thread rather than executing an unsafe continuation.

Numeric-kernel rejection reasons are mapped from
`numericRollbackReasonName`:
`source_changed`, `invalid_state`, `invalid_bindings`,
`backend_failure`, `invalid_continuation`, `commit_conflict`, or
`unspecified_numeric`. Native handoff attempts are marked
`explicit_handoff` or `handoff_refused`. Nested OSR source changes
that pass the resume guards are `source_changed`; failed safe-point
validation is `unsafe_resume`.

Each recorded attempt has one path and one reason. Reason totals and path
totals should therefore match for a complete normal-shutdown report.

### Example output (from the standalone test, not a production benchmark)

```text
[jit-deopt] path=native_handoff attempts=2 resumed=1 refused=1 rate=50.00%
[jit-deopt] path=numeric_cfg attempts=1 resumed=0 refused=1 rate=0.00%
[jit-deopt] path=nested_triple attempts=1 resumed=1 refused=0 rate=100.00%
[jit-deopt] reason=source_changed count=1
[jit-deopt] reason=invalid_bindings count=1
[jit-deopt] reason=handoff_refused count=1
[jit-deopt] reason=explicit_handoff count=1
```

## Interpretation

- The counters only cover these three known runtime deoptimization
  mechanisms (five path labels). **Other LLVM guard fallbacks and side
  exits that remain within compiled code are not counted.**
- The direct counters are separate from existing `XYO_JIT_VERBOSE` and
  `XYO_JIT_OSR_NESTED_TRACE` messages. The log analyzer reports the
  observed diagnostic lines separately to prevent double counting.
- The telemetry runs after the original continuation validation. It
  never authorizes a transfer, changes its target PC, or changes GC roots.
- At default settings, `noteJitDeopt` checks a cached environment flag
  and never creates the counters or writes diagnostic output.
- Compilation/dispatch throughput and fast-path overhead must be
  evaluated separately; deoptimization success rate is not a speedup.

## Validation

```sh
g++ -std=c++20 -Wall -Wextra -Werror -O2 \
  tests/unit/jit_deopt_telemetry_smoke.cpp -o /tmp/prof003
XYO_JIT_DEOPT_STATS=1 /tmp/prof003
XYO_JIT_DEOPT_STATS=0 /tmp/prof003
bun test tools/jit-deopt-analyze.test.ts
```

The 2026-11-09 working tree has concurrent OSR and PEN work. The
independent `src/engine/sequencer.cpp` object compiled successfully
with the project's compile flags, but this does **not** establish a
successful end-to-end executable build or 2125-case parity.
