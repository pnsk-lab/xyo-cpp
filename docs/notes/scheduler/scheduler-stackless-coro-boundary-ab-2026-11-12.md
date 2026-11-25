# xyo-cpp: stackless LLVM coroutine boundary parity sweep

Date: 2026-11-12
Status: completed (experimental backend remains OFF by default)

## Motivation

The in-tree LLVM ORC stackless path can compile source-certified simple
`repeat N { change s by delta }` scripts into heap-frame continuations.
Check boundary counts and numeric values against exactly the same original
Scratch project executed with the native Fiber backend. Avoid interpreting
"zero Fiber acquisitions" alone as proof of actual LLVM execution.

## New test

`tests/platform/fiberless_coro_boundary_ab.py` runs 12 pairs (24 native
processes), preserving an immutable executable copy. The script derives
each case from `build-scheduler-rewrite-20261111/fiberless-coro/repeat-100k.sb3`
and changes ONLY the loop count and increment literal.

Test parameters:
- Repetition counts: 0, 1, 2, 3, 8, 64, 100.
- Signed increments: -1.
- Other increments: 0, 2, 0.5.
- LLVM JIT eager, O1, `XYO_YIELD_QMODE=off`, CPU16 affinity.
- Same SB3 bytes, same executable, same flags per pair except
  `XYO_JIT_FIBERLESS=0/1`.
- Each run validates exact Scratch final `s`.
- Every stackless run additionally REQUIRES:
  - `[stackless-coro] candidate=1 lowered=1` from the LLVM lowering stage.
  - `interpreted_yield_scripts=0` (no interpreter fallback).
  - `fiber_acquires=0` (no native Fiber acquisition).

## Results

All **24/24** process executions passed, and all **12/12** paired
Fiber-vs-stackless final results matched exactly.

| Repeat count | Change per repeat | Final Scratch s |
| ---: | ---: | ---: |
| 0 | 1 | 0 |
| 1 | 1 | 1 |
| 2 | 1 | 2 |
| 3 | 1 | 3 |
| 8 | 1 | 8 |
| 64 | 1 | 64 |
| 100 | 1 | 100 |
| 1 | -1 | -1 |
| 3 | -1 | -3 |
| 8 | 2 | 16 |
| 8 | 0 | 0 |
| 9 | 0.5 | 4.5 |

Existing isolated, source-edit/restart-sensitive runtime tests:
`XYO_TEST_ONLY_STACKLESS_CORO=1` passed **20 checks, 0 failures**.

Full command to reproduce:

```sh
python3 tests/platform/fiberless_coro_boundary_ab.py \
  --binary build-scheduler-rewrite-20261111/xyo \
  --fixture build-scheduler-rewrite-20261111/fiberless-coro/repeat-100k.sb3 \
  --out build-scheduler-rewrite-20261111/fiberless-boundary-ab-20261112 \
  --cpu 16
```

Raw per-case SB3 archives, frozen executable, SHA256 of every input
and `results.json` are under
`build-scheduler-rewrite-20261111/fiberless-boundary-ab-20261112`.

## Limits

- These are **one basic block plus literal scalar operation** cases,
  not benchmarks of general `sched_*` workloads.
- This validates native LLVM coroutine lowering, not `broadcast and wait`,
  arbitrary reporter expressions, dynamic loops, warp procedures, lists
  or GC ownership across suspension.
- It does not benchmark process throughput; previous isolated
  100k-repeat A/B measured ~1.68x, not a general speedup guarantee.
- `/dev/kvm` is absent from the current development environment.
  KVM-based guest scheduler experiments cannot be executed here.
- Working tree is shared with other agents. This change adds only
  the independent test harness and this document, without resetting,
  rewriting or committing collaborators' edits.
