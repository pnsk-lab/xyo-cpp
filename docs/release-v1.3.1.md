# xyo-cpp v1.3.1 — AOT module-verification fix (LAYQ yield scope)

## Highlights

- Fixes `xyo aot` failing with `aot: module verification failed` on projects
  that contain an empty-body `repeat` inside a procedure or par-for body
  (reproduced on scratch-bench `parity.sb3`, 479 scripts). The default LAYQ
  adaptive mode (`XYO_YIELD_QMODE=adaptive`) generated loads/stores of a
  yield-quantum `alloca` belonging to a different LLVM function.
- Same-function JIT emission verified but silently read the previous
  (ended) loop's quantum alloca after scope end — also fixed.

## Details

`Codegen::beginLoopYieldScope` pushed a `YieldScope` record and then
overwrote its `quantum`/`qmask`/`qmaxVal` fields — which exist to hold the
*outer* scope's saved values — with the new scope's own allocas.
`endLoopYieldScope` therefore restored the ended scope's alloca into
`curYieldQuantum`, leaving a stale function-local pointer. While emitting a
pending procedure or par-for body (a separate LLVM function), an
out-of-scope quantum read — the inline empty-warp-repeat scheduler — then
referenced the entry function's alloca, which `verifyModule` rejects when
building the AOT object.

Changes (all in `src/jit/codegen.cpp`):

- `YieldScope::quantum`/`qmask`/`qmaxVal` now genuinely save the outer
  scope's state; the new `qpinned` field does the same for the fixed-mode
  pinned quantum. Top level saves nullptr/defaults so out-of-scope quantum
  sites fall back to the `JitCtx` field again.
- `retargetLoopQuantum` no longer writes the retargeted cap into the saved
  outer record.
- `emitPendingProcedures`/`emitPendingParFors` save and restore
  `ctxArg`/`knownNum`/`inProc` and all yield-scope members across the
  `fn` switch (the procedure path previously did not restore `ctxArg`,
  `knownNum` or `inProc` at all).

## Verification

- `xyo aot parity.sb3 --obj` compiles deterministically
  (479 scripts / 8423 slots; identical object on repeated runs).
- `xyo run parity.sb3 --aot` scores 2313/2656 vs 2312/2656 for JIT
  (remaining delta is renderer-dependent visual tests, which need a
  display).
- `XYO_YIELD_QMODE=off` and `fixed` continue to compile the same project.

## Notes

- scratch-bench's xyo-cpp AOT variant uses the GitHub release binary;
  this release is required for `aot` runs there. Alternatively set
  `XYO_SRC`/`XYO_BIN` to a local source build.
