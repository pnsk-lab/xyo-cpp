# START-015 — Native AOT v3 source-edit safety (2026-11-11)

## Scope

START-011–014 implemented the opt-in native AOT v3 lazy BlockCached
descriptors, parallel worker prewarm and an optional fast metadata reader.
This checkpoint addresses editor changes **after** loading an AOT object.

Previously, an unresolved descriptor would resolve its block identifier from
the *current* Blocks cache even after the source changed. Already resolved
descriptors retained pointers to previous cache generations. Both cases
could run stale native instructions, and a missing block could yield a null
BlockCached* to a generated compatibility helper that assumes non-null.
A separate JitEngine::invalidate() path cleared CompiledScript allocations
while live Thread objects could still hold raw pointers to them.

## Safety policy

A nonzero monotonically increasing Blocks::revision() captured at native AOT
load is an execution certificate for the **source owner** of each v3
CompiledScript and each lazy BlockCached descriptor.

- Fresh AOT root execution after a public source edit/resetCache(): refuse
  the old native entry and run the current source through the interpreter.
  The old native script's side effects have not begun, so this is safe.
- Suspended native Fiber or any already-started native computation after a
  source edit: **retire the old thread** and release its native Fiber/JitCtx.
  Arbitrary LLVM state cannot be reconstructed at a source edit boundary;
  replaying the root could duplicate committed side effects. The old thread
  is not resumed; **this is deliberately not transparent continuation**.
- Native parallel batches: reject a stale AOT script before using cached
  worker eligibility. Never resolve a source-edited lazy descriptor on a worker.
- Cached script reuse, fast broadcast/clone receivers and receiver restarts:
  reject stale v3 code; a restarted receiver must discard a potentially
  dangling pre-edit topBlock hint and read the current top from Blocks.
  readyScript(top, currentBlocks) now requires the exact live Blocks owner
  to return a v3 native entry. Calling readyScript(top) without an owner
  returns null for v3 (but keeps non-AOT JIT lookups compatible), avoiding
  dereferencing an old Blocks* after target disposal/replacement.
- JitEngine::invalidate() (also used by GPU real-time backend installation):
  remove invalidated code from the active compilation cache, mark its source
  owner invalid, and move v3 CompiledScript ownership into a lifetime-retained
  vector. Live Thread/Fiber raw pointer addresses remain stable for their
  final conservative rejection. This is a bounded per-loaded-v3 generation
  retention until JitEngine teardown.
- The low-level xyo_rt_aot_cached() now requires a mandatory source revision;
  even previously resolved entries return nullptr if that source revision
  is stale or missing. The scheduler must reject stale generated code
  *before* a null resolver result could flow into a native compat caller.

The guard is v3-specific. Existing v1/v2 AOT formats and their semantics
were not redefined. Default AOT remains v2; v3 stays explicitly opt-in.

## Implementation

- src/jit/aot_lazy_cache.hpp: mandatory immutable revision in descriptors.
- src/jit/rt.cpp: reject missing/stale stamps on the hot path and after the
  first-resolution lock.
- src/jit/jit.hpp / src/jit/jit.cpp: record script owner+revision on v3 load,
  disallow stale native cached script reuse and readyScript lookup, and retain
  invalidated v3 code objects to avoid dangling live Thread pointers.
- src/engine/sequencer.cpp: source-stamp check before native entry or Fiber
  resume, plus stale-worker rejection; fresh roots demote to interpreter,
  in-flight native work retires without replaying effects.
- src/engine/runtime.cpp: stale broadcast receiver restarts must not borrow
  old native code or stale Block* top hints.
- tests/platform/aot_lazy_cache_smoke.cpp: previously resolved descriptors,
  missing source, and delete/recreate with the same block ID.
- tests/platform/aot_lazy_parallel_smoke.cpp: real v3 AOT with eight worker
  scripts, edits of add -> subtract after greenFlag and before first native
  entry; all final variables equal the edited interpreter semantics and no
  stale worker dispatch occurs. Also exercises a **real suspended native
  Fiber**, a stale receiver restart and active CompiledScript retirement on
  JitEngine::invalidate().
- src/jit/codegen.cpp: a separate 2-line LLVM 23 compatibility correction to
  the in-progress IR-001 list-read diagnostic path (CreateGlobalString and
  FunctionCallee, not the removed CreateGlobalStringPtr or auto* decl).

## Verified

- Related native CTest: 4/4 PASS in the regular build with parallel ON,
  parallel OFF and malloc_trim ON.
- Real suspended native AOT Fiber: the scheduler logs action=retire after a
  source revision bump; subsequent Fiber/JitCtx handles are released.
- Changed edit-before-run source: eight v3 roots fall back to interpreter,
  produce the edited negative recurrence, and dispatch zero stale workers.
- Native v3 ScratchBench GUI parity: 2656/2656 PASS under XYO_PAR=1, using
  the exact frozen project and previously compiled v3 object.
- Optional fastmeta build: AOT lazy parallel, serial, fastmeta and trim smoke
  tests PASS. This build uses the same source but optional yyjson metadata.
- Legacy v2 native full ScratchBench parity: 2656/2656 PASS, recorded in
  build/start015-v2-parity.log (source guards do not change the v2 format).
- Test binaries are rebuilt from the working tree: do not infer correctness
  from a stale CTest executable.

## Follow-up compatibility fix: IR-001 list-read evidence

The no-JIT CLI previously failed to link because `Thread::releaseFinished()`
included the IR-001 list-read checkpoint audit header, which transitively
included LLVM IR headers and emitted a reference to
`llvm::DisableABIBreakingChecks`. This was an unintended compiler dependency
in a runtime-only translation unit.

Extracted the GC-rooted runtime event recorder into
`src/ir/source_list_read_events.hpp`, using the already pointer-free
`source_reporter_immediate.hpp` helper. The existing
`source_list_read_index_trace.hpp` retains the LLVM-backed
`SourceListReadCheckpointAudit` under `XYO_JIT` while including the shared
runtime event structure. `src/engine/thread.cpp` now includes only the
new runtime event header.

Verified on the actual shared source tree:

- `cmake --build build/start013-nojit -j4 --target xyo-cli`: PASS, linked.
- `cmake --build build -j4 --target xyo-cli`: PASS, linked.
- `xyo-ir001-ssa-location-bridge-smoke`: PASS (1/1).
- Native AOT cache/parallel smoke tests: PASS (4/4).
- Optional yyjson fastmeta AOT parallel tests: PASS (4/4).
- Native v2 full real GUI Scratch Parity: PASS (2656/2656).
- Native v3 full real GUI Scratch Parity: PASS (2656/2656),
  including the explicit `XYO_AOT_LAZY_DIAGNOSTIC=1` and
  `XYO_AOT_LAZY_EXPECT=1` check that lazy descriptors do not eagerly
  materialize BlockCached entries at load.

Logs: `build/start015-recheck-v2.log`,
`build/start015-recheck-v3.log`,
`build/start015-final-build.log`,
`build/start015-final-ctest.log`,
`build/start015-ir001-bridge-build.log`.
Note that `XYO_AOT_LAZY_EXPECT=1` without
`XYO_AOT_LAZY_DIAGNOSTIC=1` is a test-harness configuration error:
the diagnostic assertion requires the explicitly emitted load summary.

## Limits and remaining work

1. This guarantee applies to **public structural edits** that advance
   Blocks::revision(), normally through createBlock(), deleteBlock(),
   restoreState() or resetCache(). Raw in-place modifications to Block
   payloads that bypass resetCache() have no source revision signal;
   external editor integrations must call resetCache() at their mutation
   commit boundary. A future content digest or mutation API would close
   this gap.
2. Edits must occur at a **host scheduler safe point**, after active parallel
   workers have joined. The Blocks graph and its revision counter are not a
   thread-safe concurrent-editor transaction protocol. This change does not
   authorize unsynchronized writes while native worker code is running.
3. When a previously executing native Fiber becomes stale, the operation is
   aborted rather than transferred to the new interpreter source. Preserving
   the partially executed state requires an independently certified
   source-level continuation and remains separate work.
4. Source edits to variables, targets and other structures outside Blocks
   have their own runtime lifetime and binding rules and are not proven by a
   Blocks revision. This checkpoint does not claim all editor operations
   are safe for AOT v3.
5. The invalidation retention specifically protects v3 CompiledScript*
   objects; other JIT backends have separate lifetime policies.

Do **not** make v3 the default based solely on this checkpoint.

## Reproduce

From /home/kicky/dev/cpp/xyo-cpp:

```sh
cmake --build build -j4 --target xyo-cli \
  xyo-aot-lazy-cache-smoke xyo-aot-lazy-parallel-smoke
ctest --test-dir build -R 'xyo-aot-lazy-(parallel|cache)' \
  --output-on-failure

XYO_AOT_LAZY_DIAGNOSTIC=1 XYO_AOT_LAZY_EXPECT=1 XYO_PAR=1 \
  python3 tests/differential/gui_scratch_parity_regression.py \
    --native build/xyo --project build/start013-frozen/parity.sb3 \
    --aot-object build/start013-frozen/v3.o \
    --minimum 2656 --modes aot
```

Uncommitted changes remain in the **shared** xyo-cpp worktree. No files from
other active workstreams were reset and no commits/pushes were performed.
