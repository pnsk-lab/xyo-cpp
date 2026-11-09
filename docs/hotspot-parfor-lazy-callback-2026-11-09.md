# BUG-001: HotSpot / par-for lazy callback SIGSEGV (2026-11-09)

## Symptom

ScratchBench `performance.sb3` against the **published xyo-cpp v1.2.2**
Linux binary intermittently aborted with exit code 139 under HotSpot JIT.
Interpreter, Eager JIT and AOT completed the same suite. A later HotSpot
v1.2.2 trial completed successfully; this is a nondeterministic failure.

Input and environment:

- Binary: `scratch-bench/.cache/runtimes/xyo-release/xyo-linux-x86_64/xyo`
- Project: `scratch-bench/.cache/build/performance.sb3`
- Command: `XYO_JIT_MODE=hotspot xyo run performance.sb3 --print-list output --fps 30`
- Failing result: `scratch-bench/results/2026-11-09T14-11-28-009Z/performance-xyo-cpp.json`.
- Passing release result: `scratch-bench/results/2026-11-09T14-13-59-593Z/performance-xyo-cpp.json`.

## Native crash evidence

Real systemd-coredump captures from two independently failing runs:

- PID 1225450 (2026-11-09 23:10 JST), GDB backtrace in shared
  `board/performance-hotspot-1225450-gdb.log`
- PID 1226806 (23:11 JST), GDB backtrace in
  `board/performance-hotspot-1226806-gdb.log`

Both crashed at a page-aligned, **inaccessible generated-code address** called
from the same native helper:

```
#0 0x7f5a2d737000 in ?? ()  # inaccessible JIT callback address
#1 xyo_par_for()           # call *%r12, offset +876
#2 generated LLVM JIT code

rbx = 1000  # trip count
r15 = 1     # immediately after first attempted callback
r12 = 0x7f5a2d737000  # invalid callback target
```

`trip=1000` takes the **sequential path** of `xyo_par_for`, whose worker
threshold defaults to 2048. The crash is therefore not contingent on multiple
workers writing a list simultaneously: the host C++ runtime is invoking an
unmaterialized or otherwise non-executable internal LLVM function pointer.
The kernel reports `SEGV_ACCERR` (access protection), so the core
alone cannot distinguish an unmapped page from a mapped page with
insufficient execute permission.

## Cause and mitigation

The JIT compiler emits an `InternalLinkage` per-iteration body and passes its
address to the native host `xyo_par_for`. HotSpot's default lazy tier can
submit the containing IR module to LLVM ORC `LLLazyJIT`, which materializes
individual functions on demand. A function pointer escaping to C++ as a
callback does not have the same call-through guarantees as a generated LLVM
direct call. This is the likely mechanism; the exact ORC internals that
invalidated the pointer have not been independently proved.

In `src/jit/backend_llvm.cpp` (`LLVMBackend::compile`), detect whether
the emitted IR actually calls `xyo_par_for`. In that case only, skip the
lazy-module path and use the ordinary full `LLJIT` materialization before
exposing the code to C++. This guard takes precedence over explicit
`XYO_JIT_LAZY_MODE=on`.

The native par-for optimization stays enabled, and unrelated scripts still
use lazy materialization. Optional `XYO_JIT_LAZY_CALLBACK_TRACE=1` logs
the guard decision. This is a conservative correctness fix, not a blanket
disable of HotSpot or par-for.

For the published unpatched v1.2.2 binary, either
`XYO_JIT_LAZY_MODE=off` (retain par-for) or `XYO_PARFOR=0` (retain lazy
JIT) is a temporary mitigation. Both settings completed the real
ScratchBench workload in one controlled attempt each; no statistical
reliability claim is made from a single attempt.

## Regression coverage

Added `tests/differential/jit_lazy_parfor_callback.py` and
`xyo-jit-lazy-parfor-callback` CTest. Its generated Scratch program has a
warp procedure with `repeat(1000)`, induction-variable increments and
`add to list` for each iteration. It runs Interpreter, Eager JIT and HotSpot
with lazy mode forced on, proving **all 1000 list values and their order**,
not just the final counter.

- The callback path must emit the guard diagnostic, compile as
  `tier=eager`, and actually dispatch a sequential `trip=1000` callback.
- A second, no-callback procedure must still compile as `tier=lazy` under
  forced lazy mode.
- Published v1.2.2 correctly triggers the test's red condition (lacks the
  guard and compiles the callback script in the lazy tier).
- Modified v1.2.2: standalone test **PASS**, related CTest **7/7 PASS**.

Real workload tests using the published performance fixture and the isolated
corrected binary:

| Run | Result | Elapsed |
|---|---|---:|
| Initial fixed A/B | DONE | 34.01 s |
| Repeat 1 | DONE | 35.22 s |
| Repeat 2 | DONE | 34.27 s |
| Repeat 3 | DONE | 34.04 s |

Every fixed run confirmed the guard was exercised. Additional validation:

- LLVM 23, Release, SDL2 ON: **complete CTest 114/114 PASS**, zero
  failures, 389.45s elapsed at 4-way parallelism.
  Log: `build-hotfix/bug001-full-ctest.log`.
- Focused related CTests: **7/7 PASS**, including the new callback
  regression, JIT lazy integration, type SSA, GUI Scratch parity,
  daysSince2000, unit tests and par-for project fixture.
  Log: `build-hotfix/bug001-targeted-ctest.log`.
- ScratchBench **official CLI**, using the same performance project source hash
  `e8bd5be3e7345088`, custom binary pointing to the patched
  `build-hotfix/xyo`. All four variants: **1/1 PASS each**.

  | Mode | Startup (ms) | Runtime (ms) | Peak RSS (MB) |
  |---|---:|---:|---:|
  | HotSpot JIT | 3246.5 | 31545.7 | 147.1 |
  | Interpreter | 135.7 | 31984.6 | 115.1 |
  | Eager JIT | 3277.9 | 31541.0 | 146.0 |
  | AOT | 2862.0 | 31512.0 | 161.8 |

  Log: `build-hotfix/bug001-scratchbench.log`, JSON:
  `build-hotfix/scratchbench-results/performance-xyo-cpp*.json`.
  These are single-run figures, not reliable across-run performance changes.
  The memory readings from the user's earlier run differ **even in Interpreter**,
  so they cannot be attributed to this JIT fix.

The tradeoff is that scripts with a par-for callback use eager full-module
materialization under HotSpot, increasing JIT startup cost versus lazy on
those specific scripts. Other scripts retain the on-demand tier.
The isolated fix has **not** been published as a new release or pushed to
the remote repository.

## Scope

All source changes are in the isolated Git worktree
`/home/kicky/dev/cpp/xyo-cpp-hotspot-parfor-fix`, based on
`d9de9cb` (v1.2.2). The shared, heavily dirty source tree was not modified.
No release/tag/remote push was performed by BUG-001.
