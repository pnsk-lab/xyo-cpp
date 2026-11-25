# Mega Scheduler β — post-until timestamp and counter snapshots

2026-11-14 / experimental source-certified path / **default OFF** / uncommitted shared worktree

## Implementation

Added support for exactly two ordered measurement setters that run after a
completed dynamic `repeat until` and before its certified numeric suffix,
inside the previously implemented bounded outer `repeat` continuation.

```text
repeat 3
  set hits to 0
  set b_t0 to daysSince2000()
  repeat until not(((daysSince2000() - b_t0)*86400000) < budget)
    broadcast PING and wait
  end
  set b_t1 to daysSince2000()
  set b_n to hits
  change sum by 10
end
change sum by 100
```

This corresponds to the measurement **prefix** of the real ScratchBench
`sched_pingpong`. In the unchanged real script the inner suffix is
`procedures_call bench_round`, rather than a simple numeric change, so
**the complete original ScratchBench remains ineligible for this Mega lane**.
It is not correct to skip or substitute `bench_round`.

## Source-proof and LLVM execution details

- `src/jit/mega_yield_source.hpp`:
  `hasInnerAfterSets` adds precisely two source-validated setters.
  Each setter can read a finite numeric literal, the zero-argument
  `sensing_dayssince2000` reporter, or one `data_variable` reporter.
  Any other reporter tree is rejected. The original outer-count bound
  (0..64), elapsed-ms condition and fixed joined broadcast rules remain.
  The new proof is separately gated by `XYO_JIT_MEGA_UNTIL_POST_SET=1`.
  An unexpected extra instruction or procedure call after the two
  measurements fails source proof; existing scalar repeat paths are unchanged.
- `src/jit/jit.cpp` and `src/jit/backend_llvm.cpp`:
  Validate both destination variables and any source variable IDs before
  specialization. The new setters are emitted into the existing inner
  `until` condition-true block, BEFORE the inner numeric suffix and BEFORE
  decrementing the outer counter. They cannot execute during a pending
  receiver join. Snapshotting a variable calls `xyo_var_get` then
  `xyo_var_set` using the original tagged `Value` bits. The clock uses
  the same synchronous `xyo_sns_dayssince2000` function as native JIT.
  All variables are read fresh after the join. The Runtime's source
  revision, target binding-generation, receiver identity and GC-root
  protections remain unchanged.
- `tests/unit/test_scheduler.cpp`:
  A real joined receiver updates `hits` and `total`. Assert that
  timestamps and hit snapshots are untouched during the wait; after
  each completed dynamic inner until, they are updated before the
  suffix. Run again with an expired clock budget to exercise
  zero-broadcast inner iterations, which must still produce the
  measurement snapshots. A negative-control `bench_round` non-warp
  procedure following the two setters is rejected by Mega and executes
  through the original Fiber route exactly three times; no side effects
  are discarded.
- `tests/bench/algos/bench_mega_outer_until.py`:
  `--post-measure` option creates a fixed shared .sb3 fixture and checks
  the resulting `b_n == hits` and `b_t1 >= b_t0` as well as the exact
  numeric suffix sum and nonzero receiver counts. Uses alternating
  ON/OFF pairs, CPU16 pinned, one frozen Release binary.

## Configuration

All relevant features remain **off by default**.

```sh
XYO_JIT_MODE=eager
XYO_JIT_MEGA_BATCH_YIELD=1
XYO_JIT_MEGA_SINGLE_SENDER=1
XYO_JIT_MEGA_UNTIL_BCAST=1
XYO_JIT_MEGA_UNTIL_OUTER=1
XYO_JIT_MEGA_UNTIL_POST_SET=1
XYO_JIT_MEGA_NORMALIZE_YIELD=1
```

## Validation

Same final Release binary; existing Mega β flags held constant; only
`XYO_JIT_MEGA_UNTIL_POST_SET` toggled:

| Configuration | Scheduler | E2E |
| --- | ---: | ---: |
| Post-set ON | **815/815 PASS** | **99/99 PASS** |
| Post-set OFF | **803/803 PASS** | **99/99 PASS** |

New/old focused dynamic-until test **121/121 PASS**.
Fiber/GC smoke **3/3 PASS**. Build, Python compilation and
`git diff --check` PASS.

The original extracted `sched_pingpong`, `sched_fanwait` and
`sched_restart` executed successfully with all new flags ON.
None installed an unsupported singleton Mega sender.

## Exploratory 16-pair A/B

Identical frozen Release binary, 18ms per-inner-loop time budget, outer
repeat(5), two post-set snapshots, CPU16 fixed, interleaved 16 ON/OFF pairs.
Every run verified output invariants.

| Metric | Native Fiber | Mega snapshot continuation |
| --- | ---: | ---: |
| Median receptions/process-ms | 327.850 | 307.225 |
| Paired geomean score ratio | — | **0.92300x** |
| Mega wins | — | **1/16** |

Because the inner workload is **time-bounded and performs a different
number of receptions in each run**, this metric is exploratory and cannot
be interpreted as a rigorous fixed-work speedup. This feature does NOT
demonstrate improvement and **should not be enabled by default**.

Raw data:
`build-scheduler-rewrite-20261111/mega-post-until-20261114/bench-16/results.json`

## What's still needed for the unchanged real ScratchBench

Actual `bench_round` contains these effects:

1. `b_ms = (b_t1 - b_t0) * 86400000`
2. `if b_round > 0`, add `b_ms` to `b_total` and `b_n` to `b_chunks`.
3. Within that branch, compare `b_n / b_ms` against `b_bestrate`
   and conditionally save a new best rate.
4. `change b_round by 1`.

After the outer repeat, `b_chk = b_n` and `bench_end` are executed.
A safe direct PC must explicitly preserve those exact Scratch
arithmetic, ordering, conditional and argument semantics. No arbitrary
LLVM shortcut or silent skipping is acceptable. Current full real
ScratchBench uses the native Fiber path.

## Logs and source status

- `build-scheduler-rewrite-20261111/mega-post-until-20261114-final-focused.out`
- `build-scheduler-rewrite-20261111/mega-post-until-20261114-full-final-1.out`
- `build-scheduler-rewrite-20261111/mega-post-until-20261114-full-final-0.out`
- `build-scheduler-rewrite-20261111/mega-post-until-20261114-real-sched_pingpong.err`
- `build-scheduler-rewrite-20261111/mega-post-until-20261114-real-sched_fanwait.err`
- `build-scheduler-rewrite-20261111/mega-post-until-20261114-real-sched_restart.err`

Changes remain uncommitted. Other agents' worktree modifications were
preserved; no reset/stash/commit/push was issued.
