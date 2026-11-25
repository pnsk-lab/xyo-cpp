# ScratchBench sched_waituntil: dependency-aware event wait (2026-11-08)

## Goal and diagnosis

Continue `PERF-001` to beat five other Scratch runtimes independently on all 47 `scratch-bench` performance cases. The hardest scheduler gap was `sched_waituntil` at ~0.53× the stored TurboWarp best. The workload starts 101 `wait_until sc_gate == 1` receivers while an unrelated warp script increments a different variable repeatedly. All 101 waiters were re-entered via native Fiber in most sequencer passes despite `sc_gate` remaining 0.

The existing `XYO_EVENT_SCHED=1` merely records a global progress epoch. Each unrelated script execution still increments that epoch, thus **5 paired trials** showed `sched_waituntil` *regressing* from **179.33** to **111.84 ops/ms**. `XYO_PAR=0` was similarly slow at **115.54**. Interpreter was **57.22**. Raw `build/perf-wait-flags.json`.

## Implemented opt-in design

Enable **both** `XYO_EVENT_SCHED=1` and `XYO_EVENT_SCHED_DEP=1` to use dependency-based suppression. The default scheduler is **unchanged**.

- New `src/engine/event_wait_dependency.hpp`: when a pure `control_wait_until` is false, resolve at most one scalar variable from a whitelist of pure AST reporters, snapshot its exact `Value::bits`, and record the source revision and Target/Stage `VariableMap` binding generations. A cache is valid only as long as **all** those fields are unchanged. Non-pure reporters, multiple independent variables, missing IDs and unknown/AOT site tokens fall back to the existing scheduler. AOT tokens are compared to live source block addresses but **never dereferenced**.
- `src/engine/thread.hpp/.cpp`: record the conservative snapshot beside the existing eventWait state and clear it on Thread release/recycling.
- `src/jit/rt.cpp`: create a snapshot before the existing native Fiber yield; clear it after completing `wait until`.
- `src/engine/sequencer.cpp`: if the validated dependency is unchanged, skip resuming that waiter regardless of unrelated global progress. If its value, source or bindings change, **force reevaluation** even when the global epoch has not advanced. Interpreter/event-wait re-arming resets stale JIT snapshots.
- `tests/unit/test_scheduler.cpp`: covers one-variable AST, unchanged/changed values, variable deletion and rebinding, source revision invalidation, invalid source tokens and nondeterministic reporter rejection. A pre-existing direct Fiber-chain counter assertion now applies only when event-driven waiting is disabled; receiver completion and execution order are checked in both modes.

This is not a hard-coded `sc_gate` optimization: it applies to every source-proven, side-effect-free single-scalar predicate. It cannot prove all possible Scratch wait predicates, and it does not skip non-pure input evaluation.

### Why not enable it by default?

An experiment routing every source-proven single-variable predicate through the watcher without `XYO_EVENT_SCHED` changed broad scheduler behavior/performance: full `xyo-tests` took **189.53s** instead of ~45–50s and failed the test asserting the ordinary direct Fiber chain. Although the output ordering checks passed and parity remained unchanged in the separate opt-in configuration, the default experiment did **not** satisfy the regression standard. It was **rolled back**. The final implementation is opt-in via **both** flags. No new default scheduling behavior was shipped.

## A/B performance

Same binary / i7-12700 / 30fps, eight alternating trials, isolated unmodified `sched_waituntil.sb3`, median ops/ms:

| Option | ops/ms | Relative to baseline |
|---|---:|---:|
| `XYO_EVENT_SCHED=0` | 147.24 | 1.00× |
| `XYO_EVENT_SCHED=1` only | 93.40 | 0.63× |
| Both event and dep flags ON | **840.03** | **5.71×** |

Raw `build/perf-wait-dep-ab.json`.

The same current executable completed two **nonconcurrent** 47-case full-suite runs, once with both flags and once with both disabled:

- OFF: `scratch-bench/results/perf-wait-dep-off-20261108/performance-xyo-cpp.json` — `sched_waituntil` **177.23 ops/ms**, overall **96,809**.
- ON: `scratch-bench/results/perf-wait-dep-on-20261108/performance-xyo-cpp.json` — `sched_waituntil` **1093.97 ops/ms** (**6.17×**), overall **99,839**. Against TurboWarp's previously stored highest **324.81 ops/ms**, this is **3.37×**.
- All 47 categories: **28 wins, 19 losses** against saved five-rival values. `scratch-bench/results/perf-wait-dep-rivals-20261108.{md,json}`. Other agents were editing math code concurrently and benchmark results include normal run-to-run variability; rivals were measured on previous dates and these are not guaranteed contemporaneous wins.
- Whole-suite ON/OFF geometric mean ratio **1.040**, heavily influenced by the wait case; a handful of unrelated cases regressed in this one-run comparison. This is another reason not to turn the global event scheduler on by default.

## Correctness

- Native release `cmake --build build-perf-all --target xyo xyo-tests -j4`: PASS.
- Focused `XYO_EVENT_SCHED_DEP=1 ctest --test-dir build-perf-all -R '^xyo-tests$'`: **1/1 PASS (45.17s)**, `build/perf-wait-dep-ctest.log`.
- Full flags `XYO_EVENT_SCHED=1 XYO_EVENT_SCHED_DEP=1`: **1/1 PASS (48.55s)** after accounting for the distinct Fiber scheduling design. `build/perf-wait-dep-full-ctest.log`.
- GUI ScratchBench parity with both flags: **2121/2123 (99.9%)**; same preexisting `Looks: Size bounds per costume` failures 8/9. `build/perf-wait-dep-parity.log`, suite hash `aeaa86a89eca6afe`.
- Ordinary scheduler rebuild: **PASS**; final default CTest **1/1 PASS (72.57s)** (run-to-run CPU-load variation), logs `build/perf-wait-final-build.log` and `build/perf-wait-final-ctest.log`. Default final GUI parity: **2121/2123**, same Looks 8/9, log `build/perf-wait-final-parity.log`. A final opt-in isolated `sched_waituntil` probe measured **952.96 ops/ms** with `[event-sched] skipped=45077507 woken=129492`; raw `build/perf-wait-final-optin.{out,err}`.
- `git diff --check` was clean before the final rebuild.

The shared worktree has unrelated in-progress changes by agents `RT-001`, `IR-001`, `IR-002`, `MATH-005`. Edits were restricted to the eventWait conditional and Thread snapshot fields; other agents' continuation/deopt behavior was preserved. No commit or push.

## Next remaining gaps

With both experimental flags, the worst remaining ratios to saved fastest rivals were `algo_primes` (0.56×), `app_life` (0.56×), `sched_clonechurn` (0.59×), `sched_fanwait` (0.60×), `algo_sieve` (0.61×) and `sched_looptick` (0.62×). Achieving **47/47** remains an open task.
