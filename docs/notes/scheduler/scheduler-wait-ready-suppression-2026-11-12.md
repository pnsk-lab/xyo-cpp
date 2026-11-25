# Experimental Ready Index suppression for unchanged wait-until fibers

Date: 2026-11-12
Repository: xyo-cpp
Status: **strictly opt-in**. Default scheduling is unchanged.

## Feature flag

All three flags must be enabled:

```sh
XYO_WAIT_READY_SLEEP=1 XYO_WAIT_WAKE_REGISTRY=1 XYO_SCHED_READY_INDEX=1
```

This extends the existing Promise-aware Ready Index with an
*experimental* suppression path for native `wait until` fibers. A waiter
is eligible to be omitted from later passes only after its **source and
binding generations** and its saved dependency values are verified
unchanged, the wait is still armed, the Thread is RUNNING, and its
registration has **no pending writer notification**. All other waits,
including unsupported conditions, are conservatively scanned.

## Wake and bounded fallback

- `Runtime::notifyWaitVariableWrite` increments the scheduler mutation
  epoch and records affected Thread identities in
  `pendingVariableWakeups`. On the next scheduler pass,
  `Runtime::snapshotWaitWakeups()` reads these Thread identities without
  draining the audit evidence and restores them to sorted
  `readySlots_`. Mid-pass mutations cause the existing live iteration
  fallback to take over, preserving the scheduler's thread order.
- Every new `Runtime::step` frame forces a full status-index rebuild,
  *including all previously suppressed waiters*, whether or not an
  instrumented write was observed.
- There is also a periodic full rebuild every 256 scheduler passes, even
  for callers that invoke the Sequencer repeatedly without incrementing
  the host frame serial.
- Cached slots are only pruned **after** an entire pass has iterated,
  and only when mutation/structure epochs and thread-vector length still
  match their captured state. Mid-pass writers or broadcasts cannot
  silently exclude a later runnable slot.
- While the ordinary Promise-only Ready Index requires >=32 slots for
  sparse dispatch, this opt-in variant permits >=16 slots so small fanout
  test projects can exercise the implementation.

`Sequencer::waitReadyStats()` exposes deferred-slot operations, sparse
passes, native waiter visits and conservative rechecks. The stress
fixture can emit a compact line with `XYO_WAIT_READY_STATS=1`.

## Safety

This is not a claim of complete write-notification coverage. Some
host/plugin, AOT and WASM stores are not observable by the registry.
**The per-frame/256-pass full rebuild is required for forward progress
despite those missing notifications.** The feature makes no promise that
an uninstrumented host edit is reflected immediately within the same
frame: it may wake on the next frame instead.

The Ready Index itself retains its conservative stale-array and
mutation-epoch fallbacks, and has no effect unless explicitly enabled.
The optimized path does not destroy a native Fiber, mutate Scratch
variables or synthesize a condition result.

## Test coverage

- Existing autonomous 4-writer / 24-waiter native Scratch fixture with
  exact final-state assertions and a differential write-notification
  audit.
- Additional checks: all 24 quiet waiters get excluded, sparse passes
  execute, and each next frame rechecks conservatively.
- New 16-waiter native Scratch project with **no Scratch writer**. A
  host-side direct `Variable::value` edit deliberately bypasses the
  registry after the first exclusion. The next frame must still resume
  every waiter and finish with local `v=1`. All 16 completions are
  classified as **quiet**, because no callback was registered.
- The baseline path can be run without `XYO_WAIT_READY_SLEEP` to ensure
  that ordinary scheduler behavior does not change.

## Benchmarking caution

Counts of `unchangedQuiet` in `WaitNotifyAudit` are snapshot checks,
not CPU cycles. Eliminating these checks only demonstrates reduced
observation/dispatch overhead, not a proportional wall-time improvement.
Compare identical source and flags in paired, CPU-affined A/B runs
before claiming a speedup.

LAYQ adaptive behavior is an unrelated experiment; the unit suite is
normally checked with `XYO_YIELD_QMODE=off` here because a different
danger-time-budget test previously assumed a smaller fixed quantum.

## Verified results — 2026-11-12

The opt-in Ready Index implementation built successfully with LLVM JIT.
The full `xyo-tests` suite passed in **46.63 s** with
`XYO_YIELD_QMODE=off`, plus
`XYO_WAIT_WAKE_REGISTRY=1 XYO_WAIT_READY_SLEEP=1
XYO_SCHED_READY_INDEX=1 XYO_WAIT_NOTIFY_AUDIT=1
XYO_WAIT_MULTIVAR=1 XYO_WAIT_REGISTRY_VERIFY=1
XYO_WAIT_DEADLINE_INDEX=1`.

A direct eager-JIT suite run with `XYO_WAIT_READY_STATS=1` also passed
and reported these *actual* results for the autonomous
4-writer/24-waiter Scratch fanout fixture:

```
[wait-ready-stress] deferred=10608 sparse_passes=112356 waiter_visits=10680 full_rechecks=446
[wait-notify-audit] covered=48 missed=0 unchanged_notified=0 unchanged_quiet=10632 changed_untracked=0 invalid_bindings=0
[wait-resume-audit] same=24/0 exit=24/0 exit_rearmed=0/0 moved=0/0 incomplete=0/0 (notified/quiet)
```

With **the same rebuilt binary** and otherwise identical diagnostic
flags, but no `XYO_WAIT_READY_SLEEP`, the suite passed and the fanout
fixture reported:

```
[wait-notify-audit] covered=48 missed=0 unchanged_notified=0 unchanged_quiet=1136640 changed_untracked=0 invalid_bindings=0
[wait-resume-audit] same=24/0 exit=24/0 exit_rearmed=0/0 moved=0/0 incomplete=0/0 (notified/quiet)
```

This is a 99.06% reduction in unchanged/quiet audit observations in
these runs (1,136,640 -> 10,632). It is **not** a wall-clock or CPU
speedup measurement. The run conditions were not tightly CPU-affined
and the counters include opt-in auditing overhead.

The new unnotified-host-write test deliberately changed a Stage variable
without any registry callback after 16 native waiting scripts had been
suppressed. All 16 later completed successfully after a conservative
frame refresh, with 16 `quiet[Exited]` observations. The synthetic
notification-miss test is still expected to report a miss by design.

The suite reported `scheduler: 474 checks, 0 failures` with the new
fallback assertions. All changes remain uncommitted.

## Default-disabled regression

After the final guard adjustment in the fanout unit test, LLVM JIT tests
rebuilt and the entire xyo-tests CTest target **PASSed (43.02 s)** with
`XYO_YIELD_QMODE=off` and **all** Wait Registry, Ready Index and
wait-suppression experiment flags absent. Thus default scheduling is still
covered by a full-suite run.

New experimental state is confined to `Sequencer` and the auxiliary
non-destructive `Runtime::snapshotWaitWakeups()` API; code generation
and ordinary Scratch interpreter semantics were not modified in this stage.
