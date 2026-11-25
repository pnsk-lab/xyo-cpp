# Scheduler integration: autonomous Scratch JIT writer and waiter

Date: 2026-11-12
Repository: xyo-cpp
Status: experimental test only, no scheduler behavior changes.

## Goal

Verify the entire causal chain using the ordinary `Runtime::step()`
scheduler for **both** Scratch scripts, rather than manually stepping the
writer from the test harness. Keep all prior manual-writer tests as a
complementary deterministic diagnostic.

## Fixture

`tests/unit/test_scheduler.cpp` now includes
`testAutonomousNativeWaitWriter()` and a shared
`nativeWaitResumeProject(bool autonomousWriter)` Scratch JSON generator.

The autonomous variant adds a second `event_whenflagclicked` script:

- Waiter: a warp procedure repeats twice, each time waiting until `v>0`,
  then decrementing `v` and incrementing `done`.
- Writer: three Scratch `control_wait` (0.02s) blocks spaced between
  three `data_setvariableto` blocks that write `v=-1,1,1`.

After `rt.greenFlag()`, the test verifies both independent green-flag
threads exist and are native JIT compiled. Only
`rt.runUntilDone(300)` is used to dispatch both scripts. The test
does **not** call `Sequencer::stepThread` directly, start any broadcasts,
mutate `Variable::value`, or inject `notifyWaitVariableWrite` calls.

Assertions check:
- Both native JIT scripts started separately.
- Scheduler terminates without exceeding the step budget.
- Final Scratch state: `done=2`, `v=0`, and no active threads.
- The audited native wait observes at least one **notified**
  `RewaitSame` (false reevaluation), `ExitedRearmed` (true condition,
  same-site reentry), and `Exited` (final true completion).
- Differential audit reports >=3 covered changes, 0 missed changes.

These are stricter than the previous manually controlled writer tests:
they establish ordinary scheduler dispatch while retaining a reproducible
Scratch-only order via timed waits.

## First validation

- LLVM JIT-enabled `xyo-tests` rebuilt successfully.
- With `XYO_YIELD_QMODE=off` and
  `XYO_WAIT_WAKE_REGISTRY=1 XYO_WAIT_NOTIFY_AUDIT=1
  XYO_WAIT_MULTIVAR=1 XYO_WAIT_REGISTRY_VERIFY=1
  XYO_SCHED_READY_INDEX=1 XYO_WAIT_DEADLINE_INDEX=1`,
  CTest `xyo-tests` **PASS (61.09s)**.

`XYO_YIELD_QMODE=off` keeps this scheduler test isolated from the
previously observed LAYQ adaptive time-budget pass-count assertion.

## Limits

- This is one carefully controlled native JIT scenario, not proof of
  notification completeness for every write path or execution backend.
- Runtime and host clocks can make per-pass timing vary. The test asserts
  outcome presence and final state, not exact frame numbers.
- The reverse registry still records *advisory* wakeup candidates only.
  It does not remove wait-until threads from normal scheduler scans.
- The test is not a performance benchmark. No speedup is claimed.

## Observed audit counters

A direct CTest-equivalent eager-JIT run exited with code 0 and reported the
following counters for the *autonomous* fixture (the preceding manually
stepped fixture independently reports the same shape):

```
[wait-notify-audit] covered=3 missed=0 unchanged_notified=0 unchanged_quiet=0 changed_untracked=0 invalid_bindings=0
[wait-resume-audit] same=1/0 exit=1/0 exit_rearmed=1/0 moved=0/0 incomplete=0/0 (notified/quiet)
scheduler: 451 checks, 0 failures
```

The independent synthetic classifier fixture intentionally injects a missed
notification and prints different counters; that is not a runtime regression.
