# Pure wait-until reverse dependency registry (2026-11-11)

## Scope

Experimental opt-in: `XYO_WAIT_WAKE_REGISTRY=1`.

The existing native wait watcher snapshots 1–4 scalar Variable* dependencies
plus their raw value bits, source revision and variable-binding generation.
It already suppresses repeated evaluations of unchanged pure conditions.
Previously it did not support mapping a variable being written back to the
waiting thread identities.

This phase adds an identity-only reverse lookup in Runtime:

- `trackWaitDependencies(Thread&)` replaces the thread's watcher membership
  after a validated native wait snapshot.
- `untrackWaitDependencies(Thread&)` unregisters it on wait completion,
  thread retirement, and finished-thread compaction (including direct DONE
  paths that bypass retireThread).
- `notifyWaitVariableWrite(const Variable&)` coalesces affected Thread*
  identities in a pending set. Writers instrumented this phase: interpreter
  set/change variable, for-each loop counter, native JIT helper set/change.
- `takeWaitWakeups()` drains pending identities for a future event scheduler.
  The runtime's mutation epoch is incremented when a watched variable is
  written, making the existing opt-in Ready Index abandon its cached scan
  within that scheduler pass.

The registry maps pointer identities; a pointer is never dereferenced via
the reverse map. Source edits and variable storage mutation still require the
existing binding-stamp/snapshot validation. A mutex guards the index and
pending wakeup set, because JIT worker helpers may run concurrently.
The default, flag-OFF retirement path avoids taking this mutex.

## Validation

An LLVM-JIT-enabled headless build completed. With
`XYO_WAIT_WAKE_REGISTRY=1 XYO_SCHED_READY_INDEX=1
XYO_WAIT_DEADLINE_INDEX=1`, xyo-tests passed (49.58 s).
Added regression assertions for targeted wakeup, repeated-write
coalescing, untracking, and direct-DONE completion cleanup.

## Limitations

This is a **reverse dependency notification registry, not a complete
event-driven scheduler**. Existing full/sparse scheduler iteration is still
used. Not all native LLVM-optimized scalar stores, host/API writes, or
editor operations are wired to `notifyWaitVariableWrite`; consequently it
would be unsafe to remove all conditional waiters from the ready list on
notifications alone. The registry is intentionally opt-in and does not
claim a measured throughput improvement.

Next step: audit variable writes from generated LLVM IR, integrate the
coalesced wakeup set into eligibility decisions only after all mutation and
binding changes can be observed (or retain conservative polling fallback).
