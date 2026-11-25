# ScratchBench sched_*: per-OS-thread Fiber pool experiment

Date: 2026-11-12
Repository: xyo-cpp
Status: opt-in, OFF by default, uncommitted

## Hypothesis

The original `FiberPool::acquire()` and `FiberPool::release()` take
`gFiberPoolMtx` for every JIT Fiber activation and retirement, even
when all activity is on one OS thread. The global fallback pool is
correct, but it adds lock/unlock traffic to repeated broadcast and
restart scheduler kernels.

## Implementation

`src/engine/fiber.cpp` adds `XYO_FIBER_LOCAL_POOL=1`.

- Each OS thread caches up to 16 idle `Fiber*` objects.
- When a cached Fiber is available, reset and lease it without taking
  `gFiberPoolMtx`.
- Completed or abandoned Fibers return to the local pool first; if
  full, fall back to the existing shared pool.
- A thread-local destructor frees remaining idle stacks when the OS
  thread exits, bounding retained mappings per thread.
- Existing `activeCount`, `unsafeCount`, ASan handling, stack
  guards, pool reset, and global fallback are preserved.
- With the flag unset, the normal global-pool route remains active.
  Do **not** enable by default without broader workload validation.

## Measurements

Same frozen binary and scheduler source for each A/B pair.
ScratchBench performance.sb3 kernels unmodified.

Fixed: eager JIT, CPU affinity 16,
`XYO_CLONE_NEW_HATS_FAST=1`, `XYO_YIELD_QMODE=off`,
`XYO_SCHED_READY_INDEX=1`,
`XYO_WAIT_WAKE_REGISTRY=1`,
`XYO_WAIT_READY_SLEEP=1`.
Only `XYO_FIBER_LOCAL_POOL` toggles.

Whole eight-kernel suite, 6 alternating pairs:
`build-scheduler-rewrite-20261111/sched-fiber-local-pool-20261112/results.json`

| Case | Paired ON/OFF throughput ratio | ON wins |
| --- | ---: | ---: |
| sched_looptick | 1.0336x | 5/6 |
| sched_pingpong | 1.0117x | 4/6 |
| sched_restart | 1.0405x | 5/6 |
| sched_clonechurn | 1.0359x | 5/6 |
| sched_threads | 1.0008x | 2/6 |
| sched_fanout | 1.0131x | 5/6 |
| sched_fanwait | 1.0616x | 4/6 |
| sched_waituntil | 1.0096x | 3/6 |

This six-pair run had notable extraneous CPU load and large
within-run fluctuations. Its apparent fanwait improvement was NOT
reproduced in the isolated targeted suite below.

Targeted `sched_pingpong`, `sched_fanwait`,
`sched_restart` run, **8 alternating pairs**:
`build-scheduler-rewrite-20261111/sched-fiber-local-pool-targeted-20261112/results.json`

| Case | OFF median (ops/ms) | ON median (ops/ms) | Paired ON/OFF | ON wins |
| --- | ---: | ---: | ---: | ---: |
| sched_pingpong | 431.795 | 431.519 | 0.9933x | 3/8 |
| sched_fanwait | 825.937 | 816.151 | 0.9938x | 2/8 |
| **sched_restart** | **1121.269** | **1161.622** | **1.0370x** | **8/8** |

Interpretation:

- `sched_restart` benefits reproducibly (~3.7%).
- No convincing improvement for `sched_pingpong` or
  `sched_fanwait`. Large gains will require a more substantial
  scheduling design improvement than merely removing a pool mutex.
- Other differences in the full-suite run are too sensitive to
  noise or run order for strong causal claims.
- The test remains OFF by default to avoid extra 16 MiB/thread
  idle-stack retention on multithreaded hosts.

## Correctness

```sh
XYO_FIBER_LOCAL_POOL=1 XYO_CLONE_NEW_HATS_FAST=1 \
XYO_YIELD_QMODE=off \
ctest --test-dir build-scheduler-rewrite-20261111 \
  --output-on-failure -R '^xyo-tests$'
```

PASSED (44.05 s), all tests.

The concurrent active worktree contains many unrelated edits.
Do not reset, commit or rebase other tasks when working on this
experiment.

## Next actions

For `pingpong` and `fanwait`, profile the JIT broadcast join
boundary and native Fiber resume counts separately from receiver
startup and retirement. A previous shortcut to skip completed
receiver membership checks yielded only about 1% and was reverted.
Avoid reordering immediate receiver execution or dropping join
identities: both break the Scratch broadcast-wait semantics.
