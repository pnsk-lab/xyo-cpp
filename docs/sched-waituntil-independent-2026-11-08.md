# sched_waituntil: native independent dependency watcher (2026-11-08)

## Objective

Continue `PERF-001`, with the long-term goal to win each of 47 ScratchBench performance workloads against five other runtimes. Previous opt-in `XYO_EVENT_SCHED=1 XYO_EVENT_SCHED_DEP=1` gave ~6× throughput on `sched_waituntil` but enabling the **global event scheduler** by default broke a low-level direct-Fiber-chain assertion and changed unrelated scheduling. Its dependency watcher had never been enabled independently.

## Implementation

The new mode avoids global epoch scheduling completely. On native platforms, `XYO_WAIT_DEP_FAST` is **on by default** and `XYO_WAIT_DEP_FAST=0` restores historical polling. Emscripten/browser builds retain the former default (off), unless explicitly opted in.

- `src/jit/codegen.cpp`: compile `control_wait_until` with a pure reporter AST into the existing `xyo_wait_event_yield` and `xyo_wait_event_done` pair when the isolated dependency watcher or global event scheduler is enabled. AOT and `XYO_BLOCK_COUNT` instrumentation stay on the original `xyo_wait_yield` path: AOT may contain stale source Block addresses, and exact block-count instrumentation cannot elide repeated evaluations.
- `src/engine/event_wait_dependency.hpp`: `isolatedEventWaitDependencyEnabled()` controls the independent watcher; `eventWaitDependencyEnabled()` also accepts legacy `XYO_EVENT_SCHED_DEP=1`. Existing snapshot logic validates that the wait condition is side-effect-free, has **exactly one scalar Variable*** (after aliases), and stores its exact `Value::bits`, source revision, Stage identity and local/Stage binding generations. Unproven, time-dependent, impure and multiple-variable predicates fall back to normal retry.
- `src/engine/sequencer.cpp`: arm and check the existing `eventWaitArmed` state for the isolated watcher as well as the global scheduler. An unchanged, valid dependency skips resuming the suspended Fiber; a changed value/source/variable binding forces serial reevaluation in original receiver order. The global `eventProgressEpoch` is **not advanced** when using the isolated mode alone, so unrelated scripts retain their previous scheduling.
- `tests/unit/test_scheduler.cpp`: direct-Fiber-chain *internal counter* assertion applies only to the ordinary polling path, because the watcher intentionally suppresses these passes. The exact 5-event receiver output/order assertions, scalar dependency invalidation tests and all other correctness assertions remain active.

This is a **general Scratch AST optimization**, not a `sc_gate`/benchmark-specific rewrite. Every unproven condition remains in the original polling path.

## Controlled isolated A/B

Same initial frozen native Release executable `build/perf-wait-deponly-frozen-xyo` (SHA256 `9944f7e995f16712b81a76fb5e3f99c45d7edf2cac90a687cc874e33bf34cf12`), 30fps, i7-12700, pinned CPU8, 12 alternating replications of each option, unchanged source-derived `waituntil.sb3`:

| Variant | Median ops/ms | Versus baseline |
|---|---:|---:|
| Global event scheduler OFF, isolated watcher OFF | 180.149 | 1.00× |
| **Only isolated watcher ON** | **1,063.050** | **5.901×** |
| Previous global event + dependency scheduler | 1,052.548 | 5.843× |

The isolated watcher won **12/12 paired trials** against baseline. Raw `build/perf-wait-deponly-ab.{json,log}`. Checksums vary because the benchmark runs for a fixed duration and counts how many operations finish before the deadline.

## Whole 47-workload suite (before changing the default)

Same frozen executable and suite hash `e8bd5be3e7345088`, two sequential full runs, opt-in vs explicit OFF:

| Metric | OFF | ON |
|---|---:|---:|
| `sched_waituntil` | 182.489 | **988.659 ops/ms** (5.42×) |
| ScratchBench weighted overall | 102,205 | **103,229** (+1.0%) |
| All-47 geomean ON/OFF | — | 1.0183× |
| All-five-rival wins | — | **29/47**; 18 unfinished |

Files: `scratch-bench/results/perf-wait-deponly-{on,off}-20261108/performance-xyo-cpp.json`. Rival comparison `scratch-bench/results/perf-wait-deponly-rivals-20261108.{md,json}`. Stored rival best for `sched_waituntil` was TurboWarp at **324.81 ops/ms**; the new watcher reaches ~3.04× that stored result.

These 47-case scores are single-run estimates amid other agents and CPU load variation. Non-wait workload changes must **not** be attributed to a scheduling-only patch. Rival scores were measured on older dates; win counts are provisional.

## Verification

- Native coherent build via `cmake --build build-perf-fan-only-20261108 --target xyo xyo-tests -j4`: PASS.
- With `XYO_WAIT_DEP_FAST=1` and both global event scheduler flags OFF, `xyo-tests` **1/1 PASS (49.40s)**. Initial test run failed **only** the old direct-fiber-chain counter, not any state/order assertion; the test was made conditional on the polling path. Log `build/perf-wait-deponly-ctest2.log`.
- **Default ON** `ctest ... -R '^xyo-tests$'`: **1/1 PASS (65.87s)**, log `build/perf-wait-deponly-default-ctest.log`.
- **Explicit OFF** `XYO_WAIT_DEP_FAST=0 ctest ...`: **1/1 PASS (42.44s)**, log `build/perf-wait-deponly-optout-ctest.log`.
- ScratchBench GUI parity **2123/2123 (100.0%)** both with opt-in `XYO_WAIT_DEP_FAST=1` and with final default ON. Logs `build/perf-wait-deponly-parity.log` and `build/perf-wait-deponly-default-parity.log`; suite hash `aeaa86a89eca6afe`.
- Final default binary frozen at `build/perf-wait-deponly-default-frozen-xyo` SHA256 `d97fa94ce96e12481166dcf7c83c62a92fc78f905c28fb52623df95028ea6c0f`; standalone isolated `sched_waituntil`: **1074.824 ops/ms**.
- `git diff --check` clean. No other agents' source changes reset or committed.

## Final default-enabled native binary

After the default transition and additional compile-time guards for native
AOT/accurate block-count instruments, the independent frozen executable
`build/perf-wait-deponly-default-frozen-xyo` (SHA256 above) ran the original
47-case ScratchBench suite with *no XYO_WAIT_DEP_FAST environment variable*:
`scratch-bench/results/perf-wait-deponly-default-final-20261108/performance-xyo-cpp.json`.

- `sched_waituntil` **1,083.61 ops/ms**, 3.336× stored TurboWarp best
  324.81, and an isolated same-binary run **1,074.824 ops/ms**.
- Overall ScratchBench weighted score **106,318** (one run).
- Saved five-rival comparison **31/47 wins, 16 losses**, list in
  `scratch-bench/results/perf-wait-deponly-default-rivals-20261108.{md,json}`.
  Other agents' concurrent changes are reflected in this binary and
  source tree; improvement in unrelated categories is **not** claimed as
  the wait watcher effect. The run is not a contemporaneous rival rerun.
- Single-run peak RSS **234.7 MB**.
- Full default and explicit opt-out unit tests PASS and default GUI parity
  remains **2123/2123** (see verification section).

## Remaining gaps

**47/47 is still not achieved**. At this checkpoint `algo_primes` (~0.53× of stored MistWarp), `sched_clonechurn` (~0.57×), `sched_looptick` (~0.59×), `algo_sieve` (~0.62×), `app_life` (~0.62×) are high-priority targets. The RT-001 scheduler/continuation owner should preserve source revision and VariableMap-generation validation for the new default watcher. Concurrent edits may require clean rebuild/freeze to avoid ABI mismatches.

No commit or push in the shared dirty worktree.
