# Scheduler: transient string variable write notifications (2026-11-11)

## Scope
Experimental native JIT opt-in: `XYO_WAIT_WAKE_REGISTRY=1`. Builds with the
flag absent do not emit the notification helper calls.

## Audit
`emitTransientRepeat` and `emitTransientMultiRepeat` in
`src/jit/codegen.cpp` bypass `emitVarSet` and directly update scalar
variable storage through `xyo_transient_flush` at yield handoffs and
`xyo_transient_finish` at the loop exit. These stores escaped the prior
`emitVarSet` / `storeNumericPair` instrumentation.

## Implementation
Keep the `BlockField*` of each active transient variable alongside its
existing log and storage pointer. After each handoff flush and at each final
publish, call the existing opt-in `emitWaitVariableWrite(field)` helper.
Apply this to both the single-variable and multi-variable optimization.
All callbacks remain after publication and only record candidates in
`Runtime::notifyWaitVariableWrite`; they do not immediately execute
scripts or bypass the established wait dependency validation.

## Verification
JIT-enabled xyo-tests build and full xyo-tests PASS with
`XYO_WAIT_WAKE_REGISTRY=1 XYO_SCHED_READY_INDEX=1
XYO_WAIT_DEADLINE_INDEX=1` (50.59s). The test suite includes the native
callback identity test introduced in the preceding stage.

## Limitations
AOT, WASM, external host/API stores and other unrecognized direct memory
stores are not exhaustively covered. Notifications are advisory only;
`wait until` threads have NOT been removed from the scheduler scan.
The test duration is not evidence of a performance speedup. Full
ScratchBench parity has not been measured for this patch.
