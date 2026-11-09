# PERF-001 — Clone churn lifecycle and pure-wait dependency audit (2026-11-09)

## Goal

Continue improving the unchanged ScratchBench 47-workload native runtime against the best stored MistWarp, TurboWarp, NitroBolt, Scratch Everywhere! and Scratch results. This experiment targets `sched_clonechurn`. Original source-derived project: `build/scratchbench-cause-probes/projects/clonechurn.sb3`. The workload spawns 100 clones at a time, waits until `sc_hits == k_n`, repeats for a time budget, and reports a checksum depending on time elapsed. The time-dependent checksum is **not expected to be identical** across trials.

All isolated A/B were pinned to CPU14 (P-core), using the *same frozen binary* per trial, alternating flag order each pair. No benchmark source or original rival reports were edited.

## Failed experiments, reverted in full

1. **Per-clone Thread slot index**: tried skipping `Runtime::stopForTarget`'s full thread scan for single-spawn clones. Recorded only the first `Runtime::threads` index (not a potentially dangling Thread*); invalid indexes fall back to full scan; any second thread permanently disables optimization. Initially 24 alternating pairs: legacy **1045.04** vs indexed **1050.87 ops/ms**, only **13/24** wins, geometric paired ratio **1.00196**. Updating the index during `Sequencer`'s completed-thread vector compaction and eliding tracking when opt-out still gave **1046.22** vs **1048.64 ops/ms**, **12/24** wins and paired geomean **1.00065**. Reverted all changes from `src/model/target.hpp`, `src/engine/runtime.cpp`, `src/engine/sequencer.cpp`, and the unit-test trial. Raw logs `build/perf-clone-single-index-ab.{json,log}` and `build/perf-clone-index-updated-ab.{json,log}`.

2. **Fresh clone hat duplicate-lookup skip**: `Runtime::startHats` normally builds a lightweight `HatThreadSnapshot` and checks for already-running non-restarting hats. Trial recorded whether a freshly created clone had ever spawned a thread; only its first `control_start_as_clone` dispatch could bypass the lookup, with repeated/reentrant dispatches still checked. Median legacy **1054.59** vs opt-in **1052.92 ops/ms**, **12/24** wins, paired geometric ratio **0.99733**. All source/test changes rolled back. Log `build/perf-clone-fresh-hat-ab.{json,log}`.

3. **Monitor update only when records erased**: `Runtime::requestRemoveMonitorByTargetId` unconditionally marks `monitorState.dirty`, including on empty monitor sets. Trial skipped the dirty mark when no record was removed, preserving pending dirty. Median legacy **1053.29** vs opt-in **1055.85 ops/ms**, only **13/24** wins, geometric paired ratio **1.00598**; no reliable evidence of benefit. Rolled back the runtime and test changes. Log `build/perf-clone-monitor-dirty-ab.{json,log}`.

In the preceding batch, also rejected/reverted omission of non-restartable clone-start hats from `threadMap` after detecting a lazy-ID use-after-free in the experimental path, and an attempted duplicate stop scan optimization that slowed the workload. These are documented in `docs/sched-clonechurn-executable-order-2026-11-09.md`.

## Retained experimental feature: pure wait predicates with multiple scalars

The benchmark's `wait until sc_hits == k_n` condition has **two** distinct scalar dependencies. The existing native `event_wait_dependency.hpp` safely snapshots **one** scalar and its exact `Value::bits`, then skips resuming its waiting Fiber while that value, source revision and target variable-binding generation are unchanged. Multiple-scalar predicates conservatively fell back to ordinary polling.

Added **opt-in** `XYO_WAIT_MULTIVAR=1`, *default OFF*. When enabled, pure source-validated wait-until expressions depending on up to **four distinct scalar variables** may be watched. The first variable retains the original allocation-free single-variable fields. A small optional `Thread::AdditionalWaitDependencies` heap block holds at most three more variable pointers and their raw `Value` bits, only when an actual 2–4-variable pure predicate is armed.

All source and variable binding revision checks occur **before dereferencing any saved Variable***, exactly as for one-variable waits. Any scalar mutation (including a number-to-string transition), target variable-map rebind, Stage reassignment or code revision triggers reevaluation. Unproven/side-effecting/nondeterministic predicates, more than four independent scalar dependencies, unknown/AOT sites, and Emscripten remain in the original path. Optional storage is released from `Thread::releaseFinished`.

Touched: `src/engine/thread.hpp`, `src/engine/thread.cpp`, `src/engine/event_wait_dependency.hpp`. Added `testMultipleVariableWaitDependency()` in `tests/unit/test_scheduler.cpp`, checking opt-out, first/second scalar mutations, string bits, removal/rebinding, source revision, invalid AOT site, and resource release.

### Performance

Frozen native **headless** Release binary `build/perf-clone-multivar-frozen-xyo`, SHA256 `6119472afc3eb4679e4c2181ab4e7130fbe7ec6aab609ba7821960365681cfa6`. CPU14, 24 alternating pairs, original clonechurn probe:

| Mode | Median ops/ms | Paired ON wins | Geometric paired ratio |
|---|---:|---:|---:|
| `XYO_WAIT_MULTIVAR=0` | **1047.519** | — | — |
| `XYO_WAIT_MULTIVAR=1` | 1043.1765 | **11/24** | **0.99561** |

The new path did not improve clonechurn and may introduce minor overhead. It is **not** enabled by default; no claimed performance breakthrough. Raw `build/perf-clone-multivar-ab.{json,log}`.

### Correctness

- Isolated build configured with `-DXYO_SDL2=OFF -DXYO_JIT=ON -DXYO_BUILD_TESTS=ON`; native Release `xyo` and `xyo-tests`: PASS.
- `XYO_WAIT_MULTIVAR=1` CTest `xyo-tests`: **1/1 PASS (41.46s)**, including new two-scalar invalidation regression.
- `XYO_WAIT_MULTIVAR=0` CTest `xyo-tests`: **1/1 PASS (41.50s)**, confirms opt-out fallback remains correct.
- ScratchBench GUI parity ON/OFF: check `build/perf-clone-multivar-parity-{on,off}.log` after completion; headless results must not be compared naively against the earlier SDL build's graphics parity.
- `git diff --check`: PASS.
- Full 47-workload performance suite: not rerun here; these isolated changes have not established a performance gain.

The previously used SDL-enabled shared build was temporarily blocked by an unrelated active renderer edit (`realDlssEnabled_` undeclared in `sdl_renderer.cpp`). To avoid modifying that other agent's file, all new trials used the separate headless build at `build-perf-clone-single-index-20261109`.

No commits, no pushes, and no resets to shared source files owned by other agents. `PERF-001` remains **in progress**: all 47 workloads still need to beat the best of five stored rivals.

## Follow-up

Given repeated negative/indeterminate results from clone shutdown and scheduler-index micro-optimizations, the next measurement should attribute **actual CPU time** to `Target::makeClone`, `Runtime::pushThread`, `Target::dispose`, JIT entry/exit, and thread reclamation with low-overhead aggregation. Prioritize the dominant measured cost rather than adding more special-case branch logic.
