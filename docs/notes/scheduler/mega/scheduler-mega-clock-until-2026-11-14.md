# Mega Scheduler β — clock-based repeat until continuations (2026-11-14)

## Scope

Implemented an opt-in, source-certified LLVM continuation for a dynamic
`repeat until { broadcast [PING] and wait }` whose reporter is a comparison
of either `daysSince2000()` against a numeric literal/variable or the exact
ScratchBench-style elapsed-milliseconds expression:

```text
not( ((daysSince2000() - b_t0) * 86400000) < b_budget )
```

Both `b_t0` and `b_budget` are fetched fresh from Scratch variables for
every condition evaluation. The clock helper is the same synchronous
`xyo_sns_dayssince2000` used by ordinary JIT. The arithmetic operators
are **exactly** `xyo_sub` then `xyo_mul`; no fast-math or operand
reassociation is introduced. Comparisons use `xyo_lt/gt/eq`, and
`operator_not` negates the result after Scratch's comparison semantics.

The dispatcher uses the original source-certified join list, Thread
identity and return-PC implementation. It handles initial condition
evaluation, completion of a joined receiver, and the final suffix exactly
once.

## Precise safety limits

- Only enabled when `XYO_JIT_MEGA_UNTIL_BCAST=1` together with
  `XYO_JIT_MEGA_BATCH_YIELD=1` and `XYO_JIT_MEGA_SINGLE_SENDER=1`.
- The source must be one green flag hat or a proven non-warp argumentless
  call chain, followed by a single until, a single fixed ASCII
  `broadcast and wait` body, and one constant numeric variable-change
  suffix. The ordinary return-PC stack handles proven caller suffixes.
- One time reporter as a compare operand is eligible. Comparison with
  one Scratch variable or one numeric literal is eligible.
- For elapsed milliseconds, only the exact node sequence
  `operator_multiply(operator_subtract(daysSince2000(), t0), 86400000)`
  compared with a single scalar budget variable is allowed. The
  multiplication constant and subtraction operand order are verified.
- Every referenced Scratch variable must exist before compilation;
  variable bindings and source revisions are checked by existing Mega
  infrastructure on every resumption.
- No arbitrary arithmetic trees, unbounded nested control loops, dynamic
  broadcast names, control_wait, or unsupported procedure arguments
  are accepted. These continue on the normal native Fiber path.
- This is **not** a fast path for the complete actual ScratchBench kernels:
  their green-flag script also contains multiple procedures, nested outer
  repeat loops, setup/measurement/cleanup effects, and other statements.

## Code and tests

- `src/jit/mega_yield_source.hpp`: strict clock and elapsed-ms reporter
  source proof. Records whether the source was certified as clock vs
  literal, clock vs variable, or the exact elapsed-ms expression, and
  preserves operand order.
- `src/jit/jit.cpp` and `src/jit/backend_llvm.cpp`: check all referenced
  variable IDs, call Scratch's exact time/arithmetic/compare helpers
  at the continuation condition PC, and preserve join semantics.
- `tests/unit/test_scheduler.cpp`: real wall-clock budget test
  (budget chosen **after greenFlag/JIT compilation**), actual receiver
  join, no suffix replay, time resampling after join, expired budget
  skipping new broadcasts, the actual ScratchBench SB3 primitive
  variable input encoding `[3,[12,...],[10,...]]`, and rejection of
  an unsupported multiplication factor.

Earlier dynamic-until synthetic A/B for a variable-only condition
(10,000 broadcasts, 16 alternating pairs, frozen binary, CPU16):
normal Fiber median 84.44ms, Mega 87.92ms, paired geomean
0.99372x, Mega wins 3/16. **No demonstrated performance win**.
No runtime performance speedup is claimed for the newly added
wall-clock form: a wall-clock-deadline workload is interpreted
differently from a fixed-work throughput benchmark.

## Test artifacts

- `build-scheduler-rewrite-20261111/mega-until-elapsed-primitive-20261114-focused.out`
- `build-scheduler-rewrite-20261111/mega-until-clock-20261114-full-final-1.out`
- `build-scheduler-rewrite-20261111/mega-until-clock-20261114-full-final-0.out`

Source changes uncommitted. No reset, stash, commit, or push over
other agents' dirty worktree.

## Final validations — same final Release binary

- Source-clock and exact ScratchBench clock-arithmetic focused tests:
  **89/89 PASS** (includes the original scalar comparison tests).
- Full Scheduler Eager regression, all existing Mega beta flags held ON:
  - `XYO_JIT_MEGA_UNTIL_BCAST=1`: **783/783 PASS**.
  - `XYO_JIT_MEGA_UNTIL_BCAST=0`: **694/694 PASS**.
  - E2E on both configurations: **99/99 PASS**.
- Fiber and GC smoke tests: **3/3 PASS**.
- `git diff --check` and Python harness syntax: PASS.
- Executed original extracted ScratchBench `sched_pingpong`,
  `sched_fanwait`, `sched_restart` with source Mega trace enabled.
  All complete successfully. **None installed a singleton Mega
  sender**, because their outer loops/procedure chains do not satisfy
  the certified entry-shape proof. This change therefore does not
  yet improve the actual ScratchBench sched workloads.

Raw files:
- `build-scheduler-rewrite-20261111/mega-until-clock-20261114-full-final-1.out`
- `build-scheduler-rewrite-20261111/mega-until-clock-20261114-full-final-0.out`
- `build-scheduler-rewrite-20261111/mega-until-elapsed-primitive-20261114-focused.out`
- `build-scheduler-rewrite-20261111/mega-until-clock-20261114-real-sched_pingpong.err`
- `build-scheduler-rewrite-20261111/mega-until-clock-20261114-real-sched_fanwait.err`
- `build-scheduler-rewrite-20261111/mega-until-clock-20261114-real-sched_restart.err`

Next engineering step is to preserve the Scratch effect sequence of
the outer `repeat(5)` loop, its pre-loop `data_setvariableto` initializers
and procedure call/return PCs, while reevaluating the exact clock
expression at the inner until entry. Treat this as new source-certified
multi-level continuation state; do not rewrite it as a constant latch or
hoist time reporters across a broadcast join.
