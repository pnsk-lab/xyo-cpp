# Mega Scheduler β: bounded outer repeat + clock until continuation

2026-11-14 / opt-in feature / uncommitted shared worktree

## Scope

Implemented a real LLVM Thread-owned continuation for this **source-certified** shape:

```text
when green flag clicked
repeat (N = 0..64)
  set [hits] to 0
  set [b_t0] to daysSince2000()
  repeat until (not(((daysSince2000()-b_t0)*86400000) < b_budget))
    broadcast [PING] and wait
  end
  change [sum] by 10
end
change [sum] by 100
```

An optional acyclic, argumentless, non-warp procedure chain with the existing
certified numeric caller return suffixes is supported.

This is an incremental subset of the actual ScratchBench scheduler structure,
not a claim of handling its full program, which additionally has instrumentation
setters, sibling procedures, a bench-round call, and complex outer call stacks.

## Source-proof rules

- `XYO_JIT_MEGA_UNTIL_BCAST=1` and `XYO_JIT_MEGA_UNTIL_OUTER=1`
  are required, as are the standard Mega yield and singleton sender flags.
- The outer control must have a literal integer repeat count between 0 and 64.
  A higher count is rejected because when the inner until is already true,
  every outer iteration executes without a scheduler yield.
- Exactly two setter instructions precede the inner until:
  a constant numeric scalar set, then a scalar set to the exact
  `sensing_dayssince2000` reporter.
- The inner condition must be the certified elapsed-ms reporter with its
  start variable ID matching the second setter. Multiplication constant is
  exactly `86400000`, original operand order is preserved, and the
  comparison budget variable is read anew at every evaluation.
- The inner body must be ONE fixed ASCII `event_broadcastandwait`.
  The inner after-loop suffix and outer after-loop suffix must each be
  a single constant numeric variable change.
- Any other shape, including implicit extra side effects, variable
  reporter initializers, arbitrary arithmetic expressions, dynamic message,
  or nested unknown calls, retains the normal JIT/Fiber execution path.
  Existing scalar `control_repeat` fast paths are not intercepted.

## LLVM continuation state and ordering

No native stack, LLVM SSA or live heap pointer crosses a broadcast yield.
The existing `Thread::megaJoinRefs` stores receiver identities; its GC-root
and membership checks are unchanged.

- Initial even PC pushes statically certified procedure return PCs,
  stores the outer count in `Thread::megaRemaining`.
- If the count is zero, only the outer after-loop suffix is executed.
- Otherwise the LLVM outer-init block runs the literal setter, then calls
  the original synchronous Scratch time reporter, then the native
  `xyo_var_set` helper for the baseline.
- The exact elapsed-ms condition is evaluated from scratch at the
  inner condition block. While false, the dispatcher writes the odd PC
  and returns to C++ for precisely one broadcast-and-wait launch.
- Joined receivers must finish before entering the odd-PC resumed
  comparison block; no intermediate operand is reused across that join.
- When the inner condition becomes true, LLVM commits the inner suffix,
  decrements the Thread-owned **outer** counter, and either runs the
  next outer initializer or completes the outer suffix.
- Finally the original procedure return PCs are unwound LIFO.

For newly emitted outer setters and suffixes, use generic JIT variable
helpers instead of a direct-scalar slot that may belong to the inner suffix.
The implementation deliberately avoids aliasing two distinct variable IDs.

## Regression coverage

New native-focused checks include:

- Outer repeat(3) with a new wall-clock baseline each iteration.
- Real joined receiver that mutates two variables; no suffix replay
  before receivers finish; final sum exactly `130`.
- A second green-flag run with an already-expired budget:
  each new inner until exits immediately, hit counter resets to zero,
  output sum becomes `260`, and no new recipient is launched.
- Outer repeat(0) leaves both initializers untouched and runs only the
  final suffix.
- Outer count 65 does not install an LLVM Mega continuation.
- Wrapping the entire outer loop in one argumentless, non-warp procedure
  preserves the return PC through the join and executes the additional
  caller suffix only after the outer loop: sum `1130`.
- Existing variable-only until, time-reporter, elapsed-ms, inner
  two-procedure return, GC and native Fiber tests remain unchanged.

## Exploratory paired performance

New harness: `tests/bench/algos/bench_mega_outer_until.py`.

Same frozen Release binary, same .sb3 fixture, CPU16 pinned,
Eager JIT, 16 alternating ON/OFF pairs; outer repeat(5) with
18ms-per-inner-loop budget and two numeric writes per receiver.
The harness checks source installation (ON only), final sum `150`,
receiver totals and per-outer hit count.

This workload is **time-bounded rather than fixed-work**.
Each execution can complete a different number of broadcasts.
Its score is the number of receptions divided by total process wall-time
(including startup and LLVM compilation). Use it for exploratory
path comparisons, **not** a definitive fixed-work throughput speedup.

| Metric | Old native Fiber | New outer Mega |
| --- | ---: | ---: |
| Median receptions/process-ms | 353.859 | 327.923 |
| Paired geomean score ratio | — | **0.92195×** |
| Mega score wins | — | **0/16** |

This demonstrates no case for default enablement.
Recorded results:
`build-scheduler-rewrite-20261111/mega-until-outer-20261114/bench-16/results.json`.

## Safety, availability, future work

Feature flags:

```sh
XYO_JIT_MODE=eager
XYO_JIT_MEGA_BATCH_YIELD=1
XYO_JIT_MEGA_SINGLE_SENDER=1
XYO_JIT_MEGA_UNTIL_BCAST=1
XYO_JIT_MEGA_UNTIL_OUTER=1
XYO_JIT_MEGA_NORMALIZE_YIELD=1
```

These features remain **OFF by default**.
Never enable this source path on the full actual ScratchBench
`sched_pingpong` without proving its additional initialization,
after-until instrumentation, bench_round/end procedures, and caller
return flow. A wrong continuation would silently discard user effects.

Next improvement target: instrument exact compiled effect chains in
the real `sched_pingpong` and expand to an explicit sequence of
time/variable setters and procedure calls, with source rev and dynamic
binding validation at each externally visible side effect.

No commit, reset, stash, or push of other agents' changes.


## Final regression and genuine ScratchBench fallback

Same final Release binary with the existing Mega β flags identical
in both runs, toggling ONLY `XYO_JIT_MEGA_UNTIL_OUTER`:

| Configuration | Scheduler | E2E | Exit |
| --- | ---: | ---: | ---: |
| Outer-until ON | **803/803 PASS** | **99/99 PASS** | **0** |
| Outer-until OFF | **783/783 PASS** | **99/99 PASS** | **0** |

Source-proven focused native test: **109/109 PASS**.
Fiber/GC smoke: **3/3 PASS**.
`python3 -m py_compile` (benchmark script) and `git diff --check`: PASS.
Final source was rebuilt successfully with `xyo-tests` and `xyo-cli`.

Final logs:
`build-scheduler-rewrite-20261111/mega-until-outer-full-final-1.out`
`build-scheduler-rewrite-20261111/mega-until-outer-full-final-0.out`
`build-scheduler-rewrite-20261111/mega-until-outer-procedure-focused.out`

Also ran original, unchanged ScratchBench scheduler extracts:
`sched_pingpong`, `sched_fanwait`, `sched_restart`.
All completed successfully; **none installed the new singleton Mega
sender**, as the actual source includes additional effects/call nodes
not accepted by the strict proof. This means the new feature has **not**
yet produced an actual ScratchBench scheduler improvement.

Log locations:
`build-scheduler-rewrite-20261111/mega-until-outer-20261114/real-sched_pingpong.err`
`build-scheduler-rewrite-20261111/mega-until-outer-20261114/real-sched_fanwait.err`
`build-scheduler-rewrite-20261111/mega-until-outer-20261114/real-sched_restart.err`
