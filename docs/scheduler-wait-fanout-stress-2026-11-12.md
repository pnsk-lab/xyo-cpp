# Wait-until fanout stress: 24 native waiters / 4 native writers

Date: 2026-11-12
Status: experimental integration test only.

## Scope

Stress-test the entire Scratch JIT variable-write -> reverse dependency
registry -> ordinary scheduler -> native wait completion chain with
multiple subscriptions to each variable. The earlier two-script fixture
covered one writer and one waiter; this adds fanout and simultaneous
writers to test delivery across independently owned Scratch threads.

Added `testNativeWaitFanoutStress()` in
`tests/unit/test_scheduler.cpp`. The test generates an actual
Scratch project with:

- Four Stage scalars: `s`, `g1`, `g2`, `g3`.
- Four distinct green-flag writer scripts, one per scalar. Each writer
  executes `wait 0.02`, `set gate to -1`, `wait 0.02`,
  `set gate to 1` using Scratch blocks.
- Six independent green-flag sprite scripts per scalar (24 total).
  Each executes `wait until (Stage gate > 0)`, then `set local v to 1`.
- 28 total runnable native JIT scripts.

The test never manually calls `Sequencer::stepThread`, mutates Stage
variables, or issues notifications from the harness. Everything is run
through `Runtime::greenFlag` and `Runtime::runUntilDone(500)`.

## Checked properties

- Project and Stage variable schema load correctly.
- Exactly 28 threads are launched by the green flag and all have native
  JIT code before the scheduler begins executing them.
- All scripts complete within the bounded step count without starvation.
- Every one of four Stage gates ends at 1.
- All 24 independent waiter sprites end with local `v=1`.
- The native wait differential observer reports no missed or untracked
  changed dependencies in this execution.
- At least 24 changed-snapshot observations have associated write
  notifications, and at least 24 native wait completions carry an
  associated notification.

## Verified run

`cmake --build build-scheduler-rewrite-20261111 --target xyo-tests -j4`
compiled. CTest with `XYO_YIELD_QMODE=off` and
`XYO_WAIT_WAKE_REGISTRY=1 XYO_WAIT_NOTIFY_AUDIT=1
XYO_WAIT_MULTIVAR=1 XYO_WAIT_REGISTRY_VERIFY=1
XYO_SCHED_READY_INDEX=1 XYO_WAIT_DEADLINE_INDEX=1`
passed in **66.71 seconds**.

The test retains conservative scheduling. The reverse dependency index
collects advisory wake candidates only; it does not suppress ready-slot
scans yet. This is a correctness test, not a throughput benchmark.

## Open risks

- The test uses cooperative ordering with timed Scratch waits. Thread
  scheduling under genuinely concurrent workers should be tested
  separately when native waiters can be safely parallelized.
- Callback coverage in AOT, WASM and arbitrary host writes is not
  complete. A notification-only ready scheduler must keep a safe
  fallback for unobservable writes.
- Multiple writes can coalesce into a single notification per waiting
  thread. Counts are *resume observations*, not unique writes.
- No speedup is claimed; no A/B benchmark has been conducted.

## Direct eager-JIT diagnostic observations

With all experimental scheduler flags enabled and LAYQ disabled, a
CTest-equivalent direct test run exited successfully and reported the fanout
fixture's counters:

```
[wait-notify-audit] covered=48 missed=0 unchanged_notified=0 unchanged_quiet=925296 changed_untracked=0 invalid_bindings=0
[wait-resume-audit] same=24/0 exit=24/0 exit_rearmed=0/0 moved=0/0 incomplete=0/0 (notified/quiet)
scheduler: 460 checks, 0 failures
```

The 48 covered checks correspond to two changed snapshots per waiter
(24 false re-evaluations and 24 completions). More than 925k unchanged,
quiet snapshot checks occurred while the Stage writers were sleeping. This
is evidence of substantial repeated scanning under this fixture, not a
benchmark of wasted CPU cycles or a projected speedup. The synthetic
notification-omission unit test separately reports `missed=1` by design.

The fanout test also now exercises Scratch's exact final state when the
notification audit/registry flags are disabled; audit-specific assertions
are conditional on those flags. This makes a baseline parity run possible
without adding diagnostics to normal execution.

## Additional isolation and exactly-once assertions

The fixture now also checks that `unchangedNotified == 0` to detect
unnecessary wake notifications in the independently gated Stage groups,
that `notified[Exited] == 24` (rather than merely `>= 24`), and that
`quiet[Exited] == 0`. This guards against duplicate completion counts
and missing notifications on successful wait completion.

A subsequent complete eager-JIT test run with all scheduler diagnostics
reported:

```
[wait-notify-audit] covered=48 missed=0 unchanged_notified=0 unchanged_quiet=935376 changed_untracked=0 invalid_bindings=0
[wait-resume-audit] same=24/0 exit=24/0 exit_rearmed=0/0 moved=0/0 incomplete=0/0 (notified/quiet)
scheduler: 462 checks, 0 failures
```

All tests passed. The unchanged/quiet count varies with scheduler timing;
it counts diagnostic observations rather than unique writes or actual
Scratch predicate evaluations. This confirms that the major remaining
optimization target is reducing redundant *scheduler traversal*, not just
avoiding repeated predicate evaluation.

## Registry-off baseline parity check

The same native Scratch fanout fixture also executes with all
Wait Registry and audit environment variables absent (while
`XYO_YIELD_QMODE=off` remains set to isolate adaptive LAYQ).

`ctest --test-dir build-scheduler-rewrite-20261111 --output-on-failure
-R '^xyo-tests$'`: **PASS (61.22 seconds)**. All 24 waiters reached their
expected exact values and all four Stage gates finished at 1.

Wall times across the diagnostic ON/OFF runs are not a reliable
throughput comparison: the diagnostic mode performs extra locking and
counting, and the machine may have other processes active. A controlled,
paired A/B benchmark is still required to measure any scheduling speedup.
