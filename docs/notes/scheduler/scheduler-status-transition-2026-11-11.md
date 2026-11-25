# Scheduler status-transition migration (2026-11-11)

## Implementation

`Runtime::transitionThread(Thread&, int)` now centralizes scheduler-visible status publications. It only increments `schedulerMutationEpoch` when a status actually changes. This avoids redundant ready-index invalidations for repeated publication of the same state.

Converted all in-tree explicit `STATUS_PROMISE_WAIT` stores in core Scratch blocks, audio/music, JIT runtime blocks, translation service and block utility, and converted the `resolveWait` wakeup path. Existing broadcast-restart membership invalidation stays, including same-pointer receiver rearming, which is not always a status change.

The fast intra-Fiber `STATUS_RUNNING`/`STATUS_YIELD` transitions remain direct: they are not yet a complete, audited state machine. The sparse-ready pass remains opt-in (`XYO_SCHED_READY_INDEX=1`), and the deadline index remains opt-in (`XYO_WAIT_DEADLINE_INDEX=1`). Neither is the default scheduling policy.

## Validation and remaining gaps

The xyo-tests suite passed with both indexes enabled after the status API migration (45.42 s). The test includes status transition epoch increments, duplicate no-op behavior and retirement publication. This is **not** an end-to-end ScratchBench parity certification.

Remaining concerns: arbitrary direct `Thread::status` changes by embedded clients, native-Fiber internal yields, extension wakeups, mid-pass mutation and interleaving. Before enabling sparse dispatch by default, every scheduler-visible status transition must be audited and event-wait dependency invalidations must be proven against the sequential baseline.

Do not interpret total test runtime as a performance comparison.
