# START-016 — Versioned editor semantic field commits (2026-11-11)

## Goal

START-015 prevents stale native AOT v3 code from running when a source
`Blocks` revision changes, including lazy `BlockCached` slots and
suspended native Fibers. However, the model intentionally exposes mutable
`Block*` via `Blocks::getBlock()`, so a field written directly without
`resetCache()` can bypass the revision guard.

This checkpoint migrates the user-visible field-edit paths to a single
commit API and tests that an actual headless editor edit updates the source
revision.

## Changes

- `src/model/blocks.hpp`, `src/model/blocks.cpp`:
  `bool Blocks::setBlockField(blockId, fieldName, BlockField)` modifies the
  field and calls `resetCache()` together. An unknown block returns false
  and leaves the revision unchanged.
- `src/shell/editor.cpp`: boolean fields, dropdowns/variable selectors,
  numeric shadow inputs and text shadow inputs now call
  `setBlockField()` instead of assigning to `Block::fields` and separately
  resetting the cache.
- `src/shell/editor_sdl.cpp`: the same modifications were applied to the
  older SDL editor implementation, although the current CMake CLI uses
  `editor.cpp`. Do not confuse it with the active UI test target.
- Numeric input conversion retains its previous behavior without allocating
  an unnecessary GC-managed string for a valid number.
- The built-in editor selftest now asserts that numerical and text
  shadow updates and dropdown changes cause a revision change.
- `tests/platform/aot_lazy_cache_smoke.cpp`: a valid numerical shadow
  field edit changes `Blocks::revision()`, creates a new cached execute
  generation, preserves the immutable old cached value and rejects the
  old AOT lazy slot. A missing block ID does not change the revision.
- `tests/CMakeLists.txt`: new
  `xyo-start016-editor-source-revision` CTest invokes the actual
  `xyo edit tests/fixtures/basic.sb3` with
  `XYO_EDIT_TEST=1 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
  XYO_PEN_CPU=1`. It requires the editor's selftest PASS marker.

## Verification

- Native AOT cache/parallel/malloc-trim CTests: **4/4 PASS**.
- Actual headless editor selftest: **PASS**.
- Combined START-016 editor + AOT test group: **5/5 PASS**.
- Native AOT v3, real ScratchBench full parity: **2656/2656 PASS**
  with lazy-bind diagnostics enabled.
- Native AOT v2, same project: **2656/2656 PASS**.
- Native `XYO_JIT=OFF` CLI: builds after the model API change.

The AOT full-parity checks verify no runtime regression for unedited
projects. Their results alone do not prove that every possible editor
mutation path is versioned.

## Unresolved limitations

1. `Blocks::getBlock()` and `Blocks::allBlocks()` still expose mutable
   references. Any external caller can modify `Block::opcode`,
   `Block::inputs`, `Block::mutation`, etc. without notifying the model.
   The AOT revision guard cannot detect such a *silent* edit.
2. Editor structural actions (drag/drop, script graph rewrites, undo/redo)
   continue using their established `resetCache()`/ `restoreState()`
   paths. They were audited but not all converted to a transaction API.
   The next recommended change is a source-edit transaction that owns
   structural graph changes and commits a revision exactly once.
3. Source changes must occur at scheduler safe points. This patch does
   not make the source graph or old `BlockCached*` references safe under
   concurrent writes from GUI and native worker threads.
4. v3 remains explicitly opt-in; the default AOT format is still v2.

## Reproduction

From `/home/kicky/dev/cpp/xyo-cpp`:

```sh
cmake -S . -B build
cmake --build build -j4 --target xyo-cli \
  xyo-aot-lazy-cache-smoke xyo-aot-lazy-parallel-smoke
ctest --test-dir build -R \
  '^xyo-start016-editor-source-revision$|^xyo-aot-lazy-(parallel|cache)' \
  --output-on-failure
```

Full parity logs:
`build/start016-v3-parity.log` and `build/start016-v2-parity.log`.
Editor smoke and combined CTest logs:
`build/start016-editor-selftest.err` and
`build/start016-final-ctest.log`.

All changes remain uncommitted in the shared xyo-cpp worktree. No other
workstream changes were reset, overwritten or pushed.
