# ScratchBench sched_looptick: Scratch day-clock sampling and preboxing (2026-11-08)

## Objective and safety

`PERF-001` aims to win all 47 ScratchBench performance cases. This experiment targets `sched_looptick`, a warp loop repeatedly querying `sensing_dayssince2000` while incrementing a Scratch scalar. Previous native defaults use `rt::daysSince2000` for every reporter evaluation.

There is an existing opt-in `XYO_JIT_DAYS_SHARED_CLOCK=1` that samples wall-clock milliseconds from the sequencer's 1ms watchdog. It is **not** enabled by default: a 1ms asynchronous sample may lag the exact wall-clock value, which can be observed in a sufficiently sensitive Scratch project. The old synchronous reporter remains the reference behavior.

This batch adds an **optional second tier**, `XYO_JIT_DAYS_BOXED_CLOCK=1` when shared clock is already on. The watchdog converts sampled milliseconds into exactly the same IEEE-754 / NaN-boxed Scratch `Value` bits *once per sample*. Native JIT reads an atomic `uint64_t` instead of doing integer-to-double conversion, subtracting the Unix-to-2000 epoch constant, dividing and canonicalizing on **every** reporter evaluation.

Modified code:
- `src/engine/warp_clock.hpp/.cpp`: `sampledUnixDaysBits` atomic, recomputed after each shared wall-clock sample using `Value::num((double(ms) - 946684800000.0) / 86400000.0).bits`. Computation uses the exact ordering of `rt::daysSince2000`.
- `src/jit/jit.hpp`: pointer appended to `JitCtx`, preserving prior member offsets.
- `src/engine/sequencer.hpp/.cpp`: attach the boxed atomic only while the wall-clock sampling watchdog is active, including resumed parallel fibers.
- `src/jit/codegen.cpp`: compile-time switch between existing shared milliseconds-to-f64 code and a single atomic boxed-value load. With missing sampler, both paths fall back to the synchronous helper.
- `tests/unit/test_danger.cpp`: disarm watchdog, then require its preboxed bits to match a reference conversion of its final sampled milliseconds **bit for bit**.
- `tests/bench/algos/scratchbench_probe.py`: add `looptick` isolated diagnostic project without touching benchmark source.

This additional tier is an **experiment only**, default OFF. It does not affect default Scratch semantics. `XYO_JIT_DAYS_BOXED_CLOCK=1` alone has no effect unless `XYO_JIT_DAYS_SHARED_CLOCK=1` is also set.

## Paired A/B (i7-12700; CPU8 pinned)

The first frozen binary measured the **existing shared-clock flag**, with 12 alternating pairs:
- Synchronous reporter median **7597.576 ops/ms**.
- Existing 1ms shared clock median **9234.965 ops/ms**.
- Shared is **+21.55%**, faster in **12/12** paired trials.

Raw `build/perf-looptick-shared-ab.{json,log}`.

Second binary added preboxing, still 12 alternated three-mode groups, same i7-12700 pinned to CPU8:
- Synchronous reporter median **6735.808 ops/ms**.
- Existing shared clock median **7841.668 ops/ms**.
- **Shared + boxed** median **8157.469 ops/ms** (+4.03% vs shared, only **8/12** pair wins).

Raw `build/perf-looptick-boxed-ab.{json,log}`. Load conditions differed between batches, so do not compare the absolute medians from the two tests. The 4% figure is tentative, not statistically convincing.

An attempt to inline the synchronous reporter's C++ helper reduced one cross-translation-unit function call without changing clock semantics. Its own 12-pair A/B showed baseline **6962.08**, optimized **7209.43 ops/ms**, but only **5/12** pairs favored the new path; it was **fully rolled back** (file `src/jit/rt_blocks.cpp` restored). Raw `build/perf-looptick-inline-ab.{json,log}`.

## Whole 47-case ScratchBench, same frozen binary

Final executable `build/perf-looptick-frozen-xyo` (SHA256 `810c2c9c431d70ace30b235902cd8f8350d4f067724fae5da4543cd791c9d294`), suite hash `e8bd5be3e7345088`. Both runs used `XYO_JIT_DAYS_SHARED_CLOCK=1`; only `XYO_JIT_DAYS_BOXED_CLOCK` differed.

| Metric | Shared (old) | Shared + boxed |
|---|---:|---:|
| `sched_looptick` | 9063.001 ops/ms | 8964.108 ops/ms |
| Whole-suite overall score | **102,755** | 101,032 |
| Whole-suite all-47 geometric mean relative | 1× | 0.9853× |
| Peak RSS | 227.3 MB | 226.9 MB |

Reports: `scratch-bench/results/perf-looptick-{shared,boxed}-20261108/performance-xyo-cpp.json`; five-rival comparison on boxed run `scratch-bench/results/perf-looptick-boxed-rivals-20261108.{md,json}` recorded **28 wins / 19 losses** against older rival data. Do not attribute broad unrelated case movements to this clock-only change; these are single full-suite runs subject to system load and other agents' concurrent changes.

A later separate default/no-flags run of the same frozen binary yielded `sched_looptick` 6449.27 ops/ms and overall **89,390** (**25/47** wins versus stored rivals). Result: `scratch-bench/results/perf-looptick-default-20261108`. This run had substantially lower unrelated throughput than the earlier clock variants due to varying CPU load, so it must not be treated as a causal effect of either clock flag.

**Decision:** Do not enable shared-clock or preboxing by default. Preboxing remains opt-in as a correctness-validated experiment. There is no demonstrated whole-suite win, and the asynchronous 1ms sampling changes real-time observation semantics. Further effort should focus on retaining exact wall-clock readings while reducing reporter/variable overhead, or obtaining more controlled performance samples.

## Validation

- Release `cmake --build build-perf-fan-only-20261108 --target xyo xyo-tests -j4`: PASS.
- `XYO_JIT_DAYS_SHARED_CLOCK=1 XYO_JIT_DAYS_BOXED_CLOCK=1 ctest ... -R '^xyo-tests$'`: **1/1 PASS (53.46s)**, `build/perf-looptick-boxed-ctest.log`.
- Original ScratchBench parity with both opt-in flags: **2123/2123 (100%)**, `build/perf-looptick-boxed-parity.log`.
- Default CTest **1/1 PASS (71.22s)**; log `build/perf-looptick-default-ctest.log`.
- Final frozen binary default GUI ScratchBench parity **2123/2123 (100%)**; log `build/perf-looptick-default-parity.log`.
- `git diff --check` clean for touched files.

No original benchmarks modified. Shared source tree contains numerous other agents' edits, which were not reset, committed or pushed. **47/47 remains incomplete.**
