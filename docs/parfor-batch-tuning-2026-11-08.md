# Parallel append batching: two-stage dispatch thresholds (2026-11-08)

## Problem

On `scratch-bench`'s `list_append` workload, the JIT recognizes a 1000-iteration warp loop that repeatedly appends induction-counter values. Previously, `XYO_PARFOR_MIN=512` and `XYO_PARFOR_CHUNK=256` allowed `xyo_par_for()` to spawn three worker chunks for each 1000-iteration loop. `XYO_PARFOR_DEBUG=1` showed about **117,570 parallel dispatches** during one benchmark execution. Thread dispatch and joins dominate the actual append work.

The script and the benchmark harness remain unchanged. The suite's source hash is `e8bd5be3e7345088` and the host is an i7-12700.

## Implemented

- `src/jit/codegen.cpp`: compiler fusion threshold remains `XYO_PARFOR_MIN=512`. Eligible `repeat` loops still lower to `xyo_par_for()` even when actual worker parallelization would be wasteful.
- `src/jit/parfor_rt.cpp`: worker-parallel threshold is independently `XYO_PARFOR_PARALLEL_MIN=2048` by default; default `XYO_PARFOR_CHUNK` is raised from 256 to 512.
- Below that worker threshold, execute the already fused body **sequentially over one batch**, after reserving list slots, with the original `Gc::DeferCollect` safety and per-iteration order. Do not construct a worker pool or acquire its lock on this hot path.
- Large batches still use the pre-existing worker-pool implementation.
- `tests/unit/test_parfor.cpp`: test both 1000-iteration serial-batch (no worker dispatch) and 4000-iteration parallel-batch cases with exact list contents and counter checks.

The user-visible effect is unchanged: append order, resulting list values and the induction-counter value must match the original Scratch execution. The existing par-for safety checks for integer counters, list limits, source opcodes and valid inputs remain enabled.

## A/B benchmark: list_append

Same `build-perf-all/xyo` binary and workload, 8 alternating trials, HotSpot, 30 fps. All checksums = `11000`. Numbers are medians of `ops_per_ms`.

| Mode | Ops/ms | Relative to old |
|---|---:|---:|
| Old worker dispatch (min=512, chunk=256, parallel-min=1) | 193,390 | 1.000 |
| No fused batch (min=2048) | 297,699 | 1.539 |
| **New default fused serial batch (min=512, chunk=512, parallel-min=2048)** | **338,284** | **1.749** |

Logs: `build/perf-append/final-ab.json`, `build/perf-append/final-ab.py`.

Additional 2-pair full-47 comparison using the same binary and switching the above two interim strategies, before the final runtime lock bypass: serial min=2048 vs fused min=512, chunk=512. `list_append` was 296,184 vs 310,688 (+4.9%). Full-suite 47 geometric mean +0.36%, which is smaller than general run-to-run variance; 39 non-time-dependent checksums agree. The eight scheduler checksums vary because those workloads count executions over wall-clock windows. Raw reports: `scratch-bench/results/perf-append-{serial,batched}-*/performance-xyo-cpp.json`.

## Whole-suite result and compatibility

Latest final modified-binary ScratchBench 47-workload pass:
- `scratch-bench/results/perf-append-final-20261108/performance-xyo-cpp.json`
- weighted overall score **86,984**; 22/47 workloads ahead of the stored fastest results from MistWarp, TurboWarp, NitroBolt, Scratch Everywhere!, and Scratch; 25/47 still below.
- `scratch-bench/results/perf-append-final-rivals.{json,md}` is the itemized five-rival comparison.
- The competitors' reports come from earlier days, and this final full suite has only one run. **Do not interpret 22 vs the previously observed 16 wins as an isolated causal gain from par-for changes**; other agents' changes and CPU-load drift are confounders.

Validation:
- Native Release/LLVM build: `cmake --build build-perf-all --target xyo xyo-tests -j4` PASS. Build log `build/perf-append/batch-build.log`.
- `ctest --test-dir build-perf-all -R '^xyo-tests$' --output-on-failure`: **1/1 PASS**, 43.69s, `build/perf-append/ctest.log`.
- ScratchBench parity: **2121/2123**, 99.9%, with only `Looks: Size bounds per costume` tests 8/9 failing. `XYO_PARFOR=0` produces precisely the same failing groups. Logs: `build/perf-append/{final-parity,parity-noparfor}.log`. Parity suite hash: `aeaa86a89eca6afe`.

## Remaining work

The goal remains 47/47. Biggest current measured gaps include `sched_pingpong`, `list_append`, `math_collatz`, `math_arith`, `math_mandel`, and `algo_primes`. Analyze native generated code and runtime dispatch costs rather than special-casing the benchmark project. Run controlled paired five-rival comparisons with repeated trials before making a claim of overall superiority.

This was implemented inside a **shared dirty worktree**. The other agents' changes were preserved; no commit or push was attempted.
