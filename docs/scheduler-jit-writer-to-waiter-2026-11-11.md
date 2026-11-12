# Scratch JIT writer -> Wait Registry -> native waiter integration (2026-11-11)

## Goal

Eliminate manually injected writes from the previous native wait-resume
fixture, proving the LLVM-compiled *writer* can notify an independently
LLVM-compiled Scratch wait-until script. This adds coverage at the real
JIT variable-store boundary without changing any scheduler decisions.

## Test fixture

`tests/unit/test_scheduler.cpp::testNativeWaitResumeOutcomes()` now loads
one Scratch sprite with three distinct scripts:

- A green-flag handler calls a warp custom procedure. In its two-iteration
  `repeat`, a single `control_wait_until (v > 0)` source block waits,
  then decreases `v` by one and increments `done`.
- A `when I receive NEG` handler executes `set v to -1`.
- A `when I receive POS` handler executes `set v to 1`.

After the native wait Fiber arms, the test triggers the writer through
`Runtime::startHats` and executes that **independent Scratch thread**
through the production `Sequencer::stepThread` method, then resumes
the waiter using the normal `Runtime::step` scheduler path.

Assertions after every trigger verify:

1. The broadcast starts exactly one different Scratch thread.
2. The writer's `Thread::isCompiled` and compiled pointer are set,
   demonstrating native JIT rather than interpreter fallback.
3. The real Scratch assignment commits the expected scalar value.
4. `Runtime::waitNotificationPending(waiter)` is true, demonstrating
   that the *generated LLVM write callback* published its dependency.
5. The native wait's `WaitResumeAudit` classifies the outcomes:
   NEG -> `RewaitSame`; first POS -> `ExitedRearmed` at the
   *same* wait site; second POS -> `Exited`.
6. Final Scratch value `done=2`, no remaining threads.

The test harness triggers broadcasts and controls when one script is
stepped, but **does not mutate the watched variable** or call
`notifyWaitVariableWrite` itself. This intentionally isolates the
writer's JIT store callback from unrelated scheduling order.

## Verification

JIT-enabled `xyo-tests` rebuild completed, and the CTest suite with
`XYO_YIELD_QMODE=off`, plus Wait Registry, notification audit,
multi-variable watchers, binding verifier, Ready Index and deadline
index enabled, passed (**53.31 s**).

The disabled/default scheduler path and other deployment modes are
unchanged: notifications and audits remain opt-in. Adaptive LAYQ was
disabled only because its independent warp-budget unit test had shown
a four-pass assumption mismatch in prior checks.

## Remaining work

A fully autonomous multi-script Scratch program (with broadcasts and
writes driven by Scratch itself rather than the test harness), and
stress tests for writer/reader ordering, same-value stores, variable
rebinding and WASM/AOT stores should follow before any notification-only
ready-queue suppression is enabled.

Do not interpret passing the fixture as proof that **all** variable
writes across all execution backends emit notifications. No
ScratchBench performance A/B was performed.

## Confirmed direct JIT-suite observations

Re-running with the CTest equivalent environment (`XYO_JIT_MODE=eager`,
`XYO_YIELD_QMODE=off`, plus all scheduler experimental flags enabled) exited
successfully and printed:

```
[wait-notify-audit] covered=3 missed=0 unchanged_notified=0 unchanged_quiet=0 changed_untracked=0 invalid_bindings=0
[wait-resume-audit] same=1/0 exit=1/0 exit_rearmed=1/0 moved=0/0 incomplete=0/0 (notified/quiet)
scheduler: 440 checks, 0 failures
test_danger: 101 checks, 0 failures
```

These two audit lines are from the new full Scratch-JIT writer fixture, not
from the separate synthetic classifier fixture or the existing four-wait
integration fixture (which each print their own lines). The `covered=3`
count corresponds to the NEG, POS and repeated POS generated native writes.
No missed notifications were seen in this fixture.

Writer execution is deterministic and manually stepped by the test through
`Sequencer::stepThread`, rather than by normal runtime iteration. **Waiting
and wakeup** use `Runtime::step`. A future test should allow the runtime's
ordinary scheduler to dispatch *both* writer and waiter and compare ordering.
