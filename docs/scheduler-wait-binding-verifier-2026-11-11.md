# Scheduler: reverse wait-registry binding verifier (2026-11-11)

## Purpose

Protect advisory reverse wait-until subscriptions against external direct
mutations of `Target::variables` and source revisions which bypass
`Target::deleteVariable()`. The previous runtime already invalidated
watchers on normal deletion and clone disposal, but public map edits could
leave stale `Variable*` keys in `variableWaiters`.

## Changes

- `eventWaitBindingSnapshotCurrent(const Thread&)` in
  `src/engine/event_wait_dependency.hpp` compares the cached
  `Blocks::revision()` and `Target::variableBindingStamp()` with
  the snapshot. It deliberately **never dereferences a saved Variable***.
- `canSkipEventWaitByDependency()` uses this binding-only check before
  accessing the scalar value bits. This is the same conservative policy
  previously embedded in that function.
- `Runtime::trackWaitDependencies()` refuses to create reverse
  subscriptions when the snapshot binding or source revision is stale.
- `Runtime::validateWaitDependencyBindings()` walks existing
  subscriptions, removes every reverse index edge of stale watchers,
  invalidates each snapshot, queues advisory wake candidates and invalidates
  the ready-slot cache. Thread identities are never dereferenced via
  cached Variable pointers.
- In `Sequencer::runSchedulerPass()`, set
  `XYO_WAIT_REGISTRY_VERIFY=1` to audit the index before using ready
  slots. This is **diagnostic / opt-in**; the normal scheduler incurs no
  reverse-registry traversal. The registry itself requires
  `XYO_WAIT_WAKE_REGISTRY=1`.

## Tests

Extended `testSingleVariableWaitDependency` to assert:

1. Direct `target.variables.erase("gate")` is detected without
   dereferencing the removed pointer.
2. Stale subscriptions are removed and cause a single conservative
   reevaluation candidate and scheduler cache invalidation.
3. Recreating the variable cannot wake a stale subscription.
4. A fresh snapshot can register on the recreated variable.
5. A subsequent source revision change invalidates the registration.

LLVM JIT-enabled `xyo-tests`, with
`XYO_WAIT_WAKE_REGISTRY=1 XYO_WAIT_MULTIVAR=1
XYO_WAIT_REGISTRY_VERIFY=1 XYO_SCHED_READY_INDEX=1
XYO_WAIT_DEADLINE_INDEX=1`, passed (70.01 sec).

## Safety boundaries

- The verifier assumes owning Thread, Target and Blocks objects are alive;
  these lifetimes are managed by the runtime. Arbitrary memory destruction
  by callers outside the runtime lifecycle is not supported.
- Concurrent unsynchronized external writes to `Target::variables` are not
  made safe by a generation check.
- Direct changes to a variable's *value* with no generation change are
  handled by the existing value comparison when a waiter is examined, not
  by the binding verifier.
- Full event-driven wait-until scheduling is **not enabled**. The opt-in
  verifier is O(number of registered waiters) per scheduler pass, making it
  a safety diagnostic rather than a production optimization.
- Unit suite duration is not a throughput benchmark. Full ScratchBench
  parity has not been established by this change.
