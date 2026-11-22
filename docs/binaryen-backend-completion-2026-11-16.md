# Binaryen backend completion — 2026-11-16

## Status

The Binaryen backend is complete for its defined scope: it optimizes the
backend-neutral WebAssembly produced by `ir::lowerExecutable` / `wasm::Program`,
runs that module in the normal WASM runtime, and falls back to the built-in
WASM emitter or the interpreter when Binaryen is unavailable or an opcode is
outside the shared numerical subset.

What this does **not** claim (unchanged, tracked outside BE-001/IR-002):

- Binaryen emits WASM, not native machine code. The native LLVM ORC JIT is a
  separate backend.
- The shared *effectful* Scratch executable IR (strings/lists/GC/audio/
  rendering/events/clones independent of the legacy lowerers) is IR-003 work.
- Arbitrary recursive procedures and resumable yield/deopt continuation are
  RT-001 / IR-001 work. The Binaryen path keeps those regions
  interpreter-owned instead of guessing.

## Build fixes found while completing verification

1. **WASM/Binaryen build was broken by LLVM-only headers.**
   `src/ir/source_reporter_ssa_location_bridge.hpp` is portable, but it
   included `llvm_source_reporter_trace.hpp`, which unconditionally included
   `llvm_ssa_liveness.hpp` (`<llvm/IR/CFG.h>`). Any Emscripten build therefore
   failed with `'llvm/IR/CFG.h' file not found` in `sequencer.cpp`,
   `rt.cpp` and `rt_blocks.cpp`.
   Fix: the portable records stay unconditional; the LLVM observation
   structs and `traceSourceReporterValues` are guarded by
   `__has_include(<llvm/IR/Module.h>)`. Non-LLVM translation units now consume
   the frozen source-slot records without LLVM headers.
2. **Interpreter-only WASM builds failed to link.**
   `src/engine/sequencer.cpp` references `xyo_lt` from its opt-in
   `XYO_JIT_MEGA_FAST_VAR_PAIR_JOIN` block. `XYO_JIT=OFF` builds use
   `src/jit/nojit_stub.cpp` and had no definition, so `xyo.js` failed with
   `undefined symbol: xyo_lt`.
   Fix: the no-JIT stub now provides `xyo_lt` with the same
   `cast::compare` Scratch semantics. Compiled JIT builds keep the cached
   fast path in `rt.cpp`.
3. **Documented JIT recipe was incomplete.**
   `docs/wasm.md` used only `-DXYO_WASM_JIT=ON`, which does not enable the
   wasm JIT backend: `cmake/emscripten.cmake` forces `XYO_JIT=OFF` unless
   `XYO_JIT_BACKEND=wasm`. The recipe now passes it and explains why.

## Verification (this build, i7 host, LLVM 23, Binaryen 130 / libbinaryen)

Native and Emscripten builds:

- `build-wasm-be`: `-DXYO_JIT_BACKEND=wasm -DXYO_WASM_JIT=ON`, builds
  `xyo.js` and `binaryen-host.node`.
- `build-wasm-int`: interpreter-only, links again.
- `build`: native LLVM CLI.

| Suite | Result |
| --- | --- |
| `wasm_binaryen_backend.py` (native builtin/Binaryen, standalone, host engines) | PASS |
| `common_cfg_backend_parity.py` (Interpreter/LLVM/Builtin/Binaryen) | 33/33 |
| `common_cfg_procedure_parity.py` (four-way Call/Return/deopt) | 55/55 |
| `wasm_effect_fallback_parity.py` (broadcast, broadcast-and-wait, wait, looks, unknown in 3 positions) | 8/8 |
| `wasm_hot_emitter_selection.py` (selection/fallback matrix) | 6/6 |
| `wasm_hotspot_integration.py` | PASS (incl. unsupported string/procedure/wait) |
| `wasm_compat_fuzz.py --cases 32` | PASS 32 seeds |
| execute fixtures — native | 115/116 |
| execute fixtures — WASM JIT | 115/116 |
| execute fixtures — WASM interpreter | 115/116 |

The single fixture miss is `control-stop-all-leaks.sb2` (`pass=1 fail=0 end=0`)
and is identical in the native interpreter, native JIT, WASM JIT and WASM
interpreter, so it is an environment/step-count issue in that fixture, not a
backend divergence.

## Unsupported-effect fallback evidence

The guarantee is structural and now also re-verified after the build fixes:

- Every opcode that the shared IR does not classify falls into the
  conservative opaque branch: `Unknown | ReadState | WriteState |
  MayAllocate | MayYield | MayCall` plus the observable-extension prefix
  classification (`src/ir/scratch_effect.hpp`).
- `mayReplaySpeculatively()` is false for `Unknown`, `MayYield`, `MayCall`,
  `Observable` and `MaySpawn`, so such effects can never be replayed after a
  rollback.
- `wasm::lowerCapturedScript` / `lowerCapturedStatement` build a candidate
  program and return `false` for any unhandled opcode. The candidate is
  published only on full success, so no partially lowered module can replace
  the interpreter path.
- Hybrid lowering keeps unsupported statements as interpreter-owned regions
  and still runs the numeric islands through Binaryen.
- `wasm_effect_fallback_parity.py` confirms Interpreter/LLVM/Builtin/Binaryen
  produce identical output for an unknown extension as a statement, inside
  `if`, inside `repeat`, and as a reporter, including a real Binaryen
  optimisation of the surrounding numerical region.

## Reproduce

```sh
# native
cmake --build build -j20 --target xyo
python3 tests/differential/wasm_binaryen_backend.py build/xyo --out /tmp/be

# WASM + Binaryen
source "$HOME/.emsdk/emsdk_env.sh"
export EM_CACHE="$PWD/.emcache"
emcmake cmake -B build-wasm-be -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/emscripten.cmake \
  -DCMAKE_BUILD_TYPE=Release -DXYO_SDL2=OFF \
  -DXYO_JIT_BACKEND=wasm -DXYO_WASM_JIT=ON -DXYO_BUILD_TESTS=OFF
cmake --build build-wasm-be -j20
bash web/binaryen-node/build.sh build-wasm-be/binaryen-host.node

python3 tests/differential/common_cfg_backend_parity.py \
  --native build/xyo --wasm build-wasm-be/xyo.js \
  --addon build-wasm-be/binaryen-host.node
python3 tests/differential/common_cfg_procedure_parity.py \
  --native build/xyo --wasm build-wasm-be/xyo.js \
  --addon build-wasm-be/binaryen-host.node --out /tmp/be-proc
python3 tests/differential/wasm_effect_fallback_parity.py \
  --native build/xyo --wasm build-wasm-be/xyo.js \
  --addon build-wasm-be/binaryen-host.node --out /tmp/be-fallback
python3 tests/differential/wasm_hot_emitter_selection.py \
  --wasm build-wasm-be/xyo.js --addon build-wasm-be/binaryen-host.node
```
