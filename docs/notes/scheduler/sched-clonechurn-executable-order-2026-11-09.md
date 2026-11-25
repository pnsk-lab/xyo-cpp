# ScratchBench clone churn — executable target position fast path (2026-11-09)

## Scope

Continue PERF-001 (goal: beat the fastest of five rivals on all 47 original ScratchBench workloads). Investigate `sched_clonechurn` without changing any workload or its checksum logic, and preserve other agents' shared-tree modifications.

The `clonechurn` fixture is the original source-derived `build/scratchbench-cause-probes/projects/clonechurn.sb3`. It creates 100 clones per iteration. A clone's hat increments the Stage counter then deletes itself. Checksum and elapsed window are **time-dependent**, so they need not be equal between runs; ScratchBench parity, independent block count and final state are the correctness oracles.

## Failed experiments, fully reverted

1. **Do not index clone-start hats in Runtime::threadMap**: `control_start_as_clone` is non-restartable, but its Thread ID was formerly generated while inserting the map entry. Removing that made `Sequencer::stepThreads` cleanup lazily call `Thread::getId()` **after its Target was destroyed**, causing a SIGSEGV. Coredump and backtrace in `build/perf-clone-map-crash.core` (via gdb). The crash was resolved in the experimental path by eliding the now-redundant map erase; yet 16 alternating CPU8 trials showed OFF 835.403 / ON 836.325 ops/ms, only 9/16 ON wins and no reliable gain. All source and test changes from this approach were reverted.
2. **Skip the second Runtime::stopForTarget** after clone disposal: `Target::dispose` already invokes `stopForTarget`. However the experimental opt-in in both interpreter and JIT gave 16 alternating CPU8-pinned trial medians OFF 1021.627 / ON 963.161 ops/ms, ON 7/16 wins with high process variance. Fully reverted experimental edits in `src/blocks/core.cpp` and `src/jit/rt_blocks.cpp`.

Raw: `build/perf-clone-map-fixed-ab.{log,json}` and `build/perf-clone-double-stop-ab.{log,json}`.

## Selected opt-in optimization

`Runtime::addTarget` appends a freshly cloned Target to `executableTargets`. `Target::goBehindOther` locates the desired insertion position, then calls `Runtime::setExecutablePosition`, whose old implementation searches **again** for the just-appended target before erasing. When `executableTargets.back() == target`, an optional `XYO_EXEC_ORDER_POP_LAST=1` branch directly pops the last element instead of repeating an O(n) membership search and vector erase; all other targets use the original find/erase. The exact existing destination clamp and Stage-first invariant remain unchanged. The option is OFF by default. Full-suite validation did not support enabling it.

Modified `src/engine/runtime.cpp` only in `Runtime::setExecutablePosition`. Added `tests/unit/test_scheduler.cpp::testFractionalLayerMovement` tests for moving the last element to the end, clamping it behind Stage when position=0, moving non-last elements, target uniqueness and size preservation.

Frozen initial test executable `build/perf-clone-order-frozen-xyo`, SHA256 `cd16cccba547e5e9484d62c7410d726a12c566bbdc3e7671fe2fa7a00090842f`.

### Same-binary alternating A/B

First CPU8 16 alternating pairs had highly variable throughput (OFF median 644.4085, ON 695.807 ops/ms, 10/16 ON wins, huge variability); insufficient evidence.

Second independent CPU14 (P-core, separate from CPU8), 24 alternating pairs, same binary and original fixture:

| Metric | Legacy OFF | Fast path ON |
|---|---:|---:|
| Median ops/ms | 1050.1825 | **1076.6450** |
| Mean ops/ms | 1045.4153 | **1071.1883** |
| Standard deviation | 17.874 | 16.107 |
| ON paired wins | — | **22/24** |

Thus ON has **+2.52%** median throughput under more stable pinned conditions. CPU14: `build/perf-clone-order-cpu14-ab.{log,json}`; CPU8: `build/perf-clone-order-ab.{log,json}`. Do not attribute differing absolute medians between CPUs to this code change.

### Verification

- Native Release compile `cmake --build build-perf-fan-only-20261108 --target xyo xyo-tests -j4`: PASS.
- Full `xyo-tests` with `XYO_EXEC_ORDER_POP_LAST=1`: **1/1 PASS (43.81 s)**.
- Full `xyo-tests` with `XYO_EXEC_ORDER_POP_LAST=0`: **1/1 PASS (42.47 s)**.
- ScratchBench GUI parity with each flag setting: **2123/2123 (100%)** on both, source suite hash `aeaa86a89eca6afe`.
- Original full 47-case whole-suite ON and OFF, twice in alternated order: see final verdict below.
- `git diff --check` clean.
- No commits or pushes; PERF-001 remains in progress until all 47 workloads exceed stored rivals.

The source tree remains shared with other agents; changes outside this micro-optimization and test belong to their respective owners.

## Final full-suite verdict: leave experiment disabled by default

The original unmodified 47-case suite (source hash e8bd5be3e7345088) was run twice per mode with the same frozen executable, reversing mode order to reduce run-order confounding.

| Run order | Fast path ON score | Legacy OFF score |
|---|---:|---:|
| ON → OFF | 109,704 | 110,573 |
| OFF → ON | 110,177 | 110,395 |

Geometric mean of the two scores: ON about 109,940 vs OFF about 110,484 (~0.49% lower ON). For the 47 individual workloads, the per-workload two-run geometric means give an unweighted overall ON/OFF geometric ratio **0.99530**. The original full-suite `sched_clonechurn` result was **1115.00 ops/ms ON vs 1115.98 OFF** (ratio **0.9991**, i.e. no demonstrated win). The other 46 workloads did not all move in one direction; effects on unrelated workloads should not be attributed to this narrow branch.

Full logs/results:
- `build/perf-clone-order-full-{on,off,on2,off2}.log`
- `scratch-bench/results/perf-clone-order-full-{on,off,on2,off2}-20261109/performance-xyo-cpp.json`
- `scratch-bench/results/perf-clone-order-rivals-20261109.{md,json}`: one ON run wins **31/47** versus older five-rival reports; this is provisional and is not a drop caused by this branch.

**Decision:** preserve the guarded implementation as `XYO_EXEC_ORDER_POP_LAST=1`, **OFF by default**. In isolated P-core CPU14 trials it consistently reduces clone positioning overhead (22/24 pairs, +2.52% median), but it did not demonstrate improvement in the original full suite. Avoid overclaiming its overall impact. No further default changes based on this result.

At this checkpoint, the official PERF-001 target **47/47 is not reached**. Current full-suite snapshot still trails stored rivals most visibly in `sched_clonechurn`, `sched_looptick`, `algo_sieve`, `algo_primes`, `app_life` and other scheduler workloads. No commits or pushes.
