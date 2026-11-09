# xyo-cpp v1.2.3 — HotSpot JIT par-for callback crash fix

Patch release of v1.2.2 addressing an intermittent SIGSEGV (exit code 139)
during ScratchBench's `performance.sb3` HotSpot JIT workload.

## Fix

When LLVM's lazy JIT tier receives a module that passes an internal generated
loop body to the native `xyo_par_for` helper, the callback may not be backed
by executable machine code at the time of its first call. Two independent
v1.2.2 core dumps showed SIGSEGV (`SEGV_ACCERR`) on
`xyo_par_for`'s indirect callback invocation, including the sequential
1000-iteration batch path.

The JIT now detects actual `xyo_par_for` uses in the generated LLVM module and
materializes **that script** through ordinary LLJIT rather than the lazy tier.
This applies even when `XYO_JIT_LAZY_MODE=on` is explicitly requested.
Other scripts continue to use lazy compilation when selected, and native
par-for remains enabled. This is a deliberately narrow safety fix, without
changing the default optimization settings introduced in v1.2.2.

## Verification

- Added `xyo-jit-lazy-parfor-callback` CTest. It validates all 1000 list
  appends, in order, under Interpreter, Eager and HotSpot; checks that a
  par-for callback script uses fully materialized code, and verifies a
  separate script without callbacks still uses the lazy tier.
- New regression: fails against the original v1.2.2 binary and passes with
  the fix.
- LLVM 23 Release build: **114/114 CTests passed** (SDL2 on).
- ScratchBench, same `performance.sb3` source hash
  `e8bd5be3e7345088`: HotSpot, Interpreter, Eager and AOT **1/1 passed**
  each. HotSpot completed three more independent reproductions with the
  callback guard exercised each time, plus the initial one.
- Cross-platform binaries are built by the tag-triggered GitHub Actions
  release workflow. Their status should be checked independently before
  considering the published artifacts available.

The original v1.2.2 binary can be mitigated without upgrading by choosing
`XYO_JIT_LAZY_MODE=off` (retains par-for) or `XYO_PARFOR=0` (retains lazy
JIT). Both options passed a controlled ScratchBench reproduction, although
the failure is intermittent.

See `docs/hotspot-parfor-lazy-callback-2026-11-09.md` for GDB evidence,
implementation details and local verification logs. v1.2.2 remains tagged
for reproducibility.
