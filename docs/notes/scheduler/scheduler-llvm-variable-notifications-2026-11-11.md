# Scheduler: LLVM variable-write notifications (2026-11-11)

## LLVM audit

`emitVarSet` and `emitVarChange` in `src/jit/codegen.cpp` lower
Scratch scalar mutations to native LLVM stores, bypassing
`xyo_var_set`/`xyo_var_change` callbacks. Numeric-promotion
and loop-reduction paths also write through `storeNumericPair`.

## Implementation

An opt-in callback `xyo_wait_notify_var_store(JitCtx*, Variable*)` is
registered in the LLVM native symbol table and forwards to
`Runtime::notifyWaitVariableWrite`. `emitWaitVariableWrite`
emits its call only when `XYO_WAIT_WAKE_REGISTRY=1` and native JIT is
used (not AOT). Thus ordinary native JIT IR is not burdened with an
extra callback.

`emitVarSet` and `emitVarChange` invoke this notification for
ordinary scalar stores. `storeNumericPair` notifies for promoted
numeric writes, including optimized reductions and for-each counters.
Common set/change paths avoid duplicate notifications when they pass
through promoted-store helpers. Parallel-for temporary variable
overrides are intentionally excluded from real-variable notifications.

The existing registry coalesces repeated notifications to one pending
Thread* identity. Notification is performed before the eventual store
in some generated paths; this is safe for the **current** conservative
candidate-only registry, because the notification itself never
resumes a Fiber or removes any candidate from the scheduler.

## Validation

JIT-enabled headless build PASS. With
`XYO_WAIT_WAKE_REGISTRY=1 XYO_SCHED_READY_INDEX=1
XYO_WAIT_DEADLINE_INDEX=1`, `xyo-tests` PASS (50.70 s). A focused
unit assertion invokes the same native C callback and checks that its
Variable* argument maps to the registered waiting Thread.

## Unresolved correctness and performance work

This is still NOT complete write coverage: specialized transient
string paths, other direct variable-address writes, AOT/WASM compiled
writes and arbitrary host mutations require further audit. The
registry remains opt-in, only records wakeup *candidates*, and does
not yet control Ready Queue membership or guarantee exact write
ordering under concurrent native workers.

The unit suite is not a full ScratchBench parity campaign.
No benchmark speedup is claimed; no default scheduler behavior changed.
