# START-018 — Read-only block collection access for AOT v3 source safety (2026-11-11)

## Motivation

START-015 rejects stale AOT v3 source revisions. START-016 introduced
versioned single-field writes. START-017 introduced nested RAII
StructuralEdit transactions and made Blockly graph connection helpers
versioned. An important remaining bypass was
`Blocks::allBlocks()`, which returned a mutable map even to read-only
consumers such as compilers, renderers, analyzers and the SB3 serializer.

START-018 narrows this collection API at the C++ type level while
preserving the existing editor/loader behavior.

## Changes

- `src/model/blocks.hpp`:
  `Blocks::allBlocks() const` now returns
  `const std::unordered_map<std::string, Block>&`. For both const and
  mutable Blocks objects, ordinary collection iteration cannot mutate a
  Block through the map. A mutating iterator is obtained only through
  `Blocks::StructuralEdit::allBlocks()`, which is bound to the owning
  Blocks transaction and publishes a revision on scope exit.
- `src/load/sb3.cpp`: Scratch 3 `directCompatibility()` uses a
  StructuralEdit for its procedure-shadow and control-stop-mutation
  compatibility rewrites. All writes are committed once even if they
  update several blocks.
- `src/load/sb2.cpp`: Scratch 2 stage/sprite click opcode conversion
  uses one StructuralEdit, keeping it versioned if incremental import
  is introduced later.
- `src/load/sb3_save.cpp`: serializer
  `blockToJson(const Block&)` explicitly accepts an immutable source;
  the serializer was never supposed to mutate the project.
- `tests/platform/aot_structural_edit_transaction_smoke.cpp`:
  `static_assert` verifies the exact readonly return type from Blocks
  and mutable return type from a StructuralEdit. The existing 64-block
  nested transaction test additionally mutates an opcode through a
  transaction-scoped collection iterator and checks a single revision
  advance and old AOT cache rejection.

## Validation

- Native LLVM JIT build: **PASS**, CLI and all relevant AOT smoke targets.
- Focused editor and AOT CTests: **6/6 PASS**, including source edit,
  64-block structural changes and actual worker execution.
- Full native frozen ScratchBench v2 AOT: **2656/2656 PASS**.
- Full native frozen ScratchBench v3 AOT: **2656/2656 PASS** with lazy
  materialization diagnostics enabled and parallel scheduler active.
- Actual editor CTests: **3/3 PASS**, including `native-editor-operations`.
- JIT-disabled CLI: build/link **PASS** (`XYO_JIT=OFF`, SDL2 disabled).
- Optional yyjson fast-metadata native build and CTests: see
  `build/start018-fastmeta-build.log`,
  `build/start018-fastmeta-ctest.log`.

All tests reference the existing frozen project/object pairing. No
recompilation of the project AOT object was required by this API change.

## Important limitations

- `Blocks::getBlock()` still has a public mutable-pointer overload.
  The complete elimination of arbitrary direct writes requires a much
  larger migration through the editor and compiler internals and is
  **not** claimed by this checkpoint.
- A mutable reference obtained inside a StructuralEdit can technically
  escape its lifetime in C++; callers must not store or reuse it after
  committing the transaction.
- Source mutations continue to require scheduler safe points and a
  joined worker batch. StructuralEdit does not implement reader/writer
  synchronization for concurrent changes to Blocks.
- Neither AOT v3's source-edit semantics nor the state of unrelated
  target/variable edits is proven complete. v3 remains opt-in, default
  AOT remains v2.

## Reproduction

From `/home/kicky/dev/cpp/xyo-cpp`:

```sh
cmake --build build -j4 --target xyo-cli \
  xyo-aot-structural-edit-smoke xyo-aot-lazy-cache-smoke \
  xyo-aot-lazy-parallel-smoke
ctest --test-dir build \
  -R '^xyo-start016-editor-source-revision$|^xyo-aot-lazy-(parallel|cache)|^xyo-aot-structural-edit-smoke$' \
  --output-on-failure
XYO_PAR=1 XYO_AOT_LAZY_EXPECT=1 XYO_AOT_LAZY_DIAGNOSTIC=1 \
  python3 tests/differential/gui_scratch_parity_regression.py \
    --native build/xyo --project build/start013-frozen/parity.sb3 \
    --aot-object build/start013-frozen/v3.o \
    --minimum 2656 --modes aot
```

All edits stay uncommitted in the shared repository. Existing concurrent
workstreams were not discarded or pushed.
