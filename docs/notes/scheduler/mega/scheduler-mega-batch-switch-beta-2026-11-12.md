# Mega Batch β: 複数Scratchスクリプトを1つのLLVM switch(pc)へ統合

Date: 2026-11-12
Status: Experimental, opt-in, uncommitted.

## Current implementation

The preceding `XYO_JIT_MEGA_DISPATCH=1` merges **procedure bodies within
one script**. This next stage, `XYO_JIT_MEGA_BATCH=1`, extends the idea
to **multiple distinct green-flag Scratch scripts** on the same Target.

At green-flag startup, `JitEngine::installMegaDispatch` collects 2–8
independent, eagerly compiled, *non-suspending* native scripts belonging to
the same target. The LLVM backend uses the source-certified code generator
to construct one private LLVM function per script in a temporary Module,
then constructs one externally exported dispatcher:

```llvm
define void @xyo_mega_batch_...(ptr %ctx, i32 %pc) {
entry:
  switch i32 %pc, label %invalid [
    i32 0, label %script_0
    i32 1, label %script_1
    i32 2, label %script_2
  ]
script_0:
  ; inlined Scratch script #0's actual LLVM instructions
...
}
```

`llvm::InlineFunction` eliminates all calls to the separately generated
script functions. Those definitions are erased before O1 module
optimization. In the exact three-hat integration fixture, the compiler
reports **definitions=1**, **slots=3**, **indirect=0**.

Each `Thread` retains its own Scratch identity, `JitCtx`, independent
`megaPc`, and status; the existing scheduler order is not changed.
The normal native no-yield dispatch site calls
`megaDispatch->entry(ctx, megaPc)` instead of its separate compiled
`ScriptFn`. All effects remain on the exact owning Scratch target.

This is **one native function reused to execute multiple Scratch scripts**;
it is not yet **one single call that continuously switches among yielding
Threads**. Wait/event suspension still uses the existing scheduler.

## Eligibility and fallback

- Flag: `XYO_JIT_MEGA_BATCH=1`, optionally
  `XYO_JIT_MEGA_BATCH_TRACE=1`.
- Eager LLVM JIT, no renderer (initial headless beta), 2–8 scripts
  per target, each a green-flag hat.
- Only scripts that the existing native compiler already proves
  `mayYield=false`, no AOT/WASM/coroutine/OSR, no GPU,
  no extra procedure symbols, no main-thread-only effects or
  instrumentation requiring per-script identity.
- If any candidate is unsupported or codegen fails, no partial batch is
  published and existing code continues to run.
- Every batch is stamped by the owning `Blocks*`, the exact
  monotonically unique source revision, and the ordered list of
  Scratch top-block IDs.
- Re-running green flag with unchanged source reuses the identical
  compiled batch. A changed source revision forbids reuse.
- Before entering native code, the Sequencer rechecks the source
  identity and revision. On a stale batch it starts the *fresh*
  interpreter source before running any native effects that have not yet
  executed.
- `JitEngine::invalidate` clears the batch cache.
- Default remains **OFF**. Existing multi-agent worktree changes
  have not been reset, staged or committed.

## Correctness evidence

Fixture: `tests/bench/algos/build_mega_batch_fixture.py`, three
independent green-flag Scratch scripts on Stage, each setting a
different variable:

```
[mega-batch] slots=3 definitions=1 indirect=0 source_revision=14 valid=1
[mega-batch] installed=3
Stage/v0 = 5, Stage/v1 = 6, Stage/v2 = 7
```

Identical full variable output in mode OFF.

New `testMegaBatchSwitchDispatch()` in
`tests/unit/test_scheduler.cpp` checks three distinct Thread objects,
one shared LLVM batch, unique PC selectors, and exact variable effects.
It also checks that two successive green flags reuse the same compiled
function and each executes the same three effects once, not twice.

One-time beta ON full `xyo-tests` passed in **47.21 seconds**.
Source-edit validation is added separately and should remain passing
before the beta is expanded.

## Performance

A/B setup:
- 8 independent one-block scripts, one Stage target.
- Identical frozen binary, source SB3, CPU16 affinity, eager JIT O1.
- 8 alternating ON/OFF pairs.
- Metric: **process total wall-clock startup plus full project run**,
  not scheduler cost per Scratch instruction.

| Metric | OFF | Mega Batch ON |
| --- | ---: | ---: |
| Median process wall | 26.856 ms | 31.173 ms |
| Geometric paired speed ratio | — | **0.8892x** |
| Pairs Mega ON wins | — | 1 / 8 |

The cold run is ~11% slower because it must compile the ordinary
independent JIT scripts *and then* generate, inline and optimize the
combined native function. The source-verified cache should reduce
this compilation overhead on later green-flag restarts within the
same Runtime; it does not eliminate cold compilation.

These initial throughput numbers **do not** demonstrate scheduler
speedup. Large gains will require fusing **yielding**, long-lived
script continuations rather than only synchronous tiny scripts.

Raw results:
`build-scheduler-rewrite-20261111/mega-batch/ab-eight/results.json`

## Files changed in this stage

- `src/jit/backend.hpp`, `src/jit/jit.hpp`:
  backend-neutral mega-batch interface and code ownership.
- `src/jit/backend_llvm.cpp`: source-verified multi-script LLVM
  switch, full inlining and exactly one exported function.
- `src/jit/jit.cpp`: target-specific selection and ordered-source
  cache, invalidate on edits.
- `src/engine/runtime.cpp`: green-flag installation after callbacks.
- `src/engine/thread.hpp`, `src/engine/thread.cpp`:
  appended per-Thread batch handle and PC, cleanup at retirement.
- `src/engine/sequencer.cpp`: native no-yield entry dispatch,
  source revision verification and conservative interpreter fallback.
- `tests/unit/test_scheduler.cpp`: native integration.
- `tests/bench/algos/build_mega_batch_fixture.py`,
  `tests/bench/algos/bench_mega_batch.py`: reproducible fixture and A/B.
- This report.

## Future work

To achieve a whole-project single-loop mega scheduler, the next ABI
must persist per-script continuation PC and live values at `yield`.
It must preserve `broadcast and wait` receiver join identities,
per-script procedure return states, warp semantics, event ordering,
GC roots, source edits and clone creation. Do not treat replacing
a native call with an unconditional goto as an equivalent yield
implementation; such a change would replay or lose Scratch effects.

Only after correctness is demonstrated should the shared dispatcher
be extended to the `sched_pingpong` / `sched_fanwait` kernels.

## Latest validation after source cache and safety guards

- The strict LLVM pre-inline check rejects any additional native helper
  definitions, so the supported batch genuinely has **one defined LLVM
  function**, not just a forwarding switch.
- Final 3-hat native run: `slots=3 definitions=1 indirect=0` and
  `installed=3 ... cache=miss`; values 5, 6, 7 exactly match OFF.
- The unit test verifies a **cache hit on the second green flag**, with
  the three scripts each incrementing their shared variable exactly once.
- A third green flag followed by a source-revision change exercises the
  interpreter fallback; the aggregate scalar result is exactly 21,
  with neither replay of committed effects nor stale LLVM execution.
- One 8-script fixture also successfully runs with a single batch;
  its source and build artifact are under `mega-batch/eight-scripts.sb3`.
- CPU 16 fixed, 8 alternating A/B pairs on 8 tiny scripts, same binary:
  OFF process wall median **26.856 ms**, beta ON **31.173 ms**,
  paired speed ratio **0.8892x**, ON wins **1/8**. This is **worse cold
  startup**, not a speedup. A source-verified compiled-batch cache avoids
  repeating the expensive compilation on the next green flag in the same
  Runtime, but a separate run-to-completion throughput win has not been
  established.
- ScratchBench sched_* 4 alternating pairs: `sched_looptick=1.0009x`,
  `sched_pingpong=0.9931x`, `sched_restart=0.9915x`,
  `sched_clonechurn=1.0009x`, `sched_threads=0.9956x`,
  `sched_fanout=1.0152x`, `sched_fanwait=1.0043x`,
  `sched_waituntil=1.0052x`. These are near-noise measurements;
  the beta targets synchronous scripts, not the yielding scheduler kernels.

Raw performance data:
`build-scheduler-rewrite-20261111/mega-batch/ab-eight/results.json`
and `build-scheduler-rewrite-20261111/mega-batch/sched-ab/results.json`.

This extension is a **source-limited real multi-script switch(pc)**,
not yet a global continuing-thread VM with stateful PC yields. It stays
OFF by default, has not been committed, and should not be called a
scheduler throughput win until long-running yield continuations can
be handled and benchmarked.

## Additional boundary checks and final safety treatment

- An automatically generated project with **nine** independent green-flag
  scripts correctly *skips* the 2–8 slot experimental dispatcher and
  runs all nine normal paths, resulting in variables v0..v8 = 5..13.
- The dispatch guard rejects any mismatch of Blocks owner, source
  revision or PC range. It never executes the existing individual JIT
  entry after detecting an invalid shared batch. A missing Blocks/Target
  owner retires the Thread rather than dereferencing freed source state.
- Feature ON CTest passed in 46.25s and OFF passed in 45.45s before the
  final fail-closed owner/PC refinement. The final ON CTest is rerun
  after that refinement; preserve its result before treating the test
  cycle as complete.

No global per-Thread yielding continuation has been implemented yet.

Final rebuilt beta-ON `xyo-tests` CTest after stricter stale-owner/PC
fallback: **PASS, 47.17s**. `git diff --check` and both new Python
benchmark scripts' syntax checks passed. Working tree remains uncommitted.
