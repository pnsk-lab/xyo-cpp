# WarpClockWatchdog: constructor publication race (TSan)

Parent commit: `bd15a8f`. Isolated branch:
`work/warp-watchdog-tsan-v123`.

## Root cause

`WarpClockWatchdog` previously declared its worker thread **before**
`armed_`, `stopped_`, `active_`, `generation_`, `deadline_` and
`unlimited_`. The constructor launched `worker_([this] { run(); })`
in the initializer list. Because C++ initializes members in *declaration
order*, `run()` could read `stopped_` or `active_` before the main
thread had initialized those members.

This is not caused by the par-for optimization; the watchdog starts
for ordinary HotSpot JIT warp timekeeping. It is a native C++ data race
and undefined behavior even when most tests happen to pass.

## Fix

Move `worker_` to the end of the class data members and launch it in
the constructor **body**, after all members have been initialized.
The existing mutex, generation checks, atomic sampled values,
arm/disarm behavior and destructor join remain unchanged.

## Red/green evidence

Build the same 256-cycle
`tests/platform/warp_clock_watchdog_smoke.cpp` against the original
version and fixed version of the two watchdog sources with Clang 23
ThreadSanitizer, `-fsanitize=thread`:

| Version | Result |
|---|---|
| Original `bd15a8f` | **TSan data race** during constructor initialization; exit **66** |
| Fixed | **PASS** 256 construct/arm/disarm/join cycles; exit **0** |

The original stack points to `warp_clock.hpp:52` during member
initialization. Building the fixed standalone test with GCC 16
ThreadSanitizer also produces **PASS**, exit 0.

Native LLVM 23 Release build: **PASS**.
CTest `xyo-warp-clock-init-smoke`: **1/1 PASS**.
Full 64-round par-for regression on the fixed Release binary:
Interpreter / Eager / guarded HotSpot / opt-in HotSpot
**all matched**, including 128 native worker-pool dispatches and
5 independent opt-in runs.

## CI protection

- `xyo-warp-clock-init-smoke` is registered for native builds via CTest,
  and therefore runs in the existing Linux full-test job.
- The full Linux job also compiles only the host watchdog and this
  smoke test using GCC `-fsanitize=thread` and executes it as a
  dedicated fail-on-TSan-warning step, avoiding unrelated JIT/LLVM
  instrumentation issues.
- The branch is included in the workflow's push triggers to run all
  platform jobs without creating a PR.

## Separate TSan findings: not fixed in this commit

We also built the **entire** xyo-cpp executable with Clang 23
ThreadSanitizer to run the full par-for regression.
With the watchdog fix, Interpreter, Eager and guarded HotSpot passed
64 rounds. The experimental lazy-partitioned HotSpot reported another
TSan race between `LLVMContextImpl` destruction and an ORC worker
reading `ConstantDataSequential::isCString()`. This appears during
lazy code materialization, distinct from the watchdog constructor.

Setting `XYO_JIT_ISOLATED_CONTEXT=0` instead exposed another TSan
report in `xyo_jitVar`. These require separate investigation.
LLVM JIT-generated machine code is not necessarily TSan-instrumented,
so a clean full-executable TSan run would also not constitute complete
proof against data races.

No global TSan-clean claim is made, and no automatic enabling of the
par-for lazy path is included. No change to `main` or to the original
pinned ScratchBench parity gate is included.
