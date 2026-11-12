# xyo-cpp: stackless JIT migration, stage 1

Date: 2026-11-12
Scope: migrate away from machine-stack Fiber continuation in the scheduler.
Status: experimental **migration harness and LLVM lowering proof**, not a
finished high-performance stackless LLVM runtime. Default is unchanged.

## Why simply deleting Fiber is wrong

Current `jitEmitScript` produces `void fn(JitCtx*)` functions.
Generated loop checkpoints call `xyo_yield_fast`/`xyo_yield`, which return
to the very same LLVM instruction only because `Fiber` preserves
the entire native activation stack. Native helpers such as
`xyo_broadcast_wait_v`, `xyo_rt_wait`, `xyo_wait_event_yield`,
and procedure/reporter helpers may themselves suspend from nested calls.

Switching `xyo_yield` to a plain C++ return would either restart the
script and duplicate committed effects or lose LLVM values needed
after the yield. A portable continuation needs a valid resume PC,
correct per-frame variables and GC-root ownership.

## Stage 1: explicit fiber-free execution baseline

`XYO_JIT_FIBERLESS=1` is now recognized in the JIT and scheduler.

- Native JIT scripts with `CompiledScript::mayYield == false` continue
  running synchronously: they already need no Fiber.
- Existing `NumericThreadSession` common-CFG slices and WASM stackless
  execution are preserved.
- Native scripts that may suspend are executed by the existing
  *stateful Scratch interpreter* instead of attempting to attach a
  machine-stack Fiber. This is a conservative fall-back, not fast JIT.
- The runtime's delayed JIT adoption/OSR path skips these functions,
  so a script already partly interpreted cannot restart its native
  entry and replay effects.
- Directly dispatched, fresh native scripts get a defense-in-depth
  fallback in `Sequencer::jitStepThread()`.
- `FiberPool::fiberlessAcquires()` counts any unexpected attempts to
  create a Fiber during a fiber-free run. With
  `XYO_JIT_FIBERLESS_STATS=1`, process-exit diagnostics report the
  number of suspending JIT scripts intentionally interpreted and
  the number of attempted Fiber acquisitions.

A representative ScratchBench clone/fanout smoke project completed
successfully:

```
[fiberless-migration] interpreted_yield_scripts=150502 fiber_acquires=0
```

The zero-Fiber claim is specific to this measured workload and
execution mode, not to every possible extension/host invocation.

## Actual performance: migration harness is NOT ready for use

CPU16, eager JIT, fixed identical scheduler flags, three alternating
ON/OFF pairs, untouched ScratchBench scheduler kernels.
Only `XYO_JIT_FIBERLESS` toggled.
Results: `build-scheduler-rewrite-20261111/fiberless-migration-ab-20261112/results.json`

| Kernel | Baseline median ops/ms | Fiber-free median | Paired ON/OFF |
| --- | ---: | ---: | ---: |
| sched_looptick | 3516.7 | 1027.7 | 0.2948x |
| sched_pingpong | 432.2 | 274.7 | 0.6485x |
| sched_restart | 1093.8 | 259.1 | 0.2379x |
| sched_clonechurn | 411.9 | 314.4 | 0.7722x |
| sched_threads | 10342.1 | 1500.7 | 0.1457x |
| sched_fanout | 733.1 | 707.8 | 0.9664x |
| sched_fanwait | 726.1 | 698.7 | 0.9641x |
| sched_waituntil | 3679.4 | 33.4 | 0.0091x |

These regressions are expected when actual LLVM continuation is missing.
**Do not enable this mode for performance.** It exists to provide a
correctness reference and to distinguish Fiber-free code coverage from
the old continuation.

Experimentally adding `XYO_JIT_COMMON_CFG=1` to the same conservative
mode did not help the tested sched_looptick, sched_threads and
sched_waituntil kernels (all three ratios below 1 on two paired runs).
That flag is not turned on automatically.

## Stage 2: LLVM switched-resume coroutine proof

New file: `tests/platform/fiberless_llvm_smoke.ll`.
The IR contains a coroutine whose loop counter is an SSA PHI value
carried across two actual `llvm.coro.suspend` boundaries. A caller
runs it once and twice more using `llvm.coro.resume`, checks the
visible counter is exactly 1, 2, 3 at each boundary, checks
`llvm.coro.done`, and finally destroys the coroutine.

Reproduction:

```sh
opt -S '-passes=default<O2>' \
  tests/platform/fiberless_llvm_smoke.ll \
  -o build-scheduler-rewrite-20261111/fiberless-smoke-opt.ll
TMPDIR=\"$PWD/build-scheduler-rewrite-20261111\" \
  clang build-scheduler-rewrite-20261111/fiberless-smoke-opt.ll \
  -o build-scheduler-rewrite-20261111/fiberless-smoke-exe
./build-scheduler-rewrite-20261111/fiberless-smoke-exe
```

All commands passed; smoke process return code **0**.
The optimized IR contains actual generated
`count_co.resume` and `count_co.destroy` functions, with a
32-byte heap coroutine frame storing its state. There is
no `Fiber::resume`/native stack reservation.

This experiment uses the installed opt/clang toolchain, and does
not yet imply that the full xyo LLVM ORC emitter can execute
arbitrary Scratch source as a coroutine.

## Native LLVM migration design

1. **Selection / source proof.** Initially compile only a strict set
   of top-level scripts with simple loop-yield CFG and no indirect
   calls that suspend. Retain ordinary no-yield direct JIT for the rest.
2. **Coroutine code generation.** Provide a second entry ABI returning
   an opaque coroutine handle. Mark the function with
   `presplitcoroutine`; emit `llvm.coro.id`, `llvm.coro.begin`,
   `llvm.coro.suspend`, `llvm.coro.end` and the normal LLVM
   optimization/splitting pipeline.
3. **Per-thread scheduler state.** Store the heap coroutine handle and
   compiled source revision in the owning `Thread`. `Runtime`
   resumes via `llvm.coro.resume` and tests `llvm.coro.done`,
   then invokes `llvm.coro.destroy` on completion/cancel. A source
   edit must invalidate old continuations without replaying writes.
4. **Split yielding native helpers.**
   - `wait until`: condition evaluation / dependency registration,
     then explicit coroutine suspension and resume at the same source.
   - `broadcast and wait`: launch recipients once and retain join
     identities as **heap-owned** continuation fields, then resume
     checking completion. Never launch twice after a yielded sender.
   - timed `wait`: save its logical deadline, not a C++ stack address.
   - nested procedures, reporter returns and warp behavior: represent
     call frames and live return sites explicitly, with source-verified
     checkpoints. Reject dynamic/unsupported nesting until proven.
5. **GC rooting.** A coroutine frame's raw machine pointers are NOT
   by themselves safe GC roots; the GC must have an ownership-proof
   for every live boxed string/list/reference across suspension.
   Incorrectly treating LLVM SSA pointer locations as roots was
   already shown unsafe in separate RT-001 negative controls.
6. **Parity and performance.** Require exact Scratch output and
   event order before expanding coverage; compare all eight
   `sched_*` kernels on the same CPU and frozen binary, then run
   full ScratchBench parity and sanitizer tests.

Keep the old Fiber path as the default **only while** the actual
LLVM continuation backend is being implemented. It is not necessary
to retain stackful Fibers once the coroutine backend has proven
equivalent semantics across supported scripts.

## Files changed in this stage

- `src/jit/jit.hpp`: opt-in no-Fiber policy
- `src/engine/thread.cpp`: guard compiled native attachment and
  provide migration fallback diagnostics
- `src/engine/runtime.cpp`: prevent OSR reattachment to a native
  suspending function
- `src/engine/sequencer.cpp`: safety net on direct fresh dispatch
- `src/engine/fiber.cpp/.hpp`: audit attempts to allocate a Fiber
- `tests/platform/fiberless_llvm_smoke.ll`: independently verified
  LLVM coroutine lowering and resume/destroy behavior
- `docs/scheduler-fiberless-migration-2026-11-12.md`: this plan

No commits were created; no concurrent agent's worktree changes
were reset.

## Full ScratchBench scheduler smoke — all eight kernels

A separate full-suite command, using the frozen A/B executable with
`XYO_JIT_FIBERLESS=1` and `XYO_JIT_FIBERLESS_STATS=1`, exited successfully
with **all 8 `sched_*` result rows present**. The process reported:

```
[fiberless-migration] interpreted_yield_scripts=259838 fiber_acquires=0
```

This explicitly confirms the opt-in fallback executes all eight benchmark
kernels without allocating a native Fiber **on this measured path**. It does
not establish that no other host/extension code path could reach
`FiberPool::acquire`, nor that a full LLVM coroutine backend is ready.

Raw output and stderr:
`build-scheduler-rewrite-20261111/fiberless-full-smoke.out` and
`build-scheduler-rewrite-20261111/fiberless-full-smoke.err`.

The ordinary/default `xyo-tests` CTest target (with
`XYO_YIELD_QMODE=off`) **passed in 56.18 s**, showing that adding the
experimental flag leaves normal JIT tests unchanged when disabled.
The separately compiled LLVM coroutine smoke executable returned 0.

**Decision**: do not enable this interpreter-fallback mode by default.
The next implementation milestone must be a real compiled stackless
continuation for yielding LLVM code, with precise resume state and proven
GC roots. The current opt-in switch is only a migration baseline.
