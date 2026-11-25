# PROF-002: Guard/refusal reasons and runtime Deopt diagnostic logs

## Scope

This follow-up to PROF-001 classifies **compile-time rejection and
speculative numeric entry guards** in the existing opt-in
`XYO_JIT_COMPILE_STATS=1` report. It adds a separate log analyzer for
deoptimization messages already emitted by the runtime.

These are different phenomena:

- `[jit-refusal]`: a JIT compilation or numeric-kernel attachment did not
  start or could not complete. This is **not** a runtime deoptimization.
- `[native-simple-deopt]`: a source-handoff experiment attempted to
  transfer execution to the interpreter; `adopted=0` means it was refused.
- `xyo-common-cfg: deopt`: a numeric-kernel execution failed or its
  source changed, and the interpreter restoration path was invoked.
- `[jit-osr-inflight] deopt`: an opt-in nested OSR path returned to
  the interpreter after a source mutation at a verified yield boundary.

## Usage

```sh
XYO_JIT_COMPILE_STATS=1 ./build/xyo PROJECT.sb3 2>jit.log
bun tools/jit-deopt-analyze.ts jit.log
bun tools/jit-deopt-analyze.ts --json jit.log
```

The first flag enables compile/refusal counters only. The separate runtime
messages require the appropriate diagnostic/experimental flags, such as
`XYO_JIT_VERBOSE` for the numeric-CFG path or
`XYO_JIT_OSR_NESTED_TRACE` for nested OSR. Source handoff diagnostics
only occur on their experimental code path.

**IMPORTANT:** The analyzer counts only *observed diagnostic log lines*.
These represent incomplete coverage and must never be presented as total
runtime deoptimizations or as a side-exit rate. The native handoff refusal
counter in particular is not a completed deoptimization.

## Rejection reasons

| Name | Meaning |
| --- | --- |
| edge_activated_hat | Edge-activated hat is not a supported compile target |
| invalid_source | Scratch source snapshot rejected in synchronous compile |
| sync_backend_failure | Backend returned no native code |
| numeric_rejected_cached | Previously rejected numeric kernel skipped |
| numeric_source_invalid | Numeric source snapshot invalid |
| numeric_not_lowered | Numeric CFG lowering did not produce a kernel |
| numeric_loop_shape | Kernel has unsupported yielding/loop structure |
| numeric_slot_guard | Runtime variable/type guard rejected attachment |
| numeric_backend_failure | Numeric backend returned no runnable code |
| numeric_attach_failure | Numeric kernel attachment itself failed |

Only nonzero reasons are printed. Counters are process-local, atomic, and
updated only if `XYO_JIT_COMPILE_STATS=1`. No native OSR/RT code paths were
edited to add these metrics: `sequencer.cpp`, `rt.cpp`, `codegen.cpp`,
`backend_llvm.cpp`, and `thread.cpp` remain owned by other tasks.

## Validation

Run:

```sh
g++ -std=c++20 -O2 tests/unit/jit_compile_telemetry_smoke.cpp -o /tmp/prof001
bun test tools/jit-deopt-analyze.test.ts
```

The long-term goal is to instrument all actual runtime side exits using a
shared reason-ID ABI, after CallFrame/OSR interfaces stabilize. This
diagnostic analyzer is an intermediate, non-invasive observation tool.
