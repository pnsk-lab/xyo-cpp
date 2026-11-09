# WASM HotSpot JIT

## Execution

Emscripten can execute eligible Scratch scripts using the JavaScript WebAssembly engine through the existing HotSpot scheduler. Select CMake XYO_JIT=ON and XYO_JIT_BACKEND=wasm; native LLVM and none backends remain independent.

Pipeline: Interpreter -> HotSpot decision -> shared ScratchCompilationUnit -> (whole script or hybrid top-level regions) -> wasm emitter -> optional Binaryen -> WebAssembly.Module/Instance -> transactional execution, with opaque effect regions delegated to the original Scratch interpreter.

## Build

~~~sh
source "$HOME/.emsdk/emsdk_env.sh"
emcmake cmake -S . -B build-wasm-hotspot -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DXYO_JIT=ON -DXYO_JIT_BACKEND=wasm \
  -DXYO_SDL2=OFF -DXYO_BINARYEN_C_API=OFF \
  -DXYO_BUILD_TESTS=OFF
cmake --build build-wasm-hotspot --target xyo-cli -j4

XYO_HOT_INVOCATIONS=1 XYO_JIT_VERBOSE=1 \
  node web/xyo-node.mjs build-wasm-hotspot/xyo.js \
  run project.sb3 --jit hotspot --frames 20
~~~

The SDK linker must have a writable Emscripten cache (EM_CACHE). The WASM bridge is enabled automatically for the wasm JIT backend.

## Numeric and control-flow subset

Eligible static Scratch hats (green flag, broadcasts, key/click/backdrop events, clone start) can use numeric scalar variables, set/change, arithmetic, round, all 14 operator_mathop menu operations, numeric comparisons, and/or/not, if and if-else. Edge-activated event hats remain interpreted.

Scripts with up to 32 sequential top-level loops (control_repeat, control_repeat_until, control_while or control_forever), excluding general nested loops, can run as resumable WASM HotSpot. The compiler creates an initial prefix, a body module per loop, a condition module for while/until, and an inter-loop suffix/prefix module which can capture the following repeat count. The Thread holds the current loop index, phase, and repeat count. Each body executes no more than one iteration between scheduler checkpoints. A forever loop does not naturally reach later statements or loops.

At each non-warp loop checkpoint, the scheduler resumes the body; it never executes all iterations in one uninterruptible WASM call. Counts above one million are left to the interpreter to avoid excessive WASM host transitions. Unsupported script blocks, general non-warp nested repeats, recursive and yielding procedures, general mixed-type list mutation, drawing and I/O use the interpreter; compatible top-level regions on either side can still be JIT compiled. Common string joins, letter access, length, contains and mixed-type comparisons use the transactional host ABI. Guarded nonrecursive warp procedures with numeric arguments can be inlined; nested repeats inside these procedures use a 1024-iteration speculative fuel cap and deopt on exhaustion. Synchronous non-warp leaf procedures without loops or nested calls can also be inlined.

## Transactional execution and bailout

Every WASM segment reads the current target/stage scalar values into a per-thread, reusable staging buffer. Read-only string values remain rooted in Target variables and are not written back; exact tagged Value writes (strings, booleans and numbers) are copied back only when the segment returns successfully. Numeric-list writes use a transaction until commit. If a value becomes nonnumeric, a comparison requires Scratch-specific NaN handling, or WASM traps, the segment's writes are discarded.

When a later body/condition segment fails, the interpreter resumes at the precise loop block. For counted repeat it restores the remaining loop count on the StackFrame's ExecContext; for for-each it restores the exact induction index. For while/until/forever the interpreter re-evaluates the condition normally. Earlier successful iterations are never replayed. After a suffix failure, execution resumes directly at the suffix. The initial segment can fall back to the original entry without committing any writes. The generated code is main-thread-affine; a non-pthread Emscripten build disables the native worker pool and sampled-clock watchdog.

The current model supports whole-script promotion and safe hybrid compilation on a fresh invocation, with bounded sequential-loop continuation. It does not provide arbitrary nested suspension points or general mid-loop on-stack replacement, nor native LLVM-style OSR, arbitrary recursive procedures, full mixed-type list manipulation, or native object AOT.

When a modulo reporter is compiled in HotSpot, it calls the existing C++ Scratch runtime fmod implementation via env.h with two f64 operand slots in the shared linear memory. This is necessary because WebAssembly has no native floating-point remainder, and n - trunc(n/m)*m can produce observably different results. Standalone emitted WASM without a host helper bails out on potentially imprecise modulo inputs rather than returning inaccurate values.

For operator_mathop, WASM emits native f64.abs/floor/ceil/sqrt instructions for the four directly equivalent operations. The 10 remaining trigonometric, inverse trigonometric, logarithmic and exponentiation menu options call the same xyo_mathop numerical routine that LLVM JIT uses via the env.h helper import. The caller supplies only a numeric scratch buffer; no LLVM library is required at runtime. These operations bail out in standalone WASM modules without that import rather than returning inaccurate approximations. Unsupported menu names return zero as Scratch does.

## Binaryen

The WASM emitter is LLVM independent. Optional Binaryen optimization uses an Emscripten-compatible Binaryen C API or a synchronous host callback (Module.xyoOptimizeWasm). A Node loader is provided in web/xyo-node.mjs: installing the binaryen npm package and setting XYO_BINARYEN_JS=1 enables its optimizer. The host Linux libbinaryen.so cannot be linked to an Emscripten target.

XYO_WASM_HOT_EMITTER=auto prefers Binaryen when available and otherwise emits valid built-in WASM. Set binaryen to require it, or builtin to disable it for comparison. The real Binaryen optimizer has also been verified through the Node N-API adapter (web/binaryen-node); tests check that its native optimization callback actually runs, not just that the emitter was selected. The built-in emitter remains available for comparison.

## Regression

~~~sh
python3 tests/differential/wasm_hotspot_integration.py \
  build-wasm-hotspot/xyo.js --out build-wasm-hotspot --llvm build/xyo
python3 tests/differential/wasm_compat_fuzz.py \
  build-wasm-hotspot/xyo.js --out build-wasm-hotspot --llvm build/xyo --cases 128
node web/xyo-node.mjs build-wasm-hotspot/xyo.js wasm-selftest
~~~

The integration tests compare WASM HotSpot, Interpreter and optionally the native LLVM JIT on mathop operations (including NaN/infinity, unknown menu values and loop-body combinations) and numerical scripts, if/else, all four single-loop forms, broadcast and broadcast-and-wait event dispatch, string/NaN guard fallback, inter-thread type changes, and exact mid-loop/suffix interpreter continuation. Typed string joins and lexical comparisons are now compiled. Unsupported general procedures and waits are handled by hybrid interpreter regions, retaining exact scheduler behavior. Numeric list operations, string length and nonrecursive warp calls have separate regression tests. The deterministic seed-based test also exercises nested arithmetic, sequential loops and modulo, including NaN-to-number conversion and floating-point remainder precision. During fuzzing, a bug was found in loop-module construction: moving slotIds prematurely made the modulo host scratch area alias a Scratch variable. Segment compilation now retains its slot metadata until all loop modules have been emitted.

**This does not mean full native LLVM-JIT feature equivalence.** The current WASM backend does not accelerate all Scratch operations or offer LLVM's compile-time optimizations, recursive call handling, OSR, async compilation or AOT. See jit-backend-compatibility.md.

General-purpose Scratch and cross-browser performance are not yet covered.

## Hybrid top-level regions

If a complete script cannot be represented by the restricted numeric
scheduler, the backend captures the same immutable Scratch block graph and
effect classification used for full script lowering. It partitions up to
256 top-level statements into contiguous compiled regions and interpreter
regions; each compiled region is bounded to 16 Scratch statements and its
own guarded, transactional slot/list state. Neither the original block
graph nor a live interpreter frame is rewritten during compilation.

The Sequencer stops interpreted execution **before** the next compiled
top-level block, and only when the frame depth is one. An explicit wait,
a procedure, a nested loop or a yield remains fully owned by the native
interpreter, including its GC roots and nested StackFrames. A yield never
causes the following compiled region to execute in the same scheduler pass.
A failed WASM guard discards the whole region's speculative writes and
resumes interpreting from that region's first block, not from the original
script hat. Earlier committed regions are never replayed.

Scalar assignments from variables preserve raw tagged Value bits, including
strings, booleans and -0/NaN. Boolean reporters produce Scratch true/false
tags. Numeric reporter inputs use the existing Scratch numeric conversion
host ABI. Scratch comparisons use an in-WASM f64 fast path for normal numeric
Values and delegate mixed strings, booleans and canonical NaN to the
exact host compare algorithm. Coercing both strings to f64 is forbidden.

The hybrid scheduler is a **semantics compatibility and partial acceleration**
path. It is not LLVM's entire JIT instruction set: arbitrary
yield/OSR/recursion and dynamic rendering/sound semantics still execute
through the interpreter. It does not claim that every Scratch opcode is
compiled by Binaryen.

Reproduce the real Binaryen/LLVM tests:

    python3 tests/differential/wasm_hybrid_regions.py \
      build-wasm-hotspot-parity/xyo.js \
      --addon build-wasm-hotspot-parity/binaryen-host.node \
      --llvm build-list-epoch/xyo \
      --out scratch-bench/results/hybrid-region-fixtures

    python3 tests/differential/wasm_scratchbench_backend_parity.py \
      build-wasm-hotspot-parity/xyo.js \
      --sb3 scratch-bench/.cache/build/parity.sb3 \
      --addon build-wasm-hotspot-parity/binaryen-host.node

## Typed UTF-16 string and list bridge (November 7, 2026)

The compiled operator_join, operator_letter_of, operator_contains,
operator_length on composite expressions, comparison and if/boolean
reporters now preserve Scratch Value tags and UTF-16 code units.
Generated modules reuse the existing runtime implementations
(rt::join, rt::letterOf, rt::strContains, cast::compare and
cast::toBoolean) through env.h, rather than approximating string
operations inside WASM. The ordinary numeric comparison path stays
inside WebAssembly with no host round-trip.

The compiler owns a UTF-16 literal pool, never compiler-time pointers
into GC-managed strings. Each invocation lazily converts literals to
SStr objects and caches them for the duration of the segment. Scoped GC
collection deferral prevents freshly constructed speculative strings
from being reclaimed before the resulting Values become rooted through
Target on commit. On a late guard failure all speculative writes and
newly constructed values are abandoned. No GC-managed pointer is
embedded in an executable module.

List append/replace/insert preserve the source's **full tagged Value**,
including numeric NaN, strings and booleans. These operations use a
journal and commit only if the entire compiled segment succeeds.
A deopt or trap discards staged list writes and speculative string values.
The numericOnly optimization flag remains conservative even when a
nonnumeric item is later deleted.

Focused reproduction:

    python3 tests/differential/wasm_string_semantics.py       build-wasm-hotspot-parity/xyo.js       --addon build-wasm-hotspot-parity/binaryen-host.node       --llvm build-list-epoch/xyo       --out scratch-bench/results/binaryen-string-semantics-fixtures

Includes CJK/emoji, nested UTF-16 joins, case-insensitive contains and
comparison, boolean truthiness, mixed-value list guards and late
transactional rollback after 1000+ speculative string writes.

The backend still does not compile all Scratch instructions. General
suspension, recursive procedures, rendering/pen and sound need a fully
effectful resumable shared IR, or the original interpreter. The 2026-11-07
headless scratch-bench parity output matched all 2123 checks in interpreter,
builtin WASM and Binaryen modes; the 224 graphical checks that cannot pass
headlessly do not establish full graphical compatibility.


## Typed list and procedure arguments (continued November 7)

The WASM transaction journal for list variables now preserves full Scratch
Value tags. Reading an item returns its original string, boolean, number
or color; only numeric *consumers* perform Scratch ToNumber conversion.
Append, insert, replace, delete and clear retain speculative order and
roll back atomically. Contains and indexOf call Scratch's runtime equality
logic, including the intentional difference between SameValueZero
matching and coercive Scratch equality. The numericOnly flag is
monotone across committed writes, even when a transient nonnumeric item
is later deleted. All list transactions defer GC until publication
because staged values may be newly created SStr objects.

The special indices 'last', 'all' for deletion, and numeric strings are
interpreted by the original cast::listIndex / cast::toListIndexD functions.
'random' and 'any' always bail before consuming the global RNG; the
interpreter resumes from the start of the current uncommitted region,
avoiding a double RNG draw.

The data_listcontents reporter is compiled through a temporary
GC-protected list view of the speculative journal and the unchanged
rt::listContents formatter. Scratch's single-character-string separator
rule and UTF-16 representation are preserved.

Inlined warp and synchronous non-warp procedures capture arguments
eagerly as exact tagged Values in WASM i64 locals, not coerced f64.
The numeric/boolean/string use site applies the normal Scratch coercion
only when the corresponding reporter requires it. This supports nested
procedures, string arguments, booleans, and numeric-looking strings.

Reproduce:

    python3 tests/differential/wasm_tagged_list_semantics.py       build-wasm-hotspot-parity/xyo.js       --addon build-wasm-hotspot-parity/binaryen-host.node       --llvm build-list-epoch/xyo       --out scratch-bench/results/binaryen-tagged-list-fixtures

Every fixture compares Interpreter, builtin WASM, real Binaryen optimizer
and native LLVM HotSpot. The legacy numeric-list ABI smoke now expects
tagged list-get helper 23 instead of the old f64-only helper 6.

Headless parity (November 7): 94 scripts compiled in both WASM modes;
Interpreter, builtin and Binaryen each reported 1899/2123 and all 2123
outputs matched. Graphics tests require an actual renderer. This does not
establish complete LLVM JIT parity; arbitrary recursion, general
suspension, arbitrary reporter side effects and rendering/sound are
outside the direct WASM coverage.


## Interpreter-owned nested WASM continuation (experimental)

See [nested-wasm-continuation-2026-11-07.md](nested-wasm-continuation-2026-11-07.md)
for the source-IR checkpoints, retained Scratch StackFrame, type/GC
safety, live variable-binding generations, project edit invalidation,
bounded warp control-tree compilation and differential tests.

Unlike whole-script HotSpot, the interpreter remains authoritative for
procedure calls, loop counters and yield. The optional
`XYO_NESTED_WASM=1` enables this only in WASM HotSpot builds;
`XYO_NESTED_WASM_ALL=1` also enables expensive mixed-type regions.
`XYO_NESTED_WASM_WARP_ONLY=1` restricts the experiment to complete,
pure numeric warp control trees, including nested repeat/until/while/for-each,
if/if-else, nonrecursive warp helper calls and canonical numeric tail
recursion with 0-4 arguments, plus zero-argument non-tail warp
self-recursion with exactly one return site and pure numeric prefix/suffix. `XYO_NESTED_WASM_STATS=1` emits measured
compile/execution/guard counts. Every mode is **off by default**.

The emitter pre-guards rounded repeat bounds and shares one 1024-iteration
fuel across nested control loops. Guard failure leaves all speculative
writes uncommitted. Non-warp loops retain their per-tick yield boundary.
Numeric if/if-else fragments can run using a lightweight scalar ABI,
but arbitrary recursion and general effects still fall back.
The bounded tail-recursive path requires numeric-literal entry arguments,
provably numeric recursive updates, and a final self-recursive IF;
other non-tail forms (multiple return sites or procedure arguments)
and all nonwarp recursion retain interpreter scheduling. A single-site
non-tail recursion uses a compressed i32 LIFO continuation depth and
rolls back speculative writes when the shared fuel runs out.
