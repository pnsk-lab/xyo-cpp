# xyo-cpp v1.2.4 — Load-Adaptive Yield Quantum (LAYQ)

## Highlights

- Load-Adaptive Yield Quantum (LAYQ) v2 for native LLVM JIT warp loops.
- `XYO_YIELD_QMODE=adaptive` is the default; `off` restores the former 64-iteration cadence; `fixed` pins a quantum for comparison.
- Scheduler publishes a low-overhead quantum hint; pure numeric loops may poll at up to 16,384 logical iterations, while call-capable loops are capped at 256 by default.
- Stop-epoch checks and quantum-scope restoration preserve the existing cooperative scheduling and deoptimization paths.
- Per-thread polling diagnostics are available with `XYO_YIELD_QSTATS=1`.

## Reported performance

The development-tree LAYQ v2 measurements (not release-CI measurements) reported roughly 54–57× for `loops_repeat`, 3.2–3.7× for `vars_setchange`, and an overall geometric-mean improvement of roughly 1.5–1.9×. Results depend strongly on workload and environment. Refer to `docs/layq.md` for the methodology, safety considerations, and remaining regressions.

## Additional build fix

- Correct mutable block lookups in nested-warp smoke harnesses so they build against the const `Blocks::allBlocks()` accessor.

## Compatibility

- Set `XYO_YIELD_QMODE=off` for the pre-LAYQ polling implementation.
- Normal, non-warp yield semantics are unchanged.
- The default is experimental pending broader multi-platform validation. See `docs/layq.md`.

## Verification

The release candidate was built separately from the shared development worktree to avoid including unrelated experiments. Its headless CLI and unit-test executables compile; standalone LAYQ policy tests pass 24/24; the nested-warp silent-edit regression passes. Broader CI and longer-running deoptimization tests are left to the release pipeline and should be checked before considering the release fully validated.
