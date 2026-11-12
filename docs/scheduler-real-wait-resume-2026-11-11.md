# Native Scratch wait-until resume outcome: real codegen fixture (2026-11-11)

## Why this is different from the synthetic classifier unit test

The preceding diagnostic phase checked the pure `classifyWaitResume` helper
with fabricated before/after Thread states. It did not prove the LLVM Fiber
could actually leave a wait and re-enter at exactly the same source Block.

This phase adds `testNativeWaitResumeOutcomes` in
`tests/unit/test_scheduler.cpp`. The test loads a genuine generated Scratch
block project (`loadSb3`), starts a green-flag script that calls a warp
custom procedure, and runs a two-iteration `repeat` in that procedure.
Each iteration executes the same `control_wait_until (v > 0)` block, then
changes `v` by -1 and increments `done`.

The test uses deterministic host-side changes to the Scratch variable value
and calls `Runtime::notifyWaitVariableWrite` to simulate an *observable
notified write*. **The writer is not a separate Scratch script** in this
fixture. Importantly, the waiter, its repeat body and its wait-condition
evaluation are compiled Scratch code, not fabricated Thread states.

## Sequence and assertions

1. Start with `v=0`; green flag enters the native warp procedure and
   suspends on false `wait until`. The test confirms
   `thread->isCompiled` and `thread->eventWaitArmed`.
2. Set `v=-1`, publish a write notification and perform a real
   `Runtime::step()`. The native JIT reevaluates the false condition and
   suspends at **the same site**, without incrementing
   `eventWaitExitCount`: `notified[RewaitSame] > 0`.
3. Set `v=1`, publish a notification and step again. The native wait
   completes, the script changes `v` back to 0 and increments `done`.
   The warp procedure then enters the **same wait block in the second
   loop iteration** and yields. The audit sees
   `notified[ExitedRearmed] > 0`; exit count increases while wait site
   and armed state are retained.
4. Set `v=1` once more, publish a notification and run to completion.
   `done=2` and no threads remain.

This case is deliberately stronger than a pure classifier test: it
exercises real `xyo_wait_event_yield` and `xyo_wait_event_done` calls
from LLVM-compiled Scratch code, Fiber suspend/restart, and the same-site
return counter.

## Verification

Build:
```sh
cmake --build build-scheduler-rewrite-20261111 --target xyo-tests -j4
```

Run with matching CTest JIT environment:
```sh
XYO_JIT_MODE=eager XYO_YIELD_QMODE=off \
XYO_WAIT_NOTIFY_AUDIT=1 XYO_WAIT_WAKE_REGISTRY=1 \
XYO_WAIT_MULTIVAR=1 XYO_WAIT_REGISTRY_VERIFY=1 \
XYO_SCHED_READY_INDEX=1 XYO_WAIT_DEADLINE_INDEX=1 \
./build-scheduler-rewrite-20261111/tests/xyo-tests
```

The full suite exited successfully. The native fixture produced:

```
[wait-resume-audit] same=1/0 exit=1/0 exit_rearmed=1/0 moved=0/0 incomplete=0/0 (notified/quiet)
scheduler: 425 checks, 0 failures
test_danger: 101 checks, 0 failures
```

The separate synthetic unit fixture still produced one notified and one quiet
event per category; do not count it as a second independent real Scratch
observation.

This diagnostic mode does **not** influence scheduling eligibility, promote
waiters to runnable slots, or remove `wait until` threads from the scan.
Native LLVM JIT and host-injected mutation with notification were tested;
an independent Scratch writer, interpreter and WASM wakeup parity still need
separate integration tests.

`XYO_YIELD_QMODE=off` is used because the evolving adaptive LAYQ policy
has a separate preemption-assertion incompatibility in `test_danger`.
