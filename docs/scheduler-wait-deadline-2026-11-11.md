# Scheduler timer deadline index (2026-11-11)

## Change

A conservative deadline registry keeps the minimum timeout alongside the existing
FIFO `Runtime::waitQueue`. The opt-in `XYO_WAIT_DEADLINE_INDEX=1` checks
the registry before walking queued timeouts. If at least 16 entries exist and
none is due, the per-tick queue walk is skipped. Actual resolutions retain
the original queue order, bubble updates, cancellation/liveness checks and
`resolveWait` semantics.

All source producers use `Runtime::enqueueWait`; hot-start restoration uses
the same API and clears via `clearWaitQueue`. A size mismatch from an older
direct public deque insertion forces a rescan. **An external edit that changes
an existing deadline without changing the deque size is not observable**:
call `rebuildWaitDeadlineIndex()` after such edits, or leave the flag disabled.
NaN deadlines are treated as immediately due, matching the historical
`deadlineMs > currentMSecs` check.

## Results

LLVM JIT headless `xyo-tests` passed both with
`XYO_WAIT_DEADLINE_INDEX=1 XYO_SCHED_READY_INDEX=1` (56.00 s) and with
both flags absent (58.12 s). These durations are end-to-end suite timings,
**not** a valid benchmark of the index speed. No full ScratchBench parity or
paired performance claim is made.

## Next

Unify all direct thread-status writes into a transition API before enabling
ready-queue exclusion by default. Upgrade timer registry ownership if direct
third-party deque mutation must be fully supported.
