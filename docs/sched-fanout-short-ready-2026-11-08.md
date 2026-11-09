# ScratchBench scheduler fanout: short broadcast JIT ready-cache (2026-11-08)

## Scope

`PERF-001` continues the goal of exceeding MistWarp, TurboWarp, NitroBolt, Scratch Everywhere!, and Scratch on all 47 original ScratchBench performance workloads. This batch targets `sched_fanout` and `sched_fanwait`, both dispatching a broad set of short broadcast receivers.

Neither benchmark fixture nor the `scratch-bench` suite was modified.

## Profiling / reason

In the native Release profile build an isolated fanout run spawned about 623,000 receiver threads in ~0.4s. A previous `Thread::tryCompile` direct-cache experiment eliminated repeated `JitEngine::decideCompile` and `compile` calls but touched *all* precompiled scripts: **~585,089** per run became **3**, with median compile-related inclusive time **84.45 → 55.3 ms**. The same experiment showed no consistent `fanwait` benefit, so applying it broadly was not justified.

The final implementation in `src/engine/thread.cpp` restricts direct `JitEngine::readyScript(top)` attachment to **event_whenbroadcastreceived** native compiled scripts that satisfy *all*:
- `CompiledScript::shortSync` is true;
- `mayYield` is false, `mainThread` is false;
- the experimental `XYO_JIT_COMMON_CFG` attachment mode is disabled;
- the immutable compiled entry is still in the live JIT cache.

This avoids repeated HotSpot compile/profile and numeric-CFG attachment decisions for short synchronous listeners only. Long/yielding listeners, generic event hats, editor source changes and compile misses retain the existing path. No benchmark-specific project name, message text or receiver count is used.

Enabled by default. `XYO_JIT_READY_CACHE_FAST=0` restores the historical behavior. This replaces the earlier *broad* opt-in use of that same flag, which did not have robust full-workload benefits.

## A/B, CPU8 pinned

Same executable `build-perf-fan-only-20261108/xyo`, original source-derived `fanout.sb3` and `fanwait.sb3`, i7-12700, native HotSpot, 30fps, 12 alternating ON/OFF pairs. Outputs: `build/perf-fan-ready-short-ab.json` and `build/perf-fan-ready-short-ab.log`.

| Isolated workload | OFF median ops/ms | ON median ops/ms | Relative | Faster pairs |
|---|---:|---:|---:|---:|
| `sched_fanout` | 2016.713 | 2120.666 | **+5.15%** | 10/12 |
| `sched_fanwait` | 2076.296 | 2330.207 | **+12.23%** | 11/12 |

These are fixed-walltime throughput workloads; checksums measure events actually completed during that time and are *not* expected to remain identical across different performance rates.

## Whole 47 original ScratchBench workloads

One complete performance suite per flag, same binary and source hash `e8bd5be3e7345088`, sequential rather than parallel, on i7-12700. Reports:
- `scratch-bench/results/perf-fan-ready-short-on-20261108/performance-xyo-cpp.json`
- `scratch-bench/results/perf-fan-ready-short-off-20261108/performance-xyo-cpp.json`

| Metric | OFF | ON | Interpretation |
|---|---:|---:|---|
| `sched_fanout` | 1691.628 | 2136.557 | +26.3% in this *one* run |
| `sched_fanwait` | 1592.457 | 2084.448 | +30.9% in this *one* run |
| Overall score | **97,464** | 95,447 | **-2.1%**; not a demonstrated overall improvement |
| 47-workload unweighted geometric mean (ON/OFF) | — | 0.9887× | Broad drift in unrelated CPU-bound workloads |
| Peak RSS | 218.9 MB | 234.4 MB | Single-run variability; no causal memory claim |

The source-only change is a short synchronous broadcast receiver fast path, so movements in mathematical, list and string scores must not be attributed to this patch without paired controls. The 12-pair isolated experiment gives more reliable information about the two targeted scheduler cases than a single 47-case run.

Relative to **stored older** results for five rival runtimes, the ON run had **25 wins / 22 losses**, not the previous 29/47 observed in an earlier build. This figure fluctuates across builds and load conditions and is not a controlled competitor comparison. Itemized ranks: `scratch-bench/results/perf-fan-ready-short-rivals-20261108.{md,json}`. `sched_fanout` remains at 0.732× the stored fastest Scratch Everywhere! value, `sched_fanwait` at 0.650×, and **47/47 remains unmet**.

## Correctness

A new regression in `tests/unit/test_scheduler.cpp` creates a real Scratch broadcast receiver with one Stage variable increment, starts broadcasts 12 times (including lowercase message variants), drains each receiver, checks exact 1-per-broadcast state changes and verifies native `shortSync` JIT warmup. It passes through both enabled and disabled paths.

- `cmake --build build-perf-fan-only-20261108 --target xyo xyo-tests -j4`: PASS.
- `XYO_JIT_READY_CACHE_FAST=1 ctest --test-dir build-perf-fan-only-20261108 --output-on-failure -R '^xyo-tests$'`: **1/1 PASS, 66.87 s**.
- `XYO_JIT_READY_CACHE_FAST=0 ctest --test-dir build-perf-fan-only-20261108 --output-on-failure -R '^xyo-tests$'`: **1/1 PASS, 54.08 s**.
- Original ScratchBench GUI parity: ON **2123/2123**, OFF **2123/2123**, both **100%**, source hash `aeaa86a89eca6afe`.
- `git diff --check` clean on modified implementation and test files.

Shared working tree contains other agents' ongoing IR/RT/math/render changes. Only the narrow `Thread::tryCompile` section and scheduler unit regression were retained in this batch. No commit or push.

## Rejected experiments in this batch

The following were measured and rolled back:
- Precompute broadcast target/block lookup strings in receiver index: CPU8 12-pair `fanout` **917.23→887.16** and `fanwait` **1033.59→932.45** ops/ms, both slower.
- Retain JIT variable slots across reused broadcast thread runs: initial A/B inconclusive, guarded variant gave no consistent net gain or clean source-lifetime advantage. Reverted from runtime and Thread.
- Stack-allocate `JitCtx` for short synchronous native scripts: CPU8 12-pair `fanout` **1892.38→1920.39** (+1.48%, only 7/12 pairs) but `fanwait` **1743.67→1689.94** (−3.1%, 5/12 pairs). The change was removed from `src/engine/sequencer.cpp`.

These low-value paths were not enabled by default. The final narrow ready-cache path is the retained improvement.

## Remaining work

A controlled sequence of alternated full 47-case runs, with background CPU load held stable, is needed before claiming a positive aggregate score. Major open gaps are `sched_waituntil` (dependency-driven experiment faster but opt-in), `algo_primes`, `app_life`, `sched_clonechurn`, `algo_sieve` and `sched_looptick`. Preserve existing semantics and parity; no hard-coded benchmark rewrites.
