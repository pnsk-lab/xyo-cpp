# xyo-cpp

`xyo-cpp` is a new Scratch-compatible LLVM VM/JIT prototype based on
`scratch_llvm_vm_spec.md` and the checked-in `scratch-editor/packages/scratch-vm`
sources.

The current implementation is an MVP foundation:

- C ABI runtime in `runtime/`
- C++ host/JIT shell in `src/`
- Scratch-like `SValue`, string, cast, compare, operators, variables, lists
- thread/frame/scheduler primitives with pc-based script functions
- green flag, key/click, broadcast, and backdrop-switch hats
- sprite state, clone basics, and draw command queue
- LLJIT script-entry generation that runs compiled scripts through a native
  pc-state-machine entry, plus a smoke path that calls a C runtime helper from
  generated LLVM IR

The host side owns process startup, LLJIT setup, input collection, and rendering.
When runtime bitcode is available, green-flag start, thread scheduling, tick
execution, and runtime thread queries are resolved from LLVM bitcode so the host
runner no longer drives execution-thread management directly. Scratch block
semantics still live in the C ABI runtime, so generated LLVM script entries can
drive the existing scheduler/runtime contract while individual blocks are
lowered more aggressively over time.

## Build

The runtime/JIT tests do not need a graphics backend:

```sh
make test
```

The executable uses SDL2 and `librsvg-2.0` through `pkg-config`, plus the Skia
C++ API for its persistent pen layer. The pen layer uses Skia's Ganesh OpenGL
GPU backend when SDL provides an OpenGL context, and falls back to raster mode
for headless SDL drivers. Skia is built separately with GN/Ninja;
the [official Skia build guide](https://skia.org/docs/user/build/) describes the
supported flow. The default non-official configuration embeds Skia's third-party
dependencies in the static library, which keeps this link path self-contained:

```sh
cd /path/to/skia
python3 tools/git-sync-deps
bin/gn gen out/Static --args='is_debug=false cc="clang" cxx="clang++" skia_use_gl=true'
ninja -C out/Static skia
```

Point this project at the Skia source root and GN output directory:

```sh
make SKIA_ROOT=/path/to/skia SKIA_OUT=/path/to/skia/out/Static
make test-skia SKIA_ROOT=/path/to/skia SKIA_OUT=/path/to/skia/out/Static
./build/scratch-llvm-vm
```

### WebAssembly / GitHub Pages

The Web build uses the C runtime and interpreter through Emscripten. It does
not require the native LLVM JIT, SDL2, or Skia. Activate an Emscripten SDK and
run:

```sh
bash scripts/build-web.sh
python3 -m http.server 8080 --directory build-web/site
```

Open `http://localhost:8080/` and select an `.sb3` project. The generated page
loads the project into the WASM virtual filesystem and renders the stage on a
Canvas. A browser will not load the generated ES module correctly from a
`file://` URL, so use a local HTTP server.

To exercise the LLVM Web JIT path locally, build a WebAssembly-targeted
`llvm-project` checkout first, then set its source and build directories:

```sh
LLVM_SOURCE_DIR=/path/to/llvm-project \
LLVM_BUILD_DIR=/path/to/llvm-web-build \
LLVM_CMAKE_GENERATOR=Ninja \
bash scripts/build-web-llvm.sh

SJIT_WEB_LLVM_JIT=ON \
SJIT_WEB_LLVM_SOURCE=/path/to/llvm-project \
SJIT_WEB_LLVM_BUILD=/path/to/llvm-web-build \
bash scripts/build-web.sh
```

The `web-llvm-jit` job in `.github/workflows/ci.yml` builds the pinned LLVM/lld
sources, caches the cross-build, and runs a dynamic WebAssembly reload smoke
test. The normal Web build remains the smaller interpreter build unless
`SJIT_WEB_LLVM_JIT=ON` is set.

The `.github/workflows/pages.yml` workflow repeats this build on every push to
`main`, enables the LLVM Web JIT path, and deploys `build-web-llvm/site` with
GitHub Pages. In the repository
settings, set Pages' source to **GitHub Actions** once; subsequent pushes are
published automatically.

If a packaged Skia supplies `skia.pc`, plain `make` discovers it automatically
and requests its static-link dependencies. Set `PKG_CONFIG_PATH` when the file
is outside pkg-config's normal search path. For a GN configuration that uses
system libraries, append its required link flags with `SKIA_EXTRA_LDFLAGS`.
Run `make clean` when switching between different Skia roots or build options.

Run a specific Scratch 3 project:

```sh
./build/scratch-llvm-vm project.sb3
```

Open a window for a Scratch 3 project:

```sh
./build/scratch-llvm-vm --window project.sb3
```

The window is a minimal SDL2 host renderer for the runtime draw command buffer.
It shows the 480x360 Scratch stage, rasterized SVG costume textures (including
position, size, rotation center, and direction), layer-aware costume hit
testing, and variable monitor bars.
The window can be resized while running; the stage keeps its 4:3 aspect ratio
and is centered in the window. Set the initial stage scale with
`--stage-scale N` (aliases: `--scale N`, `--window-scale N`), for example:

```sh
./build/scratch-llvm-vm --window linux.sb3 --stage-scale 3
```

Pen strokes use an independent high-resolution buffer (at least 960x720);
changing pen resolution does not enlarge the visible Scratch stage.

Press Escape or close the window to exit. The window build requires SDL2 and
`librsvg-2.0` (available through `pkg-config`) and Skia.

For headless smoke checks, limit the number of rendered frames:

```sh
SDL_VIDEODRIVER=offscreen ./build/scratch-llvm-vm --window project.sb3 --frames 3
```

Set the host frame rate and enable turbo-mode scheduling:

```sh
./build/scratch-llvm-vm --window 3D.sb3 --fps 60 --turbo
```

The same runtime options also work in the headless project runner:

```sh
./build/scratch-llvm-vm 3D.sb3 --frames 600 --fps 120 --turbo
```

The runner defaults to TurboWarp compatibility. Select the Scratch preset or
override the list capacity at runtime when needed:

```sh
./build/scratch-llvm-vm linux.sb3 --compatibility turbowarp --list-limit 67108864
./build/scratch-llvm-vm project.sb3 --compatibility scratch --list-limit 200000
```

TurboWarp compatibility also normalizes its extra keyboard names (`enter`,
`delete`, `shift`, `caps lock`, `home`, `page up`, and others), supports the
`any` key query, and uses TurboWarp's case-insensitive letter behavior.
Vertical mouse-wheel events also start the matching `up arrow` or `down arrow`
key hats, as in TurboWarp, without making that key appear held to
`key pressed?`. The `--turbo` option remains the separate scheduler speed
setting.

Any bundled `.sb3` project can be opened through the same window path:

```sh
./build/scratch-llvm-vm --window 3D.sb3
./build/scratch-llvm-vm --window 3D2.sb3
./build/scratch-llvm-vm --window 3DOcean.sb3
./build/scratch-llvm-vm --window teapot.sb3
```

`3D2.sb3` intentionally waits for a mouse click during its intro. Its ghost and
pixelate transitions are rendered by the host before it broadcasts the start of
the main renderer. `3DOcean.sb3` uses stage backdrop selection and backdrop hats;
`teapot.sb3` uses the sprite drag-mode block.

The LLVM runtime owns the persistent pen path in a small growable array. The
C++ host reads it through the runtime pen-path ABI and incrementally rasterizes
round, anti-aliased strokes into a GPU-backed Skia `SkSurface`; SDL presents
that layer before sprites. SVG costume data is loaded from the SB3 archive and rasterized
once into an SDL texture per costume; variable monitors render text with a tiny
built-in bitmap font.

Emit LLVM IR for the generated smoke JIT module:

```sh
./build/scratch-llvm-vm --emit-ll build/sjit_smoke.ll
```

Emit LLVM IR for the first generated script entry in an `.sb3` project:

```sh
./build/scratch-llvm-vm --emit-ll quicksort.sb3 build/quicksort.ll
```

If a project contains multiple registered hat scripts, the first script uses the
requested path and later scripts are written with numeric suffixes such as
`build/project.1.ll`.

Emit LLVM IR for the runtime execution side, including `sjit_runtime_tick` and
`sjit_scheduler_tick`:

```sh
./build/scratch-llvm-vm --emit-runtime-ll build/sjit_runtime.ll
```

Emit linked runtime LLVM IR:

```sh
make runtime-ll
```

This writes `build/sjit_runtime.ll`.

The current SB3 runner supports green-flag scripts, variables, lists, custom
blocks with arguments, core arithmetic/string reporters, `repeat`, `repeat
until`, `if`, `if else`, `forever`, timer/key/mouse reporters, pen primitives,
background switching and hats, graphic-effect state, drag mode, and a growing
subset of motion/looks blocks. `operator_contains` has a native value-slot ABI
path with the same case-insensitive semantics as the interpreter.

When an SB3 project is loaded, each green-flag script is registered with an
LLVM-generated script entry when LLJIT is available. The generated entry owns
the resumeable pc loop, including control substacks for `repeat`, `repeat
until`, `while`, `forever`, `for each`, `if`, and `if else`. Block-specific
C ABI helpers still provide Scratch value conversion and runtime side effects,
but generated script IR no longer falls back to the statement interpreter for
ordinary block dispatch. Arithmetic, numeric comparisons, timer/mouse reporters,
literal key sensing, and numeric/bool variable hot paths are lowered to direct
LLVM loads/stores/math with helper fallback only for dynamic conversions. If JIT
setup or compilation fails, registration falls back to the interpreter entry.

JIT correctness is guarded by a centralized per-opcode effect table. A reporter
that the loader and interpreter understand but native lowering does not yet
implement causes an explicit interpreter fallback for the affected script or
procedure lowering unit; an unknown SB3 reporter or statement is rejected with
its opcode and block id instead of being silently replaced by an empty string,
zero, or a no-op. Set
`SJIT_LOG_JIT_FALLBACK=1` to print native-eligibility reasons in release builds.
Set `SJIT_LOG_JIT_TIMING=1` to print the end-to-end module construction,
optimization, machine-code materialization, and symbol-lookup time for each
compiled script entry.
All three Scratch `control_stop` variants are represented explicitly; `all` and
`other scripts in sprite` retain their scheduler-wide effects across the JIT
generic boundary.

Within a non-yielding native region, the current pc stays in an LLVM local that
the O3 pipeline can promote to SSA. The entry commits `SFrame::pc` at observable
pause points and completion, so it remains a scheduler resume record rather than
a store performed after every block. Eligible procedures also have a guarded
numeric-argument path: numeric-only callees receive a `double` array without
boxed `SValue` arguments, while type-guard failures use the generic procedure
semantics.

When CMake is available, the intended flow is:

```sh
cmake -S . -B build-cmake \
  -DSKIA_ROOT=/path/to/skia \
  -DSKIA_OUT=/path/to/skia/out/Static
cmake --build build-cmake
ctest --test-dir build-cmake
./build-cmake/scratch-llvm-vm
```

To build only the runtime/JIT tests on a machine without Skia, configure with
`-DSJIT_BUILD_HOST=OFF`. If a Skia package exports a supported CMake target, it
is auto-detected when `SKIA_ROOT` is omitted. System-library GN builds can pass
additional link dependencies through `-DSKIA_EXTRA_LIBRARIES="..."`.

The `runtime-bitcode` target compiles the runtime C sources, except for the
host-resident persistent thread pool, plus the minimal QuickJS-derived
`dtoa`/`atod` subset into LLVM bitcode and links them into
`build/sjit_runtime.bc`. `JitEngine` loads that bitcode when it is present
or when `SJIT_RUNTIME_BC` points at an alternate runtime bitcode file.
Override bitcode/object artifacts must match the current runtime ABI and leave
the five `sjit_thread_pool_*` host symbols undefined; legacy artifacts that
embed pool definitions are not compatible with the host-resident lifetime
guarantee. During script compilation, a reviewed leaf subset of
numeric/frame/sprite runtime
helpers is linked while still external, then internalized before the LLJIT O3
pipeline; this exposes helper
bodies and compiler-inferred attributes for cross-boundary inlining and
load/store optimization. Cold interpreter/deoptimization entries remain shared
external boundaries so they do not pull the complete interpreter into every
script. Unreachable definitions are removed before JIT optimization. Large
local list-number and cached-compare dispatch helpers are kept out of line when
they have more than 16 uses, avoiding control-flow duplication in renderer-sized
modules. Modules above 10,000 pre-optimization instructions use the O1 pipeline;
smaller modules retain O3. `SJIT_JIT_OPT_LEVEL=0`, `1`, `2`, or `3` overrides
that size policy for diagnostics. Set
`SJIT_DISABLE_SCRIPT_RUNTIME_LINK=1` to retain the external
helper boundary for diagnostics or comparisons. Set
`SJIT_DISABLE_SCRIPT_JIT=1` to keep the same runtime/scheduler but execute every
loaded script through the interpreter for differential testing.

Compiled entries borrow the immutable `SCompiledScript` and its complete AST,
so that arena must remain at stable addresses until the owning `JitEngine` is
destroyed. The engine must also outlive every runtime registration that can call
one of its entries. A runtime-specialized entry first checks a monotonic runtime
instance id, then may borrow the original, runtime-owned `SSprite` object for
that invocation. It reloads the owner's reallocatable variable-array base and
uses an append-only `(owner, index, type)` handle; normal code generation does
not embed an `SVariable *` or `SList *`. Handle resolution is hoisted once per
native script/procedure invocation. A different runtime, clone, or dynamic
target selects the interpreter before any borrowed owner is dereferenced.
Interpreter/helper variable caches pair their runtime pointer with the same
instance id, so allocator address reuse after runtime destruction cannot revive
a stale owner-id/index entry.
The renderer's fused color-list/brightness-list/replace/stamp path receives
those checked, invocation-local variable handles directly. This removes
per-pixel name/cache resolution without embedding raw variable or list pointers,
while retaining the interpreter's list-index conversion and side-effect order.
For a warp-mode pen row whose complete loop shape matches the supported AST
exactly, the JIT can additionally select a guarded native row kernel. It makes
no Scratch-visible mutation until every runtime guard and pen-buffer reservation
succeeds; otherwise execution continues through the existing LLVM loop. Set
`SJIT_LOG_NATIVE_PEN_ROW=1` to log the first activation or rejection reason.
The opt-in `SJIT_UNSAFE_EMBED_RUNTIME_POINTERS=1` mode exists only for profiling
the old strategy and does not provide that lifetime guarantee.

Each registered script also keeps a saturating 64-bit invocation counter. The
scheduler increments it on thread start/restart, not on yield/resume re-entry,
so it is a low-overhead foundation for later tiering without adding work to hot
native loops.

## Ownership-Proven Parallel Scheduling

The SB3 loader runs a fail-closed ownership analysis over each registered
script and every reachable custom procedure. The first parallel tier accepts
only original, non-stage sprites whose mutable state is confined to
pointer-free local number/bool variables. Stage/global variables, dynamic or
string-owning scalar storage, lists, clones, random/live-clock operations,
and waits remain sequential, as do shared effects such as drawing, motion,
looks, monitors, broadcasts, or global stops. Unknown, malformed, missing, or
recursive procedure trees also fail the proof. Scripts dominated by list or
pen operations may therefore report zero parallel-safe entries; this is the
intended safety boundary, not a failed optimization.

Native ownership certificates can be attached only through `JitEngine`. Its
private registry must bind the exact AST to the compiled top-level entry and
the complete vector of procedure entries before certification. The runtime
then snapshots those pointers and the scheduler rejects any later entry swap.
The public `sjit_runtime_analyze_script_ownership()` API certifies only a pure
interpreter registration whose procedures have no native entries; it cannot be
used to attest an arbitrary native function pointer.

The full reachable AST, including variable/procedure cache provenance, is
checked once during analysis and attestation. The resulting certificate stores
the deduplicated stable indices of only the owner-local scalars reached by that
AST. Immediately before batching, the scheduler verifies the top-level and
procedure entry snapshots, then performs an O(k) scan of those k dependencies:
each index must remain in bounds and identify a non-cloud number/bool scalar
whose current value has a pointer-free number/bool/null tag. A stale dependency
sends that execution through the sequential path; an unrelated owning local is
not in the manifest and does not cause fallback.

String literal construction deep-copies the bytes into each AST instead of
sharing an `SString`, isolating its non-atomic refcount and numeric cache
between otherwise independent scripts.

In each scheduler pass, adjacent runnable entries with valid ownership
certificates are batched only when both their owner targets and compiled-script
arenas are distinct. A lazily created fixed pthread pool runs the entries, with
the scheduler thread participating in the work. The batch is joined before any
unproven entry or scheduler-status boundary is crossed, and entry results are
then committed in the original thread-vector order. Completion timing cannot
therefore reorder status transitions, restarts, or cleanup. Two individually
safe scripts for the same sprite are deliberately serialized.

Pool creation, task-buffer allocation, or batch submission failure falls back
to the existing sequential path, as do missing/stale certificates and batches
smaller than two. Builds with `SJIT_PROFILE_RUNTIME` also stay sequential
because their low-overhead profiling counters are non-atomic. The pool is
persistent for the lifetime of the runtime and is destroyed before runtime
objects are released.

The persistent pool implementation stays in the process-resident native host
image rather than `sjit_runtime.bc`. `JitEngine` absolute-binds the pool's
external symbols before loading runtime code. A completed batch clears its
temporary task callback, so idle workers execute only the host worker loop and
hold no instruction pointer into an unloadable ORC module. Destroying a
`JitEngine` therefore cannot invalidate an idle pool, while native script-entry
lifetime rules still apply before any later script execution.

The normal loader/host also provides the external serialization assumed by
this design: host-facing runtime lifecycle, event, input, and tick calls run on
one host thread, and a tick never overlaps another tick, input mutation, or
runtime destruction. After native compilation and ownership attestation, the
semantic AST and procedure table remain immutable at stable addresses for the
certificate's lifetime. Only standard helpers may populate implementation
caches, and they must preserve the owner/index/procedure meaning established at
attestation. The hot scheduler therefore need not rescan cache provenance. The
standard project loader and host follow both contracts.

The pool can be configured and inspected with these environment variables:

- `SJIT_THREAD_POOL_SIZE=N` selects the total parallelism, including the
  scheduler thread. The default is the number of online CPUs, capped at 32;
  `N=1` selects sequential execution.
- `SJIT_DISABLE_THREAD_POOL=1` forces the sequential scheduler.
- `SJIT_LOG_THREAD_POOL=1` logs pool initialization and the first active
  parallel batch.
- `SJIT_LOG_OWNERSHIP_ANALYSIS=1` logs each script's proof result and rejection
  bit mask.

Runtime query functions expose each script's proof result and rejection flags,
the active pool parallelism, and saturating parallel batch/task counters for
tests and diagnostics.
