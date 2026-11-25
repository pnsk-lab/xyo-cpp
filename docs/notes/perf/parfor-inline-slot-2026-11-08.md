# ParFor direct LLVM SList slot stores — benchmark batch (2026-11-08)

## Objective

Continue `PERF-001`, moving xyo-cpp toward outperforming MistWarp, TurboWarp, NitroBolt, Scratch Everywhere!, and Scratch on **every separate** ScratchBench performance workload. This batch targets `list_append` (previously 0.45× the best stored rival).

The project is a real Scratch script, not a hand-coded benchmark shortcut: the existing ParFor compiler analyzes any `control_repeat` with a positive integral induction counter and exactly one append per destination list per iteration. The optimization applies to that generic verified pattern.

## Root cause and changes

Previously `emitListAdd()` inside a compiled ParFor body called the C helper `xyo_par_slot(SList*, index, Value.bits)` once per append. The ParFor runtime has already:
1. validated trip count/counter/list limits;
2. reserved exactly `base+trip` list positions before executing the body;
3. disabled GC collection for the body lifetime through `Gc::DeferCollect`;
4. restricted the source loop to a straight chain of induction changes and appends, with no append-list reads, yield, or external effects.

Thus every slot's bounds and address are already valid. `src/jit/codegen.cpp::emitListAdd` now emits an LLVM load of `SList::items` and a direct 64-bit tagged-value store to `items[base+iter]`, replacing the C helper call. The old helper remains available with `XYO_PARFOR_INLINE_SLOT=0`; direct stores are **on by default**.

The old `xyo_list_reserve` also wrote zeros to every newly reserved position before the JIT body overwrote every single position. `src/jit/parfor_rt.cpp` adds an internal reserve path that skips redundant zeros and moves `Gc::DeferCollect` to cover both reservation and the body, preventing GC from inspecting temporarily unfinished contents. `XYO_PARFOR_SKIP_ZERO_FILL=0` restores the defensive initialization, and the zero-fill skip is **on by default**. The full old behavior can be restored with both opt-outs.

The existing safety checks still reject alternative SList modes (4-7), overflowing/invalid counters, over-limit lists, absent list storage and ineligible reporters. Invalid plans retain the ordinary interpreter/JIT loop. The source codegen and the Runtime/host effect ABI otherwise remain unchanged.

A regression was added to `tests/unit/test_parfor.cpp`: a 1000-item warp procedure whose append value is a *new, dynamically generated string* `"v=" + i`. This checks the exact UTF-16 strings, tagged GC string references and element order, in addition to the pre-existing 1000-element serial batch and 4000-element worker-parallel numeric cases.

## Measured performance

The same Release binary and same isolated `list_append.sb3` fixture were used in each alternating A/B pair on i7-12700, HotSpot, 30fps. The workload's checksum was **11000** in every run.

| Measurement | Baseline median | Optimized median | Factor | Trials |
|---|---:|---:|---:|---:|
| C helper vs direct LLVM slot store (zero fill unchanged) | 338,220 ops/ms | 609,114 ops/ms | **1.80×** | 12 alternating pairs |
| Direct slot store, ordinary zero fill vs no zero fill | 708,891 ops/ms | 721,856 ops/ms | **1.018×** | 12 alternating pairs |

Raw data: `build/perf-parslot-ab.json`, `build/perf-parslot-zero-ab.json`. Results come from *different build timestamps*, as other agents were concurrently editing LLVM math optimizations; the first comparison and second comparison are not interchangeable absolute baselines.

### Whole ScratchBench performance, all 47 workloads

Each full suite has one run; results are sensitive to CPU contention and test order. Competitor reports are saved from earlier dates on the same i7-12700 and use suite hash `e8bd5be3e7345088`. These are **provisional comparisons**, not guaranteed wins against contemporaneous releases.

- `scratch-bench/results/parslot-inline-off-20261108/performance-xyo-cpp.json`: direct store OFF, zero-fill unchanged. `list_append` **336,258 ops/ms**; overall **98,247**.
- `scratch-bench/results/parslot-inline-on-20261108/performance-xyo-cpp.json`: direct store ON, zero-fill unchanged. `list_append` **705,705 ops/ms**; overall **99,387**; **28/47** above the saved five-rival maximum.
- `scratch-bench/results/parslot-default-final-20261108/performance-xyo-cpp.json`: direct store ON and skip-zero-fill ON by default. `list_append` **684,338 ops/ms** (0.980× Scratch Everywhere!'s saved 698,143); overall **100,666**; **27/47** above five-rival maximum. The difference from the earlier 28/47 run is a small measurement swing and not grounds to claim a regression.

The final itemized comparison is `scratch-bench/results/parslot-default-rivals-20261108.{md,json}`. Among the biggest remaining gaps are `sched_waituntil`, `algo_primes`, `app_life`, `sched_clonechurn`, `algo_sieve` and `sched_looptick`. The 47/47 goal remains **unmet**.

## Correctness and builds

- `cmake --build build-perf-all --target xyo xyo-tests`: native Release build PASS.
- `XYO_PARFOR_INLINE_SLOT=1 XYO_PARFOR_SKIP_ZERO_FILL=1 ctest --test-dir build-perf-all -R '^xyo-tests$'`: PASS 1/1, 48.38s (including new dynamic-string test). Log `build/perf-parslot-zero-ctest.log`.
- `ctest` with direct stores default-ON and zero-fill still OFF: PASS 1/1, 48.47s, `build/perf-parslot-final-ctest.log`.
- ScratchBench parity after both flags default ON: **2121/2123** (99.9%); the two failures are existing `Looks: Size bounds per costume` tests 8,9, unrelated to this code. Log `build/perf-parslot-default-parity.log`. Suite hash `aeaa86a89eca6afe`.
- Final CTest with both defaults: **PASS 1/1 in 45.66s**, `build/perf-parslot-default-ctest.log`.
- `git diff --check` passed for the focused code/test files.

## Concurrency/ownership

Changes were restricted to the `ParFor` body branch in `src/jit/codegen.cpp`, `src/jit/parfor_rt.cpp`, and `tests/unit/test_parfor.cpp`. Other agents' unrelated extensive dirty source changes were preserved. No commit/push was performed, and `PERF-001` remains in progress.
