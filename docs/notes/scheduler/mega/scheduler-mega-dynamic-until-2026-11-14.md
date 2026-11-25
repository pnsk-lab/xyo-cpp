# Mega Scheduler β: dynamic repeat-until × broadcast-and-wait

2026-11-14 / source-proven experimental path / default OFF / uncommitted

## Summary

Implemented the first **actual executable LLVM continuation** for a
dynamically checked Scratch loop. A source-proven

```text
when green flag clicked
repeat until (variable > numeric constant)
  broadcast [PING] and wait
end
change [sum] by 10
```

is mapped to a single LLVM function with a Thread-owned continuation PC.
The condition is re-evaluated on entry and **after every completed joined
broadcast**, using the existing Scratch tagged-value comparison helpers.
The source can optionally be wrapped in up to four argumentless, non-warp,
acyclic procedures with existing return-PC suffix rules.

This is NOT a general transformation of arbitrary Scratch loops,
dynamic names, timers or reporter expressions.

## Code changes

- `src/jit/mega_yield_source.hpp`:
  `Source::Kind::BroadcastUntil` with opt-in
  `XYO_JIT_MEGA_UNTIL_BCAST=1`; strict proof for
  `control_repeat_until` and precisely one
  `event_broadcastandwait` in its body, with a required single
  numeric variable-change suffix. The condition can be one
  `operator_gt`, `operator_lt` or `operator_equals` comparing
  `data_variable` to a finite numeric literal, optionally wrapped
  in `operator_not`. Both operand orders are supported. No
  reporter input or multi-block side effects can be carried over a yield.
- `src/jit/jit.cpp`: require the dynamic condition variable exists
  before source selection; recognize BroadcastUntil as an eligible
  **singleton sender** source only with the existing sender opt-in.
- `src/jit/backend_llvm.cpp`: add a shared LLVM compare block that
  the first-entry and resume-after-join switch-PC cases both enter.
  Use `xyo_var_get` + `xyo_gt/lt/eq` (Scratch value semantics,
  not unchecked LLVM floating-point comparisons). When condition is
  false, set the odd PC and hand control back to the C++ runtime to
  launch the message. When condition is true, execute suffix and
  unwind the saved procedure return-PCs. Register broadcast message
  in `MegaDispatchCode::broadcastNames` so Runtime actually launches
  its precise receiver identities.
- `tests/unit/test_scheduler.cpp`: focused
  `XYO_TEST_ONLY_MEGA_UNTIL=1` test cases for all three comparisons,
  operand reversal, `operator_not`, receiver-updated exit condition,
  initial condition true (0 recipient starts), 2-level procedure return
  PCs, second green flag and cached source-specialized LLVM dispatch.
- `tests/bench/algos/bench_mega_single_receiver.py`:
  `--until --optimize until-sender` generates a real receiver-dependent
  dynamic loop and performs alternating ON/OFF tests on identical frozen
  CLI binary, CPU affinity and .sb3 fixture. Every run verifies the
  Stage variables match exactly.

## Runtime semantics

Thread state:
- PC even: not entered; LLVM validates initial until condition.
- PC odd: must join the current launch, or launch one new receiver group.
- `Thread::megaBroadcastPending` and `megaJoinRefs` distinguish
  pending join from a completed iteration.
- Once all receivers finish, the host calls the same LLVM switch PC.
  The comparison sees the **latest variable Value**, which could have
  been changed by a receiver or external effect.
- If condition true, execute suffix once and pop return PC stack.
  If false, keep the odd PC to launch the next broadcast on its next
  scheduled selection.
- `STATUS_YIELD` follows the opt-in
  `XYO_JIT_MEGA_NORMALIZE_YIELD=1` rule; `STATUS_YIELD_TICK` never
  becomes immediately runnable.

The old Fiber path remains unchanged if flags are absent or source proof
does not pass. The GC root and Thread identity set are held by existing
`megaJoinRefs`, not by LLVM stack pointers.

## Focused validations

- Three condition variants, each with one recipient that increments the
  variable from 0→3 and triggers the suffix exactly once: PASS.
- Initially true (counter already 3): no broadcast, suffix once: PASS.
- Source immutable-revision cache reused on second green flag: PASS.
- Two nested argumentless procedures: two return PCs survive the join;
  receiver updates counter to 3; +10 inner suffix, +20/+100
  caller suffixes unwind in LIFO order: PASS.

## Synthetic A/B measurement

CPU 16 pinned, same Release binary, Eager, `XYO_PAR=0`,
`XYO_YIELD_QMODE=off`, `XYO_JIT_MEGA_NORMALIZE_YIELD=1`,
10,000 dynamic-until iterations, each receiver executes `repeat(3)`
before returning; 16 alternating pairs, exact Scratch Stage equality.

| Metric | Original Fiber | New Mega until |
|---|---:|---:|
| Median total elapsed | 84.441 ms | 87.922 ms |
| paired geomean OFF/ON | — | **0.99372×** |
| Mega wins | — | **3/16** |

This is a successful correctness implementation, not a demonstrated
performance win. Do not enable by default.

Raw results:
`build-scheduler-rewrite-20261111/mega-until-20261114/bench-until-10k-16/results.json`

## Uncovered actual ScratchBench

The real `sched_pingpong`, `sched_fanwait` and `sched_restart`
send paths are nested inside the Stage's `suite_sched` procedure and
their own setup/measurement/cleanup code. Each uses
`control_repeat_until` with `sensing_dayssince2000`, numeric
reporter arithmetic, external stopping conditions and other effects.
They are **not** yet eligible for this narrow source-proof.

The next step is not to relax `prove` to accept an arbitrary
reporter. We need exact continuation state for nested loops and
caller frames, a GC-safe re-evaluation of clock/reporters, and a
validated code-generation strategy that preserves every prior Scratch
effect before the first asynchronous broadcast.

## Final verification: comparison/equality and regression

The final test includes a fourth condition (`operator_equals`) in
addition to greater-than, swapped less-than, and `operator_not`.
For all four conditions, the joined receiver increments the test variable,
the LLVM continuation rechecks the latest value, and the loop suffix
executes exactly once. A nested two-procedure, non-tail call also
returns through two saved PCs.

- Focused dynamic-until LLVM test: **51/51 PASS**.
- Full Eager suite with Mega beta flags held constant:
  - `XYO_JIT_MEGA_UNTIL_BCAST=1`: Scheduler **745/745 PASS**,
    E2E **99/99 PASS**, exit 0.
  - `XYO_JIT_MEGA_UNTIL_BCAST=0`: Scheduler **694/694 PASS**,
    E2E **99/99 PASS**, exit 0.
- Fiber and GC smoke: **3/3 PASS**.
- `git diff --check`, Python benchmark syntax, Release rebuild: PASS.
- Original ScratchBench `sched_pingpong`, `sched_fanwait`, `sched_restart`
  were run with the new flag ON plus Mega source trace. None had
  an installed eligible singleton sender LLVM continuation, as
  expected: real source is more complex than this static proof.

Logs:
- `build-scheduler-rewrite-20261111/mega-until-20261114-full-final-1.out`
- `build-scheduler-rewrite-20261111/mega-until-20261114-full-final-0.out`
- `build-scheduler-rewrite-20261111/mega-until-20261114/real-sched_pingpong.err`
- `build-scheduler-rewrite-20261111/mega-until-20261114/real-sched_fanwait.err`
- `build-scheduler-rewrite-20261111/mega-until-20261114/real-sched_restart.err`

Opt-in invocation: set
`XYO_JIT_MODE=eager`, `XYO_JIT_MEGA_BATCH_YIELD=1`,
`XYO_JIT_MEGA_SINGLE_SENDER=1`, `XYO_JIT_MEGA_UNTIL_BCAST=1`,
and `XYO_JIT_MEGA_NORMALIZE_YIELD=1`.

Changes remain **uncommitted**. Other agents' dirty worktree changes
were not reset, stashed, committed, or overwritten.
