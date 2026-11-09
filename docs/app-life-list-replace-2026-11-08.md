# ScratchBench app_life: JIT list replacement null-check elimination (2026-11-08)

## Goal

Continue `PERF-001` toward exceeding MistWarp, TurboWarp, NitroBolt, Scratch Everywhere!, and Scratch on **every one** of the 47 ScratchBench performance cases. This batch targets `app_life`, a 12×12 Conway Life simulation repeatedly reading and replacing list elements in two 144-entry lists.

The benchmark source was not changed. For an isolated experiment, `tests/bench/algos/scratchbench_probe.py` adds a generic `life` case that builds `build/scratchbench-cause-probes/projects/life.sb3` via the vendored Fractch compiler.

## Root cause and implementation

The JIT `emitListReplace` implementation in `src/jit/codegen.cpp` already emits a bounded, direct element store when a list index is numeric. When a loop's `PromotedList` caches `SList::len`, `SList::items` and the list's numeric feedback, an independent list-pointer null check on **every replacement** is normally redundant. The preheader and yield reloader already map a null pointer to `len=0/items=null`. Numeric indices fail the bounds check; special string indices such as "last", "random", and malformed text still use the general helper.

The pre-existing experimental environment setting `XYO_JIT_REPLACE_NO_NULL=1` enabled this direct branch. Five-trial isolated measurements showed it consistently beats the default.

**Now default-enabled, guarded by a compile-time proof**: the source `Target` must have a known `Variable::kListType` with an existing `SList*` for that ID. This guard matters because a malformed Scratch file may reuse a **scalar variable ID** in a list field; then `Target::lookupOrCreateList` returns the scalar variable and the `xyo_list_replace` helper must lazily allocate an SList. Skipping this helper could silently drop the allocation. If the proof fails, the original checked branch is retained. An explicit `XYO_JIT_REPLACE_NO_NULL=0` restores the historical null check for every ordinary list replacement. Adaptive mode 7 keeps its existing separate policy.

This optimization applies generically to eligible Scratch list writes, not the benchmark name, list dimensions or cellular automaton rules. Mathematical, scheduler, native continuation and IR code owned by other agents were not changed.

**New correctness regression** in `tests/unit/test_list_codegen_bounds.cpp` constructs a malformed Scratch project with one scalar ID also referenced as a list in a three-iteration warp loop and asserts: both interpreter and native JIT lazily create the missing SList, the out-of-range replace remains a no-op, the scalar value is unchanged, and the script terminates. This augments the existing comprehensive list index tests (NaN/infinity/negative/fractional/numeric strings/last/random/name aliasing).

## Experimental performance

Machine: i7-12700, same native Release build, 30fps, HotSpot, no source changes to benchmark. Rates are ops/ms. The isolated Fractch diagnostic writes a deterministic checksum **53** in every trial.

| Measurement | Old checked path | New guarded fast path | Improvement |
|---|---:|---:|---:|
| Initial isolated sample, 5 runs each | 23.68 | 25.64 | +8.3% |
| Final exact-source A/B, 12 alternating pairs | 23.6455 | 25.6985 | **+8.68%** |
| Whole original 47-case suite, 1 run each | 12,890.385 | 14,573.564 | **+13.06%** |

Raw isolated tuning: `build/perf-life-flags.{json,log}`; 11 flag/storage options were compared, with no other consistent wins. Final same-binary paired A/B: `build/perf-life-default-ab.{json,log}`. The full-suite ON/OFF comparison used the old explicit `XYO_JIT_REPLACE_NO_NULL=1` and `=0` forms (prior to narrowing the safety condition); the original `app_life` lists are statically known and satisfy the new guard, so they take the same fast path.

Full-suite output: `scratch-bench/results/perf-life-null-{on,off}-20261108/performance-xyo-cpp.json`. Overall score ON **99,886**, OFF **99,372**; this +0.5% change is within normal run-to-run variability. The one-run per-case results include unrelated scheduler noise; do not ascribe such differences to this list-only patch.

Saved five-rival comparison of the ON run: `scratch-bench/results/perf-life-null-rivals-20261108.{md,json}`: **29/47 wins**, **18/47 losses**; `app_life` **0.621×** saved MistWarp **23,477.49 ops/ms**. Final **default-enabled, statically guarded binary** run:
`scratch-bench/results/perf-life-default-final-20261108/performance-xyo-cpp.json`, `app_life` **14,873.01 ops/ms**, overall **100,484**. Five-rival comparison in `scratch-bench/results/perf-life-default-rivals-20261108.{md,json}`: **29/47 wins, 18 losses**, `app_life` **0.634×** MistWarp's earlier **23,477.49 ops/ms**. This is a separate one-run measurement; run-to-run CPU load and order can cause fluctuations. Competitors were measured on earlier dates, so wins are provisional rather than simultaneous head-to-head runs.

## Correctness and validation

- Native `cmake --build build-clone-perf --target xyo xyo-tests -j4`: **PASS**. Isolated build dir avoids races with other agents.
- Default `ctest --test-dir build-clone-perf --output-on-failure -R '^xyo-tests$'`: **1/1 PASS, 82.33s** (system has other active agents).
- ScratchBench GUI parity, default JIT: **2123/2123 = 100.0%** with source hash `aeaa86a89eca6afe`, log `build/perf-life-default-parity.log` and result `scratch-bench/results/perf-life-default-parity-20261108`. The parity result reflects *all* changes in the concurrently modified shared checkout, not solely this patch.
- Historical-path test `XYO_JIT_REPLACE_NO_NULL=0` CTest: PASS 1/1 (48.61s), log `build/perf-life-optout-ctest.log`.
- `git diff --check` passed for `src/jit/codegen.cpp`, `tests/unit/test_list_codegen_bounds.cpp`, and `tests/bench/algos/scratchbench_probe.py`.

**47/47 remains unmet.** The next large gaps are `sched_fanwait`, `sched_fanout`, `sched_waituntil` under default settings, `algo_primes`, `sched_clonechurn`, `sched_looptick`, `algo_sieve`. This change is in the shared working tree but is not committed or pushed.
