# Wait-until resume outcome audit (2026-11-11)

## Purpose

Combine advisory reverse-index variable-write notifications with the result
of actually resuming a native `wait until` Fiber. This remains a
diagnostic-only feature: no ready-slot membership, script scheduling order,
yield behavior, or Scratch predicate evaluation is changed.

Enable with `XYO_WAIT_NOTIFY_AUDIT=1`; pair it with
`XYO_WAIT_WAKE_REGISTRY=1` to obtain meaningful notified-vs-quiet results.

## Classification

The new pure helper `classifyWaitResume()` in
`src/engine/wait_resume_audit.hpp` compares an initial
`{eventWaitArmed, eventWaitSite, eventWaitExitCount}` snapshot against the
same fields *after* `Sequencer::stepThread()`:

- `RewaitSame`: same wait site and exit count; condition remained false
- `Exited`: exit counter increased and no wait is currently armed
- `ExitedRearmed`: exit counter increased and another wait is armed,
  including reentry at **the same site** during one scheduler slice
- `ChangedSite`: rearmed at a different wait site without an exit signal
- `Incomplete`: wait disarmed with no native completion signal, or exit
  counter reset; e.g., thread termination or a non-native path

This classification does not assume that a disarmed wait implies a true
predicate: only `xyo_wait_event_done()` increments the exit counter.
The classification is not based on arbitrary pointer dereferences.

## Notification correlation

`Runtime::waitNotificationPending(thread)` samples the coalesced per-thread
notification bit *after flushing the parallel work batch*, but
**before** `stepThread()`, because `xyo_wait_event_done()` unregisters
and removes the pending notification. `auditWaitResumeOutcome()` then
increments a separate pair of counters (notified/quiet) for each outcome.
The runtime destructor prints `[wait-resume-audit]` when observations exist.

`XYO_WAIT_NOTIFY_AUDIT` guards the entire new scheduler path. When absent
the runtime incurs no extra per-thread wait notification lookup, mutex lock,
or outcome classification. The existing variable-snapshot differential audit
continues to collect its own `[wait-notify-audit]` counters separately.

## Tests

`tests/unit/test_scheduler.cpp::testWaitResumeOutcomeAudit` tests each
outcome, including completion-and-reentry to the exact same site, and checks
both notified/quiet counter buckets in diagnostic and disabled modes.

LLVM-JIT-enabled `xyo-tests` with registry, multi-variable watcher,
binding verifier, Ready Index, deadline index, notification audit and
`XYO_YIELD_QMODE=off`: **PASS (58.32 sec)**.

The default adaptive LAYQ setting was not used for this validation because
the known `test_danger` four-pass assumption can fail with enlarged yield
quanta; in a previous turn, disabling adaptive LAYQ restored the full suite.

## Limitations

- A "quiet" recheck can be caused by an unsupported external write or a
  drained notification; it is not automatically proof of a missing JIT
  callback. A "notified" recheck may stem from a same-value write.
- `Exited` is evidence of the native wait completion callback. Interpreter
  and WASM paths do not necessarily share that callback.
- Exit and reentry are classified from the native counter and wait state,
  not from running the Scratch predicate a second time.
- One observation counts one resumed wait per scheduling pass; it is not
  a unique-write counter.
- No full ScratchBench parity or throughput A/B benchmark has been run
  for this diagnostic change. Conditional waiters remain in the scheduler
  scan; event-driven readiness is not yet enabled.

## Observed native wait outcomes

Running the built `tests/xyo-tests` directly with the **same**
`XYO_JIT_MODE=eager` setting that CTest supplies, plus the diagnostic
registry/index flags and `XYO_YIELD_QMODE=off`, exited successfully.
The real event-wait fixture reported:

```
[wait-notify-audit] covered=4 missed=0 unchanged_notified=0 unchanged_quiet=16 changed_untracked=0 invalid_bindings=0
[wait-resume-audit] same=0/0 exit=4/0 exit_rearmed=0/0 moved=0/0 incomplete=0/0 (notified/quiet)
```

The artificial classifier unit fixture separately reported one
notified **and** one quiet occurrence in each of the five outcomes;
that fixture is a coverage check, not a measurement of real Scratch waits.
The same direct run printed `scheduler: 416 checks, 0 failures` and
`test_danger: 101 checks, 0 failures`.

An earlier direct launch without `XYO_JIT_MODE=eager` had four unrelated
E2E failures because it did not match the CTest environment; it must not
be used as a parity result. Re-running with the CTest environment passed.
