# Ready Index wait-until suppression: CPU-pinned ON/OFF A/B

Date: 2026-11-12
Repository: xyo-cpp
Feature flag: `XYO_WAIT_READY_SLEEP=1` (experimental, OFF by default)

## Setup

Use the same frozen executable and frozen project archive within each
paired experiment. Six pairs, alternating OFF/ON order by pair,
with process affinity pinned to CPU 16. No wait-notification diagnostics
(`XYO_WAIT_NOTIFY_AUDIT` and `XYO_WAIT_REGISTRY_VERIFY` are absent).

Both variants use the following fixed settings:

- `XYO_JIT_MODE=eager`
- `XYO_WAIT_WAKE_REGISTRY=1`
- `XYO_SCHED_READY_INDEX=1`
- `XYO_YIELD_QMODE=off`
- ScratchBench scheduler-only project derived from the same
  `scratch-bench/.cache/build/performance.sb3`, including all eight
  unmodified scheduler kernels.
- ONLY `XYO_WAIT_READY_SLEEP` changes between OFF (0) and ON (1).

Measurements are the ScratchBench-reported `ops/ms` values. Each
"ratio" is the geometric mean of six paired ON/OFF throughput ratios,
not the ratio of displayed median rates.

The tool `tests/bench/algos/bench_scheduler_flags.py` was extended
to support repeated `--common-flag XYO_NAME=value` and `--jit eager`;
it copies a frozen binary and source SB3 snapshot into its result folder
and asserts their hashes remain unchanged throughout measurement.
Without `--jit`, its historical default remains `hotspot`.

## Original wait-ready implementation

Result folder:
`build-scheduler-rewrite-20261111/wait-ready-ab-fullsched-20261112/`

Binary SHA256:
`8f89265cb9559360baf01e2c5a0f042da26872836963cb4af592bcedf75ff1ff`

| Case | OFF median ops/ms | ON median ops/ms | Paired ON/OFF | ON wins |
| --- | ---: | ---: | ---: | ---: |
| sched_looptick | 3575.13 | 3520.02 | 0.9784x | 0/6 |
| sched_pingpong | 429.33 | 424.79 | 0.9883x | 1/6 |
| sched_restart | 1129.98 | 1115.02 | 0.9842x | 2/6 |
| sched_clonechurn | 379.13 | 375.53 | 0.9880x | 1/6 |
| sched_threads | 10446.97 | 10473.72 | 1.0007x | 4/6 |
| sched_fanout | 784.13 | 776.26 | 1.0002x | 2/6 |
| sched_fanwait | 773.20 | 765.34 | 0.9885x | 0/6 |
| **sched_waituntil** | **401.74** | **3681.15** | **9.0539x** | **6/6** |

The `sched_waituntil` case won all six pairs, but the other cases
showed potential 1–2% overhead when the feature was enabled even if
no waiters had actually been suppressed.

## No-wait fast-path refinement

Implementation in `src/engine/sequencer.cpp/.hpp`: track whether the
Ready Index currently has any omitted native wait-until slot
(`hasDeferredWaiters_`). Only when true:

1. consult the reverse registry for pending wakeup identities after
   a scheduler mutation;
2. force a full Ready Index rebuild once per host frame or once every
   256 passes.

A normal non-wait workload no longer acquires the registry mutex or
reconstructs the index solely due to this experimental feature.
After any rebuild, the bit is cleared until a new safe suppression
takes place.

The unnotified host-write fallback unit test was also corrected to
check that the *delta* in rechecks and visited waiters after a direct
write is sufficient, rather than depending on a hardcoded count of
unnecessary full rebuilds. With the full experimental flags enabled,
the entire `xyo-tests` target passed in **46.49 s** after this change.

## Refined measurements (all eight kernels)

Result folder:
`build-scheduler-rewrite-20261111/wait-ready-ab-fullsched-fastguard-20261112/`

Binary SHA256:
`9e3251efc55d235954c16749ff5dd9ad24a65d82b3dd2e5da190640f9a936a56`

Project SHA256 for both full-scheduler sessions:
`682a9818d6191de69a67c2e9de9f789bff93c029df1bede804b63988b8891ed7`

| Case | OFF median ops/ms | ON median ops/ms | Paired ON/OFF | ON wins |
| --- | ---: | ---: | ---: | ---: |
| sched_looptick | 3586.72 | 3557.46 | 0.9921x | 0/6 |
| sched_pingpong | 430.36 | 429.52 | 0.9921x | 2/6 |
| sched_restart | 1118.17 | 1126.51 | 1.0048x | 3/6 |
| sched_clonechurn | 377.02 | 375.90 | 0.9985x | 3/6 |
| sched_threads | 10542.19 | 10421.94 | 0.9926x | 2/6 |
| sched_fanout | 780.49 | 783.29 | 0.9937x | 2/6 |
| sched_fanwait | 777.60 | 778.64 | 0.9993x | 1/6 |
| **sched_waituntil** | **399.96** | **3689.11** | **9.1472x** | **6/6** |

The new binary still produces a large, repeatable throughput improvement
for `sched_waituntil`. The overhead in `sched_fanwait` is nearly zero
within measurement precision. Other non-wait scheduler kernels still
show small fluctuations, generally below 1%; further repetitions,
different CPU cores, and workload validation would be needed to
establish whether they are systematic.

Do not infer that this feature accelerates all Scratch projects by 9x.
The improvement is specific to the wait-until-heavy scheduler
microbenchmark. This is an ops/ms throughput comparison, not a
measurement of total app wall-clock speedup or display frame latency.

## How to reproduce

```sh
python3 tests/bench/algos/bench_scheduler_flags.py \
  --binary build-scheduler-rewrite-20261111/xyo \
  --project scratch-bench/.cache/build/performance.sb3 \
  --out build-scheduler-rewrite-20261111/wait-ready-ab-repro \
  --flag XYO_WAIT_READY_SLEEP \
  --common-flag XYO_WAIT_WAKE_REGISTRY=1 \
  --common-flag XYO_SCHED_READY_INDEX=1 \
  --common-flag XYO_YIELD_QMODE=off \
  --jit eager --cpu 16 --runs 6
```

The script stores each run's stdout/stderr, exact runtime flag set,
binary and project hashes, and `results.json`.

## Safety and interpretation

Only the experimental feature is toggled in the within-binary A/B
measurements; registration, the Promise-aware Ready Index and other
settings are identical. The *two* compiled binaries above were
produced from a shared active worktree; unrelated concurrent edits may
have occurred between rebuilds. The difference between the two sessions
should therefore not be treated as an isolated causal estimate of the
fast-path refinement without checking the complete source diff.

The unit tests separately exercise 24 native waiters and 4 JIT writers
with zero missing notifications, and an intentionally unnotified host
write after 16 waiters have been suppressed. The 16 waiters must still
complete via the next-frame conservative recheck. These tests check
functional behavior; the benchmark runner only verifies completion and
the expected result rows, not arbitrary Scratch semantic parity across
all programs. Full ScratchBench parity and frame latency remain open.

All changes remain uncommitted.
