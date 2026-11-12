# Wait-until notification differential audit (2026-11-11)

## Purpose

Before any attempt to drop conditional waiters from the scheduler's normal
scan, compare write notifications recorded by the reverse dependency registry
with the established, conservative wait-dependency snapshot guard. This
phase is **diagnostics only** and does **not** modify thread eligibility,
ordering, skip decisions, or waiting semantics.

Enable with both:
```sh
XYO_WAIT_WAKE_REGISTRY=1 XYO_WAIT_NOTIFY_AUDIT=1 <xyo command>
```

The runtime prints one `[wait-notify-audit]` line on destruction when there
are observations.

## Implementation

At the existing `Sequencer::dispatchReadyThread()` point that already
checks `canSkipEventWaitByDependency`, the opt-in audit independently
evaluates the saved binding/source generation and the variable value snapshot.
Then `Runtime::auditWaitDependencyDecision` looks up whether a notification
was registered for the same Thread identity. The following observation
counters are protected by the existing waitDependencyMutex:

- `covered`: value bits changed, registered watcher has a notification
- `missed`: value bits changed, registered watcher has no notification
- `unchanged_notified`: value bits unchanged, notification present
- `unchanged_quiet`: unchanged and no notification
- `changed_untracked`: value bits changed, no active reverse-index entry
- `invalid_bindings`: source or variable-binding generation changed;
  never dereference the saved Variable* in this case

`missed` is a **candidate write-notification gap**, not proof of a broken
scheduler execution: direct edits to public Variable::value, deliberately
drained wakeup sets, and other unsupported mutation paths can explain it.
`unchanged_notified` is not necessarily a false positive, because the writer
might publish the same value or send an early notification before its store.

This is a shadow **value/binding-change comparison**, not a second run of the
full Scratch condition reporter and not an independent check of whether the
wait predicate became true. The current native wait scheduling decision
remains the source of truth. The audit is opt-in and adds locking and extra
value snapshot checks only in diagnostic mode.

## Tests and preliminary evidence

`tests/unit/test_scheduler.cpp::testWaitNotificationDifferentialAudit`
constructs a monitored scalar and injects these cases: quiet unchanged,
notified unchanged, notified changed, deliberately unnotified changed,
changed with no active registration, and invalid source revision. The
expected counters are `covered=1 missed=1 unchanged_notified=1
unchanged_quiet=2 changed_untracked=1 invalid_bindings=1`.

A JIT-enabled full xyo-tests run with all experimental registry flags
reported, in one actual scheduler fixture,
`covered=4 missed=0 unchanged_quiet=16`.
The synthetic fixture separately reported `covered=1 missed=1`, proving
the injected notification omission was detected. The scheduler checks
reported `409 checks, 0 failures`.

That full xyo-tests process nevertheless had **one failure outside the
scheduler suite** in `test_danger`: the JIT warp-loop time-budget
preemption assertion with `limited counter after 4 passes = 20000`.
The failure's relationship to concurrent LAYQ codegen changes is under
investigation; the full suite cannot be reported as PASS for that run.

## Limits and next steps

- Notification presence is recorded per waiting thread, coalesced since its
  last wait registration. A same-value write can legitimately notify without
  a changed snapshot.
- A diagnostic probe counts observations (not unique writes). A thread may
  contribute to `unchanged_quiet` on multiple passes.
- Native worker writes and variable reads still rely on the engine's existing
  scheduling/ownership rules; this audit does not make arbitrary concurrent
  mutation safe.
- `wait until` is **not removed from the ready scan**.
- Next: separately distinguish a re-evaluated condition that stayed false
  from a wait that exited, using the existing eventWaitSite and exit counter,
  then investigate candidate misses in a real stress workload.

## Final test matrix

- `XYO_WAIT_NOTIFY_AUDIT=1` plus registry/index verifier flags under
  default adaptive LAYQ: `xyo-tests` FAIL at one `test_danger` time-budget
  assertion; the scheduler's 409 checks all PASS. Actual observed scheduler
  audit counters include `covered=4 missed=0 unchanged_quiet=16`.
- No experiment flags under default adaptive LAYQ: same single
  `test_danger` failure (the audit feature is not needed to reproduce it).
- `XYO_YIELD_QMODE=off`, otherwise default: `xyo-tests` PASS (50.00 s).
- `XYO_YIELD_QMODE=off` with the wait-notification audit, multi-variable
  registry, verifier, Ready Index and deadline index enabled:
  `xyo-tests` PASS (63.17 s).

**Interpretation:** `test_danger` requires `limited < 20000` after *four*
`stepThread` passes, while adaptive LAYQ can increase the maximum poll
quantum to 16384 iterations. Completing the 20,000 iterations in up to four
passes can be consistent with an actual budget-driven yield; thus the
failing test alone does not establish a runtime preemption bug. Its
pass-count assumption needs review by the LAYQ owner before updating
semantics or test thresholds. No unrelated LAYQ code was changed here.
