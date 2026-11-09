# LLVM 非依存の JIT バックエンド境界

## 構成

- `src/jit/jit.hpp`, `jit.cpp`: LLVM 型を含まない `JitEngine`、HotSpot 判定、コンパイルキャッシュ、AOT のスロット束縛。
- `src/jit/backend.hpp`: `JitBackend` 抽象インターフェース。LLVM ヘッダを公開しない。
- `src/jit/backend_llvm.cpp`: LLVM ORC、IR 生成・最適化・遅延手続きコンパイル、ネイティブオブジェクト登録、文字列リテラルの GC ルート。
- `src/jit/codegen.cpp`: 現行の LLVM IR lowering。LLVM バックエンド固有。
- `src/jit/backend_none.cpp`: LLVM 非依存ビルド用の空バックエンド。Binaryen 実装そのものではない。
- `src/aot/aot_stub.cpp`: LLVM のない構成では AOT を明示的に拒否する。

## ビルド

```sh
cmake -S . -B build-llvm -DXYO_JIT=ON -DXYO_JIT_BACKEND=llvm
cmake --build build-llvm -j8

# LLVM の検出・リンクを一切行わず JIT ランタイムをビルドする
cmake -S . -B build-no-llvm -DXYO_JIT=ON -DXYO_JIT_BACKEND=none -DXYO_SDL2=OFF -DXYO_BUILD_TESTS=OFF
cmake --build build-no-llvm -j8
```

既定のバックエンドは従来どおり `llvm`。 `-DXYO_JIT=OFF` も従来どおりのインタプリタ専用ビルド。

## バックエンド追加手順

1. `JitBackend` を継承し、コンパイル成功時に `CompiledScript` を返す。
2. `ScriptFn = void (*)(JitCtx*)`、NaN-box Value、yield/Fiber、GC ルート、スクリプト・手続きの寿命契約を維持する。
3. 関数ポインタが生きている間は機械語の所有者を破棄しない。`JitEngine::invalidate` は実行中スレッドがいないときに限る。
4. 現行のネイティブ AOT オブジェクトを扱わないバックエンドは `addObjectFile` を拒否してよい。
5. Binaryen は LLVM IR を入力として受け取れない。Scratch lowering を再利用するには、LLVM に依存しない中間表現を別途導入する必要がある。
6. ブラウザの WebAssembly モジュールはネイティブ `ScriptFn` と同一ではないため、WASM バックエンドでは呼び出しブリッジも設計する。

現在は `XYO_JIT_BACKEND=wasm` による Emscripten HotSpot 実行と Binaryen 最適化の経路を追加済み。ただし、LLVM の最適化と全 Scratch ブロックが WASM に移植されたわけではない。

実装済みの範囲・差分は [WASM HotSpot](wasm-hotspot.md) と [バックエンド互換性マトリクス](jit-backend-compatibility.md) を参照。


## Experimental shared numeric SSA (2026-11-06)

The first backend-neutral graph is `src/ir/numeric.hpp`. It uses typed
`ValueId` definitions and `Parameter/Add/Sub/Mul/Div` instructions; SSA
operands always refer to earlier nodes. `ir::lower` accepts an adapter with
`parameter(index)` and `binary(op, lhs, rhs)` methods. The graph has no
dependency on LLVM or Binaryen. Small graphs lower with stack storage, and
common two-parameter binary graphs are cached.

Both native LLVM (`src/jit/codegen.cpp::emitArithNumeric`) and WASM
(`src/wasm/wasm_compile.cpp::Gen::emitExpr`) now call this shared graph for
basic F64 arithmetic. Scratch input coercion stays in their original code
paths, preserving the existing NaN and type handling. The separate WASM
encoder still produces a module that the optional Binaryen optimizer consumes;
Binaryen itself is not used as a native machine-code JIT.

Tests: `xyo-numeric-ir-smoke`, `xyo-wasm-binaryen-backend`, and native
numeric JIT differential tests. This is **not** a full unification:
modulo, comparisons, branching, variable slots, phi nodes, GC, yields,
procedures and deoptimization are still owned by existing backends.
Implementing those requires a whole-script SSA control-flow graph and a
runtime ABI shared by both emitters.


## Shared numeric comparisons and CFG foundation (2026-11-06)

`numeric.hpp` now supports typed `LessThan` and `GreaterThan` returning
`I1`. The LLVM numeric comparison fast path and WASM numeric comparison
path both lower these instructions; Scratch string/NaN comparisons remain
guarded by their pre-existing backend-specific fallback logic. The WASM
Binaryen smoke test includes numeric comparison execution.

`src/ir/control_flow.hpp` additionally defines basic blocks, conditional
and unconditional terminators, return terminators and Phi edge lists.
The structural validator checks terminator targets and exact incoming
predecessor sets for each Phi. This is **schema and validation only**:
neither LLVM nor Binaryen currently lowers general CFG/Phi nodes from this
representation. Dominance, value typing, side effects, yield points and
deoptimization continuation must be verified before whole-script migration.
The small control-flow tests live in `xyo-numeric-ir-smoke`.


## Structured diamond lowering (2026-11-06, follow-up)

`src/ir/diamond.hpp` now supplies a **live** shared conditional-flow
lowering contract. `Diamond` owns four logical CFG blocks (entry/true/false/
merge) and optionally a typed Phi. `lowerDiamondValue` calls the backend
branch/otherwise/finish/phi operations; `lowerDiamondVoid` emits both
statement paths without a value-producing Phi.

The LLVM native numeric comparison fast-vs-Scratch-fallback path now passes
through `lowerDiamondValue`. The LLVM adapter emits a real conditional
branch and native Phi with the *actual* incoming basic-block ends, so future
value-generation paths that split blocks do not leave stale predecessors.

The WASM `SK::IfElse` emitter uses `lowerDiamondVoid`, which
maps the same logical CFG to structured `if/else/end`. Single-arm `SK::If` retains its original compact direct emission. No dispatcher or
synthetic interpreter is inserted in either backend. Common Diamond instances
are cached at their code-generation sites.

This is limited to structured, reducible diamond regions; arbitrary CFGs,
loops with backedges, Phi cycles and effectful yield/deopt transitions are
not yet lowered by the shared API. Binaryen remains a post-emission optimizer
rather than a native-code JIT. Guard behavior, Scratch comparison semantics
and transactional WASM bailout continue to use existing runtime code.

Regression suite: `xyo-numeric-ir-smoke` (logical Phi selection and branch
order), `xyo-jit-plain-list-compare` (native LLVM fast/fallback Phi),
`xyo-wasm-binaryen-backend` (actual Node/WebAssembly if/else plus numeric
comparisons). These tests cover the migrated paths, not full backend parity.


## Typed loop backedges shared between LLVM and WebAssembly

`src/ir/loop.hpp` defines a one-latch `BackedgeLoop` containing a real
preheader -> header / header -> header / header -> exit CFG, and one or more
header Phi inputs (preheader initialization + loop backedge). The
`lowerBackedgeLoop` contract is implemented in native LLVM's guarded
`emitRepeatAffineChunk` (both iteration and accumulator remain LLVM SSA Phi
values) and the standalone WASM `SK::Repeat` lowering (the induction Phi is
represented by a WASM local and a structured `loop/br_if`). To preserve
zero-count semantics, WASM tests for a positive iteration bound *before*
entering the do-while-like backedge region. The Scratch rounding, guard
and interpreter bailout behavior remains in the surrounding existing code.

The shared loop currently supports these two selected single-latch patterns,
not arbitrary irreducible graphs, nested-OSR continuation, value dominance
verification, scheduling or deoptimization. Structural loop CFG validation
checks the existence of proper Phi predecessor edges. The LLVM optimizer
continues to see and optimize actual LLVM PHIs. Runtime parity is exercised by
`xyo-jit-repeat-ssa`, `xyo-jit-repeat-i32`, and
`xyo-wasm-binaryen-backend` (including zero-count and nested repeat
scripts); the template/control-flow smoke test is `xyo-numeric-ir-smoke`.


## Backend-neutral guard dispatch

`src/ir/guard.hpp` introduces `GuardReason` for `NumericValue`,
`IntegralRange`, `LoopCount`, and `OrderedComparison`, plus an explicit
failure policy: `Fallback` means native LLVM branches into the original
Scratch path; `Bail` means a standalone WASM function returns status 1.

`lowerGuard` delegates the predicate to backend-specific `require`
implementations. It is now used by LLVM's guarded repeat-affine SSA fast path
and by WASM variable reads, change-variable operations, numeric comparisons
and repeat-count range validation. The surrounding code remains responsible
for establishing Scratch-compatible numeric coercion and the safety of the
guard location. In particular, this API **does not yet carry a deoptimization
snapshot** or guarantee transactional recovery after arbitrary side effects.

The WASM differential test injects canonical numeric NaN and a tagged
boolean directly into imported memory, checking that the first is allowed
and the second bails without writing its destination. It also checks that
out-of-range repeat counts bail before executing the body.


## P0: Deoptimization snapshot and initial IR verifier

`src/ir/deopt.hpp` defines `DeoptSnapshot`, which captures raw 64-bit
Scratch value slots and a typed `ResumePoint` containing script entry /
loop body / loop suffix, loop index, remaining count, and a logical block
index. Captures of up to 16 slots avoid a heap allocation; larger captures
are bounded to 4096 slots.

In the Emscripten-specific `src/wasm/hot_jit.cpp::invokeSegment` path,
the snapshot is taken **after variable resolution and before speculative
ForEach overrides**. A nonzero WASM status, an invalid output tag or a bad
captured loop bound now restores the staged slots and loop cursor, leaving
runtime `Variable` values unmodified. Successful segments publish all
variable changes only after every output has passed its existing checks.
The snapshot is then discarded. Roots remain held by the original runtime
variables: snapshots of tagged pointers are not GC roots in isolation.

This is a transaction boundary for **isolated WASM segments**, not a
general LLVM deoptimization state map. It does not yet describe virtual
objects, uncommitted effect logs, complete interpreter frames, dynamic
procedure stacks, scheduler state, or values live across arbitrary optimized
basic blocks. In particular, no assumption of safe mid-segment replay is
introduced: failure always restarts the current uncommitted segment.

`src/ir/verifier.hpp` introduces Release-safe numeric SSA validation
(opcode, operand definition order, scalar operand/result types) and CFG
validation (terminators, Phi predecessor sets, entry reachability).
Full cross-block SSA dominance checking still requires per-block value
definitions and a def-use map, which the current structural CFG lacks.

Tests: `xyo-numeric-ir-smoke` checks exact tagged-word restoration,
resume coordinates, rejection of malformed and unreachable graphs and both
inline/heap snapshot storage. Existing WASM and JIT regression tests pass.
A native compiler also syntax-checks the Emscripten-gated HotSpot source with
`-D__EMSCRIPTEN__`; an actual Emscripten build/browser runtime test is
still required.


## P0 #2: Guard + Deopt transaction contract

`src/ir/guard.hpp::checkSpeculativeGuard` now ties a failed predicate to
`DeoptSnapshot` restoration, and reports **both** whether the guard passed
and whether rollback succeeded. The `ResumePoint` records the current
script/loop continuation; the snapshot retains the exact NaN-boxed slot bits.
The Emscripten-specific `invokeSegment` calls this routine for nonzero
WASM status, invalid output slots, invalid captured counts, and invalid loop
slot overrides. No `Target::Variable` is modified before all output
validation succeeds.

This is a **segment-level transaction only**. Runtime helpers that perform
external side effects and arbitrary native LLVM frames require an effect log
and stack-map/deopt metadata before the same recovery guarantee is valid.
The native LLVM `GuardFailure::Fallback` branch remains available and is
not silently replaced by WASM-style interpreter unwinding.

## P0 #3: Typed inter-block SSA verification

`src/ir/ssa.hpp` provides an explicit table of typed SSA definitions,
Phi bindings, per-block instruction positions and use sites.
`src/ir/verifier.hpp::verifySSA` checks:

- structural CFG validity and entry reachability;
- iterative dominator sets, including backedges;
- definitions before ordinary intra-block uses;
- types at ordinary uses and on incoming Phi edges;
- that a Phi's incoming value dominates its **predecessor edge**, not the
  merge block (including a loop-carried value from its own header);
- duplicate Phi ownership and duplicate definition positions.

The `xyo-numeric-ir-smoke` regression includes passing diamond/loop cases
and failing dominance, type, missing-definition, duplicate-definition and
forward-reference cases.

The existing `Codegen` and WASM `Gen` still create much of their real
IR directly; the new cross-block metadata is verified in unit tests but
**not yet constructed for all compiled Scratch scripts**. Automated
whole-script SSA verification before emission is a subsequent migration step.


## P0 #4/#5: General CFG and multi-Phi executable lowering

The portable IR now contains a **separately executable numeric CFG subset**:

- `src/ir/executable.hpp`: typed per-block instructions for i64 constants,
  signed integer add/subtract, signed comparison, CFG terminators and i64
  return; uses `SsaGraph` for Phi placement and def-use validation.
- `src/ir/cfg_lower_llvm.hpp`: creates actual LLVM basic blocks, integer
  instructions and typed Phi nodes; fills incoming values after every block
  has been emitted, allowing forward references and loop backedges.
- `src/ir/cfg_lower_wasm.hpp`: lowers arbitrary graph edges through an
  explicit program-counter dispatch loop. Each SSA value occupies a WASM
  local. On each edge, **all incoming Phi values are staged first**, then
  written to destination locals, ensuring cyclic/parallel Phi assignments
  do not corrupt the previous iteration's values. The Phi-local mapping is
  precomputed during compilation. The emitter can be given an initial local
  index when inserted into a function with arguments.

The test `xyo-ir-cfg-backends` independently compiles the *same five*
graphs with LLVM and the custom WASM emitter, checks LLVM IR verification and
WASM binary validation, and runs the outputs with `lli` and Node.js:
a two-Phi summation loop (55), two conditional diamonds (42 and 99),
a three-Phi backedge swap (20), and a branch with identical destinations (7).
Malformed value IDs and type inconsistencies are rejected by
`verifyExecutable`. When the tools are present, tests also run the LLVM
O2 optimization pipeline and Binaryen wasm-opt O2 on the generated modules,
then rerun both optimized outputs.

**Scope limitation:** this is a correctness-oriented new path for pure typed
integer graphs. Existing Scratch LLVM/WASM code generation is **not yet fully
migrated** to it. The WASM implementation's per-block PC dispatcher is
intentionally a general CFG fallback and may be slower than structured
`if/loop/block` emission. Type guards, runtime slot memory effects,
transactional deoptimization and scheduling remain outside this subset.
The next task is a structured CFG pass (reducible regions first), then
migration of variable access and effect metadata into the same IR.


## P0 #6: Portable typed variable slots (i64 subset)

The executable CFG subset now supports mutable zero-initialized i64 slots,
independent of its SSA values. A program declares `variableSlots` and emits
`LoadVar(slot)`, `StoreVar(slot, value)`, or
`ChangeVar(slot, delta)`. All accesses are checked at verification time.
`StoreVar` explicitly has no SSA result (`kNoValue`), while
`ChangeVar` publishes the updated i64 as an SSA value. Store operands
must dominate the write; load/change results have normal SSA definitions.

The LLVM adapter creates one entry-block `alloca i64` per slot with a zero
store. Subsequent accesses use LLVM loads/stores; `ChangeVar` is
load/add/store without signed-overflow assumptions. LLVM O2 can promote these
allocations with standard scalar replacement / mem2reg optimizations.

The WASM adapter emits corresponding i64 locals initialized to zero at
function entry; loads and stores become `local.get/set`, and
`ChangeVar` uses `i64.add/local.tee`. Both adapters use the same
CFG+SSA verifier, including invalid-slot rejection and store-before-definition
checking. The differential test covers branches, loop-carried changes,
two distinct slots, zero initialization and malformed accesses. It executes
both native/portable outputs before and after LLVM O2 and Binaryen O2.

**Important scope:** These are **private i64 slots**, not Scratch's
general-purpose NaN-boxed `Value` storage. There is no implicit
conversion of booleans/strings, no GC root publication, no cross-thread
variable sharing, and no transactional Deopt of partially committed stores.
Scratch runtime variable access requires an explicit tagged-value ABI,
guarded specialization, and a commit/recovery contract before using this
new path for real `.sb3` script variables.


## Shared tagged Scratch slot access (2026-11-06)

`src/ir/slot_access.hpp` now defines a backend-neutral raw
`lowerSlotLoad`, `lowerSlotStore`, and `lowerNumericSlotGuard`
contract. The ABI retains the exact eight-byte Scratch `Value` bit pattern;
the shared `isNumericBits` predicate matches `Value::isNumber` including
canonical numeric NaN, but intentionally rejects booleans, string tags and
color tags.

The LLVM `Codegen::emitVarGet` ordinary load, `emitVarSet` ordinary
store, and `emitIsNum` call adapters for this shared contract without
replacing their existing promotion/guard/GC metadata. The WASM compiler's
`VarGet`, `ChangeVar`, numeric-value guard and `SetVar` store also
use the raw slot contract. Its current `storeF64` still canonicalizes NaN
before storing and the HotSpot host still stages/commits slots transactionally.
The adapter is header-only; it does not add runtime virtual dispatch or
heap-allocate a backend IR object.

The platform test compares the shared predicate to `Value::isNumber` on
numeric NaN, signed zero, finite doubles, booleans, empty/heap strings,
undefined and colors, and checks preservation of raw 64-bit payloads.
The native LLVM and built-in/Binaryen WASM regression tests pass.

This is the *common raw access/guarding layer* for existing Scratch variables,
not the full replacement of their runtime semantics. In particular, generic
string-to-number casts, changing cloud variables, allocation/GC barriers,
and Deopt state maps remain in their existing runtime paths. The i64
`Executable` private slots have not been silently reinterpreted as dynamic
Scratch variables.


## Shared immediate-value ABI (2026-11-06)

`src/ir/value_abi.hpp` introduces backend-independent tagged-value
classification and fast numeric coercion for IEEE-754 numbers, canonical
numeric NaN, booleans, the empty-string singleton, color values and
undefined. Heap strings are explicitly marked as requiring the UTF-16
Scratch numeric parser; tagged pointers are never reinterpreted as doubles.

`cast::toNumber` uses the immediate path and retains the existing cached
string parser for heap strings. `cast::toNumberOrNaN` is unchanged, so
NaN-preserving conversions keep their original semantics.

Standalone WASM `ChangeVar` uses `lowerImmediateNumber` to convert
booleans, empty strings, colors and undefined without a bailout. Heap
strings still bail before writing, leaving the exact Scratch parsing path
to the interpreter. Existing numeric-only `VarGet` consumers are not
changed, because some comparisons need operator-specific coercion.

Tests cover signed zero and raw tagged values; the WASM differential runner
exercises true/false/empty/color coercion, heap-string bailout and both
Builtin/Binaryen backends. Full GC, strings, lists and OSR are outside this
specific ABI step.


## WASM HotSpot heap-string ToNumber host ABI

`env.h` operation `5` now converts one *host-staged* heap-string
Scratch `Value` (8-byte NaN-boxed bits) into a raw f64 in place. The
implementation is `convertHeapStringNumberSlot` in
`src/wasm/string_number_bridge.hpp`, which delegates to the established
UTF-16 numeric parser and its existing string-number cache.

`wasm::Gen::emitImmediateToNumber` dispatches heap-string values to this
helper only when `Program::useHostMath` is enabled. The standalone
module retains its safe bailout, and the host return status is guarded
before continuing. HotSpot now requests the helper import when a
`ChangeVar` statement might need coercion. The segment invoker can
stage heap-string scalars while their authoritative `Target::Variable`
remains a GC root. It still requires every committed slot to be numeric.
Consequently, an untouched string slot causes rollback and interpreter
fallback, not a string write from a numeric-specialized WASM segment.

Tests:
- `xyo-wasm-string-bridge-smoke` checks real Scratch parsing and the
  rejection of non-heap-string raw tags.
- `xyo-wasm-host-string-abi` generates a standalone and a host-enabled
  WASM module from the same ChangeVar statement, then uses Node.js
  WebAssembly to verify `env.h(5, slotAddress, 0)`, correct post-conversion
  results, standalone bailout, and failure with no slot mutation.
- Existing LLVM JIT, numeric IR and Builtin/Binaryen tests retain parity.

**Limitations:** A native Node.js mock tests the generated imported-call
ABI, but it does not invoke the real Emscripten runtime helper in a browser;
the actual Emscripten build / browser GC integration remains untested here.
This does not yet migrate lists, general string comparisons, or other
Scratch runtime effects into the shared IR.


## Shared numeric-list access and rooted ownership

`src/ir/list_access.hpp` defines the portable numeric-list access
contract. The LLVM backend uses the shared `SList` layout offsets and
`lowerNumericListGuard` for its existing `numericOnly` speculation.
The generic runtime `rt::listGet` also uses `readNumericListItem` for
a numeric index when the list-wide flag still guarantees numeric contents.

The helper requires a live rooted `SList`, checks numeric-only feedback,
finite/in-range one-based indexing, and the loaded value's actual tag. It
returns the exact 64-bit Scratch `Value` bits, without dereferencing heap
strings, doing a generic cast, allocating, or holding an `items` pointer
across an allocation/scheduler boundary. Out-of-range, mixed-type, and
non-number indices retain the existing `cast::listIndex` semantics,
including special names such as `last` and `random`.

`xyo-numeric-list-abi-smoke` validates numeric lookups (including
fractional indices, negative zero and canonical NaN), bounds, permanent
`numericOnly` invalidation after a string write, and a forced GC
collection while an explicit owner root provider keeps the list and its
string element live.

**WASM limitation:** A list is not a scalar `Value` slot. Current HotSpot
stages scalar variables only; sending raw `SList*` or `items` pointers
through its WASM memory would bypass the runtime's GC ownership contract.
A future WASM list helper must resolve a list through a rooted owner/variable
identity per invocation, not assume it can retain a C++ list pointer across
WASM calls. The present change does not claim compiled WASM list statements
are supported.


## WASM HotSpot numeric-list access with host-owned binding

`src/wasm/list_host_bridge.hpp` defines an invocation-scoped binding table
of borrowed `Variable*` entries owned and rooted by the live `Target`.
`ScopedListBindings` restores any previous binding context on exit,
including bailout paths. Generated WASM passes only a **list index in this
binding table**, never a native `SList*`, element-array address, or GC root.

The `env.h` helper with operation ID **6** receives a pointer to a
host-owned eight-byte scratch cell containing the f64 one-based list index
and a list-binding index. It validates the binding, resolves the current
`Variable::list`, checks `numericOnly` and the actual numeric value tag,
and writes back the raw 64-bit numeric `Value` on success. Failure leaves
the scratch cell untouched and returns a nonzero status. It neither
allocates nor caches list item storage across calls.

`wasm::Lower` now recognizes `data_itemoflist` for supported numeric
index expressions, placing IDs in `Program::listIds` separately from
`slotIds`. The compiler adds a guarded `EK::ListGet` host import.
The HotSpot backend validates list IDs at compile time, enables the
import, and resolves them freshly for every synchronous segment. Failure
to read a numeric element bails out to the existing interpreter continuation,
which retains all Scratch index-string and mixed-type behavior.
Standalone WASM safely bails, without importing the list helper.

Tests:
- `xyo-numeric-list-abi-smoke` checks rooted host binding, successful
  numeric access, out-of-range/mixed-list failure and nonmutating rejection.
- `xyo-wasm-host-list-abi` runs generated WASM under Node.js with a
  strict `env.h(6, scratchAddress, listBindingIndex)` mock, checking valid
  results, nonmutating failure and standalone bailout.
- `test_wasm_compile` checks actual Scratch `data_itemoflist` block
  lowering into separate list/scalar identity namespaces.
- Both Emscripten-only source files passed native syntax checks under
  `-D__EMSCRIPTEN__`; an actual browser/Emscripten runtime integration
  test is not yet available.

This remains a **numeric list read specialization only**. List writes,
general string-valued list reporters, index magic strings, GC ownership
changes, and scheduler effects still use the interpreter or original
runtime paths. No native pointer is embedded in the generated WASM.


## Transactional numeric-list writes in WASM HotSpot

`src/wasm/list_host_bridge.hpp::NumericListTransaction` now keeps
copy-on-first-mutation numeric list buffers in an invocation-scoped
transaction. `env.h` operations `7/8/9` stage append, replace, and
delete respectively. Operation `6` (numeric list read) consults that
same staged buffer, preserving read-after-write semantics within a
segment. Neither the live `SList` length nor elements are changed
until `HotSpot::invokeSegment` has validated its WASM return code,
scalar outputs and captured loop counts. Any bailout discards staged
lists while the interpreter resumes at the segment checkpoint.

The host owns all `Variable*` and their GC-visible `SList` objects.
The WASM ABI supplies only binding IDs and f64/numeric Value scratch
cells. No list pointers are exported. Staging is intentionally bounded
to 65,536 numeric elements per list; larger or mixed-type lists bail
rather than attempt unsafe speculative writes. All appended/replaced
values must be numeric, and nonnumeric/special string indices remain in
the interpreter. Successful commit pre-reserves destination buffers,
then publishes staged values/lengths and marks monitors dirty.

The portable WASM AST supports `SK::ListAppend`, `SK::ListReplace`
and `SK::ListDelete`. `Lower::stmt` recognizes the corresponding
Scratch list opcodes and records stable IDs in `Program::listIds`
rather than scalar slots. `xyo-numeric-list-abi-smoke` verifies
read-after-write, append/replace/delete, rollback by scope exit,
commit, and numeric-only invariants. `xyo-wasm-host-list-abi`
executes generated WebAssembly using a Node.js host-import simulator
with success and late-bailout scenarios. The actual browser runtime
still needs a full Emscripten/GC integration test.

This numeric specialization is deliberately *not* the full Scratch
list semantics (arbitrary types, special indices, cloud/monitor side
effects). Unsupported cases must fall back without partial writes.


## Sparse numeric list write journal

`NumericListTransaction` no longer copies a numeric `SList` on the
first append or replace. Instead, `Stage` holds only the appended
values and an index-to-value replacement map. Reads resolve the staged
appends/replacements first and consult the original rooted list for all
other elements. The original list remains untouched until successful
commit. A delete may shift indices, so it materializes exactly once,
merging all earlier staged edits into a vector before erasing.

For an `n`-element list with `k` replaced elements and `a`
appended elements, the common append/replace-only case uses
`O(k+a)` additional memory instead of `O(n)`. A single indexed
replacement is not required to scan/copy the entire list. Commit still
pre-reserves the resulting capacity of every touched live list before
publishing the staged changes. Deletes retain the full-copy fallback
(`O(n)` extra memory), and `kMaxItems = 65536` bounds this
specialization. GC ownership and the segment-level rollback contract
remain unchanged.

Regression coverage: 50,000-element numeric list; sparse replace
and append without materialization; noncommitting rollback; commit
publishing exactly the intended values; a deletion forcing
materialization; deterministic interleaved edit/read trace checked
against an eager reference model.


## 実Scratchスクリプトの共通Source IR

`src/ir/scratch_script.hpp` で全opcode、入力順序、シャドウ、SUBSTACK、
手続きのメタデータ、未対応拡張opcodeを保持するスナップショットを
導入した。WASM `lowerScript` は `lowerCapturedScript` を経由し、
LLVM `jitEmitScript` は同じ形式のスクリプトを構造検証してから
従来のCodegenを呼ぶ。

LLVM Codegenの `Block*` をコピーに置換する試験ではOSR/SSA回帰が
発生したため、LLVMの命令生成元は依然として既存のライブBlock。
完全な共通実行可能IRではなく構造的Source IRの段階である。
詳細は [Scratch共通Source IR](scratch-unified-source-ir.md) を参照。
