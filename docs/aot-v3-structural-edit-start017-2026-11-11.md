# START-017 — Nested structural editor transactions for AOT v3 (2026-11-11)

## Goal

START-015 introduced per-Blocks source revision certificates for native AOT
v3, and START-016 moved common field edits to Blocks::setBlockField().
However, structural graph edits (next, parent, input sockets, topLevel,
scripts registration) still took raw mutable Block* and relied on each
editor call site to remember resetCache(). Recursive deleteBlock() and
bulk pastes also generated multiple intermediate revision/cache resets.

START-017 makes these edits explicitly versioned and coalesces nested
source changes without publishing a partially edited graph.

## Implementation

- src/model/blocks.hpp, src/model/blocks.cpp:
  Blocks::StructuralEdit is an RAII scope, usable with nested scopes.
  Each scope ensures a cache invalidation is scheduled. resetCache() called
  by createBlock(), deleteBlock(), or setBlockField() inside a transaction
  only marks the pending reset. Leaving the outermost scope invokes
  resetCacheNow() exactly once, updates the monotonic source revision and
  retires the previous execute-cache generation. No intermediate version is
  published during the editor operation.
- src/shell/editor.cpp and src/shell/editor_sdl.cpp:
  deleteBlockChain(), deleteSelectedBlock(), duplicateBlock(), detachDrag(),
  doDrop(), cancelDrag() now use StructuralEdit scopes. The actual graphical
  drag remains two distinct committed operations: detach on mouse gesture,
  and insert/cancel at mouse release. No transaction remains open across
  event-loop iterations or scheduler frames.
- src/blockly/ops.cpp:
  insertBefore(), insertAfter(), attachToValue(), attachToSubstack(),
  makeTopLevel() also enter the same scope, after validating the required
  target/connection. When used directly by a caller outside Editor, their
  source revision is updated; when called inside a larger editor transaction,
  the nested scope merges into the outer commit.
- src/shell/editor_drag_tests.inc and editor selftests:
  removed now-redundant manual resetCache() calls after blockly helpers.
  Added checks that standalone insertBefore, insertAfter, attachToValue
  publish an updated source revision.
- tests/platform/aot_structural_edit_transaction_smoke.cpp:
  constructs a real 64-node block chain in nested source transactions,
  validates only one revision advance, verifies cached immutable
  snapshots, checks old native AOT lazy slot rejection, deletes the entire
  chain under another single outer commit, and tests direct opcode editing
  inside StructuralEdit.
- tests/platform/aot_lazy_parallel_smoke.cpp:
  the already tested real eight-script native AOT edit-after-greenFlag
  and suspended native Fiber tests now change operator_add into
  operator_subtract through a StructuralEdit scope with NO manual
  resetCache(). This demonstrates that native v3 detects the committed
  structural edit and fails closed.
- tests/CMakeLists.txt:
  new xyo-aot-structural-edit-smoke target and regression CTest.

## Tested behavior

Native CLI, structure smoke, lazy cache, real v3 parallel worker and native
editor were rebuilt from source. Targeted CTests:

- xyo-start016-editor-source-revision
- xyo-aot-lazy-cache-smoke
- xyo-aot-structural-edit-smoke
- xyo-aot-lazy-parallel-smoke
- xyo-aot-lazy-parallel-serial-smoke
- xyo-aot-lazy-parallel-trim-smoke

**6/6 PASS** in build/start017-final-ctest.log. A user-visible editor self
test runs headlessly via SDL's dummy driver and exercises drag, deletion,
undo/redo and now revision-aware standalone Blockly helpers. The native
AOT worker regression verifies both the current interpreter fallback and
the non-resumption of a source-stale suspended native Fiber.

- Native AOT v2, frozen full ScratchBench parity: **2656/2656 PASS**.
- Native AOT v3, frozen full ScratchBench parity with worker dispatch and
  lazy materialization diagnostics: **2656/2656 PASS**.
- Native no-JIT CLI build (XYO_JIT=OFF, XYO_SDL2=OFF): **PASS**.
  Editor UI selftest is unavailable in this build *by design* because
  SDL2 was disabled; the ordinary SDL editor CTest above passed.
- Optional yyjson direct-metadata build: combined editor/AOT suite
  **7/7 PASS**, including fastmeta+parallel smoke.

Logs: build/start017-v2-parity.log,
build/start017-v3-parity.log, build/start017-nojit-build.log,
build/start017-fastmeta-ctest.log.

## Safety and scope limitations

- All structural edits must be performed at a scheduler/editor safe point
  **after** worker batches have joined. This API is not a thread-safe graph
  transaction or an atomic concurrent editor/worker lock.
- No runtime script execution or BlockCached cache resolution is permitted
  *inside* an open StructuralEdit. The old revision remains visible until
  the commit, so callers must not allow concurrent native execution.
- Blocks::getBlock() and allBlocks() remain mutable; edits made through
  those raw references **outside** a StructuralEdit scope can still bypass
  source revision tracking. A future enforcement of read-only access and
  vetted mutation APIs is needed before asserting complete editor safety.
- An already executing native Fiber whose source is edited is retired,
  rather than replayed from its beginning. General state-preserving
  resumption across edits is not provided.
- AOT v3 is still opt-in and AOT v2 remains the default. No patches were
  committed or pushed from this shared uncommitted worktree.

## Reproduction

From /home/kicky/dev/cpp/xyo-cpp:

```sh
cmake -S . -B build
cmake --build build -j4 --target xyo-cli \
  xyo-aot-lazy-cache-smoke xyo-aot-lazy-parallel-smoke \
  xyo-aot-structural-edit-smoke
ctest --test-dir build -R \
  '^xyo-start016-editor-source-revision$|^xyo-aot-lazy-(parallel|cache)|^xyo-aot-structural-edit-smoke$' \
  --output-on-failure

XYO_PAR=1 XYO_AOT_LAZY_EXPECT=1 XYO_AOT_LAZY_DIAGNOSTIC=1 \
  python3 tests/differential/gui_scratch_parity_regression.py \
    --native build/xyo --project build/start013-frozen/parity.sb3 \
    --aot-object build/start013-frozen/v3.o --minimum 2656 --modes aot
```
