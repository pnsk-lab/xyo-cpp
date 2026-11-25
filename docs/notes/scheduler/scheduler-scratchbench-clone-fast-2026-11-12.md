# ScratchBench sched_* bottlenecks: clone hat launch optimization

Date: 2026-11-12
Repository: xyo-cpp
Status: experiment OFF by default, uncommitted

## Scope

This work specifically addresses ScratchBench scheduler kernels
(`sched_looptick`, `sched_pingpong`, `sched_restart`,
`sched_clonechurn`, `sched_threads`, `sched_fanout`,
`sched_fanwait`, `sched_waituntil`).
Frame presentation and VM work-time budgets are explicitly NOT part of
the performance claim.

## Diagnosis

`sched_clonechurn` creates and deletes 100 Scratch clones per iteration.
The existing native clone lifecycle profiler
(`XYO_CLONE_LIFECYCLE_PROF=1`) measured in a typical run:

- 164,200 creations and deletions.
- Clone make: total 154.208 ms.
- Clone variable copy: 12.472 ms.
- Clone setup: 8.148 ms.
- **Clone start-as-clone hat dispatch: 120.405 ms** (~78% of make time).
- Clone disposal: 30.001 ms.

The generic `Runtime::startHats("control_start_as_clone", nullptr, clone)`
builds a `HatThreadSnapshot` of unrelated active threads and does a
linear check for each candidate even if the Target is brand-new and
cannot have an existing hat Thread. This is particularly wasteful in
high clone-churn workloads with many concurrent Threads.

## Implementation

- `Runtime::startFreshCloneHats(Target*)` reuses the existing
  cached `Blocks::getRuntimeScripts("control_start_as_clone")`,
  creates the same Threads in the same script order, and initializes
  interpreter hats only after *all* Threads have been spawned.
- A single-hat hot path directly starts the Thread and avoids
  allocating a transient vector of Thread pointers.
- `Target::makeClone()` uses the specialized route only when
  `XYO_CLONE_NEW_HATS_FAST=1` AND
  `Runtime::onTargetCreated` has **no callback**. A callback might
  start threads on the new Target; the generic snapshot path is
  retained for this case. SDL's renderer installs such a callback.
- Without the opt-in flag, code behaves exactly as before.

The initial proposal to skip already completed children in
`broadcast and wait` was independently measured and **reverted**:
it showed only ~+1.3% for `sched_pingpong`, +0.7% for
`sched_fanwait`, and introduced possible event-ordering ambiguity
in other cases. There is no new broadcast join shortcut left.

## Exact benchmark process

Use the unchanged ScratchBench performance.sb3 scheduler kernels,
CPU-affinity pinned to CPU 16, LLVM JIT eager, one frozen native
binary per paired run, alternating OFF/ON, 6 pairs.
Common settings for both variants:

```sh
XYO_YIELD_QMODE=off
XYO_SCHED_READY_INDEX=1
XYO_WAIT_WAKE_REGISTRY=1
XYO_WAIT_READY_SLEEP=1
```

Only `XYO_CLONE_NEW_HATS_FAST` is toggled.
Results use ScratchBench `ops/ms` and geometric mean of
paired per-run ON/OFF throughput ratios.

First implementation (generic multiple-hat vector path):
`build-scheduler-rewrite-20261111/sched-clone-hats-fast-20261112/results.json`

- `sched_clonechurn`: OFF 388.20, ON 419.13 ops/ms,
  paired speed ratio **1.0753x**, ON wins 6/6.
- `sched_fanout`: paired 1.0103x, ON wins 3/6.

After the single-hat no-allocation specialization:
`build-scheduler-rewrite-20261111/sched-clone-hats-fast-single-20261112/results.json`

| Case | OFF median ops/ms | ON median ops/ms | Paired ON/OFF | ON wins |
| --- | ---: | ---: | ---: | ---: |
| sched_looptick | 3535.95 | 3542.11 | 0.9990x | 2/6 |
| sched_pingpong | 429.47 | 431.95 | 0.9969x | 4/6 |
| sched_restart | 1126.18 | 1128.91 | 1.0018x | 4/6 |
| **sched_clonechurn** | **380.22** | **419.75** | **1.0928x** | **6/6** |
| sched_threads | 10497.60 | 10455.01 | 0.9901x | 2/6 |
| sched_fanout | 769.86 | 778.60 | 1.0117x | 5/6 |
| sched_fanwait | 763.83 | 766.67 | 1.0050x | 5/6 |
| sched_waituntil | 3696.13 | 3714.13 | 1.0063x | 5/6 |

The 9.28% improvement is repeatable on clonechurn.
Other non-clone kernel changes are small and include measurement
noise. A broad 9% speedup is *not* claimed.

## Correctness

New `tests/unit/test_scheduler.cpp::testFreshCloneHatLaunchOrder()`
exercises two independent `control_start_as_clone` hats, each of
which increments the Stage variable once. Creating a clone without
a callback must produce exactly two increments and two Threads;
creating another clone with `onTargetCreated` set must take the
conservative fallback and add exactly two more increments.
Existing clone, broadcast, JIT and scheduler tests are retained.

`XYO_YIELD_QMODE=off XYO_CLONE_NEW_HATS_FAST=1
ctest --test-dir build-scheduler-rewrite-20261111
--output-on-failure -R '^xyo-tests$'`: **PASS (56.14s)**.

Because this worktree is shared by multiple tasks and contains
uncommitted work, avoid checking out, resetting or committing unrelated
files.

## Next bottlenecks

- `sched_pingpong` / `sched_fanwait`: joined broadcast
  receiver allocation, Fiber suspend/resume and join-list scanning.
  A broader scheduler/Fiber redesign is required for large gains; no
  unproven event-order-changing shortcut has been enabled.
- `sched_restart` / `sched_fanout`: broadcast receiver
  replacement and thread identity/compiled-script reuse.
- `sched_clonechurn`: further cost is actual Target allocation
  and copy, status/Thread bookkeeping, clone dispatch and disposal.
  Profiling the *remainder* after removing generic start-hat scans
  is needed before modifying it.
- `sched_looptick` measures native time/reporting and JIT loop
  throughput, not necessarily scheduler ready-queue overhead.
- `sched_waituntil` already benefits from the prior opt-in
  dependency-aware Ready Index.

This is not a ScratchBench baseline comparison against TurboWarp;
such a comparison requires matching JVM/host, project, FPS,
OS CPU affinity, and JIT mode separately.

After the final single-hat specialization, the feature-flag OFF
baseline CTest also **PASSed (48.09s)** using `XYO_YIELD_QMODE=off`.
The results are measured using the same final binary for each
ON/OFF pair; all source changes remain uncommitted and the feature
is still opt-in.
