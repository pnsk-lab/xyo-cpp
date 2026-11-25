# ScratchBench clone churn optimization — 2026-11-08

## Goal and scope

Continue **PERF-001**: xyo-cpp should eventually exceed MistWarp, TurboWarp, NitroBolt, Scratch Everywhere! and Scratch on each of 47 ScratchBench performance workloads. This batch investigates `sched_clonechurn`. The result is a small compiler scheduling improvement; **47/47 remains unachieved**.

The ScratchBench workload creates 100 clones of a hidden sprite per iteration. Each clone increments a Stage scalar and deletes itself. The test source was not modified. Added `clonechurn` as a selectable diagnostic fixture in `tests/bench/algos/scratchbench_probe.py`.

## Root cause

`JitEngine::accessFor()` stores an `AccessSet` by (compiled top `Block*`, runtime `Target*`). A freshly created clone has a different Target pointer each time, so even a source script containing `control_delete_this_clone` (which dependency analysis already correctly marks with `AccessSet::spawn` as **worker-unsafe**) was repeatedly analyzed and inserted in the per-Target cache.

The compiled helper `xyo_ctl_createclone` or `xyo_ctl_deleteclone` always mutates scheduler/clone state. Such a script is necessarily caller-thread-only, irrespective of its Target-specific data dependencies. Marking it `CompiledScript::mainThread=true` at code generation skips `parallelAccessFor` and avoids repeated dynamic per-clone dependency analysis and cache insertions while preserving the existing serial execution order.

**Implemented** in `src/jit/codegen.cpp` at helper declaration: mark a script that calls `xyo_ctl_createclone` or `xyo_ctl_deleteclone` as caller-affine. The change applies to all Scratch scripts using either opcode, **not** a benchmark project name or a special operand value. Now enabled by default, with `XYO_CLONE_SERIAL_CODEGEN=0` retaining the previous analysis path.

**Regression** in `tests/unit/test_scheduler.cpp`: a synthetic real Scratch clone-hat containing `control_delete_this_clone` is compiled with LLVM, and the resulting `CompiledScript::mainThread` is checked for both default/ON and OFF configurations.

## Paired A/B measurements

Host i7-12700, same native Release binary, 30fps, isolated unmodified `clonechurn.sb3`, 12 alternating pairs (one process per case). Rates are `sched_clonechurn` ops/ms medians:

| Variant | ops/ms median | Relative |
|---|---:|---:|
| `XYO_CLONE_SERIAL_CODEGEN=0` | 1,049.45 | 1.000× |
| `XYO_CLONE_SERIAL_CODEGEN=1` | **1,078.38** | **1.0276×** |

All twelve paired comparisons favored the static caller-affinity hint, though the gain is modest. The workload counts events in a fixed time window, so checksums naturally vary and should **not** be compared for exact equality. Raw `build/perf-clone-serial-ab.json`.

Whole 47-workload single-run comparisons on the same executable:
- ON: `scratch-bench/results/perf-clone-serial-on-20261108/performance-xyo-cpp.json`: clonechurn **1,174.01** ops/ms, overall **100,513**.
- OFF: `scratch-bench/results/perf-clone-serial-off-20261108/performance-xyo-cpp.json`: clonechurn **1,148.51** ops/ms, overall **101,585**.
- Clonechurn alone improved by **2.2%** in the full suite. The full-suite overall **did not improve** in this single run, and other workloads varied by -10% to +2% due to clock/load/test-order effects. Do not ascribe them to this narrow clone change.
- Five-rival ranking of the ON run: `scratch-bench/results/perf-clone-serial-rivals-20261108.{json,md}`: **28/47 wins, 19/47 losses** against saved earlier data. Clonechurn is **0.615×** the saved Scratch Everywhere! score (**1,907.96** ops/ms). **All-rival superiority is not reached.**
- Rivals are saved results from different times, so these are provisional.

## Explored and rejected optimization paths

- Fresh clone initial hat start previously called `HatThreadSnapshot::contains` even though no script had yet run. An opt-in shortcut was benchmarked with 12 paired trials: **1,061.17 → 1,048.08 ops/ms**, a slight regression. Rolled back; no new default dispatch path.
- Reusable Thread pool cap and reclamation slack were made temporarily configurable to test 2048/8192/32768/65536 capacities. The best measured median was only ~1.6% above baseline, and high caps gave no consistent advantage. The temporary knobs were removed and the default **2048** remains.
- Temporary `XYO_CLONE_PROFILE` lifecycle timers showed ~330k clones per 0.4s test. Sample inclusive times: `make_clone` ~173ms, `push_thread` ~142ms, `stop_for_target` ~12ms, `reclaim_threads` ~86ms. These numbers are **instrumented, nested times**, not components that can be added. The profiling header and call-site modifications were removed; diagnostic logs `build/perf-clone-profile2.{out,err}` remain.
- Disabling parallel dispatch globally gave a small clonechurn improvement but would compromise unrelated workloads; no change to the default scheduler.

## Verification

Build isolated from other agents in `build-clone-perf`, because the earlier shared `build-perf-all` directory was removed during concurrent work.

- `cmake --build build-clone-perf --target xyo xyo-tests -j4`: PASS.
- `ctest --test-dir build-clone-perf -R '^xyo-tests$'`: **1/1 PASS (42.21s)** with optimization default ON.
- `XYO_CLONE_SERIAL_CODEGEN=1 ctest ...`: **1/1 PASS (42.26s)**.
- `XYO_CLONE_SERIAL_CODEGEN=0 ctest ...`: **1/1 PASS (42.44s)**.
- GUI ScratchBench parity **2123/2123 (100%)** in both default ON and opt-out OFF configurations; `build/perf-clone-final-parity.log` and `build/perf-clone-final-off-parity.log`. Source hash `aeaa86a89eca6afe`.
- The 2123/2123 improvement over earlier 2121/2123 must **not** be attributed solely to this patch: the shared worktree includes concurrent graphics/parity changes by other agents.
- `git diff --check`: PASS on the touched files.

The remaining performance gaps are substantial: `sched_waituntil` still needs two explicit experimental flags, and `algo_primes`, `app_life`, `sched_looptick`, `algo_sieve`, `sched_clonechurn` are below the five-rival stored best. No commit/push in the shared dirty tree. Keep `PERF-001` in progress.
