# START-019 — Scoped mutable single-block access (2026-11-11)

## Intent

AOT v3 source revision certificates detect a semantic editor change only
if it is committed through Blocks::resetCache / StructuralEdit. START-018
made allBlocks() read-only outside StructuralEdit. START-019 reduces the
remaining mutable getBlock() call sites while preserving legacy compiler
interfaces and explicitly separates layout changes from semantic changes.

## Implementation

1. src/model/blocks.hpp:
   - Added Blocks::StructuralEdit::getBlock(id) -> Block*, backed by the
     owning Blocks map. The returned mutable pointer is only intended for
     use before the scope commits, and the scope publishes exactly one
     source revision after all nested writes.
   - Documented public Blocks::getBlock(id) -> Block* as a compatibility
     interface that must not be used for new semantic writers. It is not
     removed in this checkpoint; this is an incremental migration, not a
     proof that arbitrary raw writes are impossible.

2. src/blockly/ops.cpp:
   - insertAfter, insertBefore, attachToValue, attachToSubstack and
     makeTopLevel obtain every writable Block* through edit.getBlock().
     Existence/input preflights occur before entering the scope. Nested
     Blockly calls remain coalesced into the outer editor edit.

3. src/shell/editor.cpp and src/shell/editor_sdl.cpp:
   - Migrated mutable block graph access inside deleteBlockChain,
     deleteSelectedBlock, detachDrag, doDrop and cancelDrag to edit.getBlock().
   - The source of dupRec() is explicitly const Block* because that function
     only copies the original block; structural mutation is performed via
     createBlock and the caller's existing edit scope.
   - Migrated instBlock, backpackPaste, makeProcedure and monitor field
     writes to explicit scoped mutation; removed now redundant manual
     resetCache calls where appropriate.
   - Primary SDL editor: deleteAllBlocks coalesces its deletions into one
     transaction and copies the getScripts() IDs before deleting, avoiding
     iterator invalidation when deleteBlock modifies the original vector.
   - Headless editor selftest checks multi-root delete-all, single source
     revision advance, and Undo restoration.

4. src/jit/rt_blocks.cpp:
   - Monitor isMonitored updates take a monitorBlocks StructuralEdit scope,
     and the old manual resetCache is no longer necessary.

5. src/model/blocks.hpp and src/model/blocks.cpp:
   - Added setBlockPosition(id,x,y) for purely visual x/y changes.
     Position changes are not executable Scratch semantics; they do NOT
     bump the revision or invalidate native code. This prevents a cosmetic
     workspace cleanup from aborting a live AOT Fiber.
   - src/shell/editor.cpp cleanUpBlocks calls this new API, not a source
     edit scope.

6. tests/platform/aot_structural_edit_transaction_smoke.cpp:
   - Compile-time check that StructuralEdit::getBlock returns Block* and a
     const Blocks::getBlock returns const Block*.
   - Runtime checks for nested versioned pointer writes, current
     BlockCached snapshots, stale AOT resolver rejection and unchanged
     revision/cache identity for setBlockPosition.

## Remaining restrictions

- The legacy public mutable Blocks::getBlock still exists. Full compiler-
  enforced prevention of raw unversioned semantic writes will require
  const-correct migration of many compiler and editor call sites before
  removing that overload. This checkpoint does NOT claim full isolation.
- Some temporary GPU compatibility rewrite paths alter source Block*
  values and restore them, and must be audited separately for active
  workers and AOT source certificates.
- StructuralEdit is an editor/scheduler safe-point API and is not a
  cross-thread reader-writer lock. Mutation during active worker batches
  is not supported.
- Cosmetic x/y changes deliberately do not advance AOT source revisions.
- AOT v3 remains opt-in; AOT v2 stays default. All work is uncommitted.

## Verification

Verified on the final source state in the shared worktree:
- Regular LLVM/JIT CLI and AOT test targets: build PASS.
- Regular CTest set (editor source revision, native editor operations,
  native AOT cache, structural edit, parallel/serial/trim): **7/7 PASS**,
  recorded in build/start019-final-ctest.log.
- Optional yyjson metadata CLI and native tests: **8/8 PASS**,
  recorded in build/start019-final-fastmeta-ctest.log.
- No-JIT CLI: compile and link PASS,
  recorded in build/start019-final-nojit-build.log.
- Legacy SDL editor implementation: standalone C++20 syntax check PASS,
  in build/start019-legacy-editor-syntax.log. This is NOT a full runtime
  test of the legacy SDL editor.
- Frozen native AOT v2 full Scratch GUI parity: **2656/2656 PASS**.
- Frozen native AOT v3 full Scratch GUI parity: **2656/2656 PASS**,
  with parallel enabled and required lazy-materialization diagnostics.
- git diff --check on changed source paths: PASS.

Logs: build/start019-final-{v2,v3}-parity.log,
build/start019-final-build.log, build/start019-final-ctest.log,
build/start019-final-fastmeta-{build,ctest}.log.

A source project and its AOT object must match exactly; no AOT object
was regenerated or substituted during the full parity comparisons.

Reproduction from /home/kicky/dev/cpp/xyo-cpp:

    cmake --build build -j4 --target xyo-cli xyo-aot-lazy-cache-smoke xyo-aot-lazy-parallel-smoke xyo-aot-structural-edit-smoke
    ctest --test-dir build -R '^xyo-start016-editor-source-revision$|^native-editor-operations$|^xyo-aot-lazy-(parallel|cache)|^xyo-aot-structural-edit-smoke$' --output-on-failure

    XYO_PAR=1 XYO_AOT_LAZY_EXPECT=1 XYO_AOT_LAZY_DIAGNOSTIC=1 python3 tests/differential/gui_scratch_parity_regression.py --native build/xyo --project build/start013-frozen/parity.sb3 --aot-object build/start013-frozen/v3.o --minimum 2656 --modes aot

Do not commit, reset, overwrite or push unrelated concurrent worktree edits.
