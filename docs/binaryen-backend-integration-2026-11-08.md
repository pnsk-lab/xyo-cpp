# Binaryen backend integration — 2026-11-08

## Scope

This change **does not** implement a general-purpose Scratch compiler from
scratch. It removes three redundant Binaryen selection/optimization paths in
the existing Emscripten JIT and allows the portable LLVM/WASM numeric CFG to
use the same Binaryen optimization entry point as `wasm::Program`.

- `selectHotWasmBackend(requested, hasCAPI, hasHostOptimizer)`: one
  deterministic selection function for full script, hybrid region and
  resumable numeric CFG.
- `optimizeWasmWithBackend(bytes, backend, level)`: optimizes already-lowered
  WASM (including the backend-independent executable CFG).
  `compileWithBackend(program, ...)` is now a wrapper around this API.
- Both in-process Binaryen C API and synchronous host Binaryen hooks remain
  supported. Explicit `XYO_WASM_HOT_EMITTER=binaryen` fails closed if neither
  optimizer exists; `auto` falls back to builtin.
- When compiling a common numeric CFG, the frontend lowering remains the
  existing `ir::lowerExecutable`; no second Scratch opcode compiler is used.

## Selection semantics

| XYO_WASM_HOT_EMITTER | C API | Host optimizer | Selection |
| --- | --- | --- | --- |
| builtin | any | any | built-in WASM, no optimization |
| auto | yes | any | Binaryen C API |
| auto | no | yes | host Binaryen |
| auto | no | no | built-in WASM |
| binaryen | yes | any | Binaryen C API |
| binaryen | no | yes | host Binaryen |
| binaryen | no | no | reject JIT, interpret |
| other | any | any | reject JIT, interpret |

Host optimization is applied at instantiation in `host_bridge.cpp`; the C API
optimizes the bytes before instantiation. A failed optimization must never
pretend to be a successful JIT compilation.

## Verification

Native and Emscripten build:
```sh
cmake --build build-list-epoch --target xyo-cli -j4
cmake --build build-wasm-hotspot-parity --target xyo-cli -j4
```

Shared CFG parity: 22/22 four-way (Interpreter/LLVM/Builtin/Binaryen).
Common CFG procedure parity: 11/11 four-way.
Binaryen selection and fallback: 6/6 in
`tests/differential/wasm_hot_emitter_selection.py`.
Native Binaryen C API/external smoke:
`XYO_TEST_BINARYEN_NATIVE=1 python3 tests/differential/wasm_binaryen_backend.py build-list-epoch/xyo --out build-list-epoch` passed.
Native CTest CFG/slot/ABI checks: 3/3 passed.
Full headless ScratchBench parity: Interpreter, Builtin and Binaryen
**1899/2123** each; output identical for **all 2123** checks. Both JIT paths
compiled 94 scripts, and the Binaryen addon reported real optimizer calls.
The remaining failures are present in the headless interpreter and cannot
be attributed to Binaryen alone.

Logs:
- `board/be001-native-build.log`, `board/be001-wasm-build.log`
- `board/be001-cfg-parity.log`, `board/be001-procedure-parity.log`
- `board/be001-native-binaryen.log`, `board/be001-ctest.log`
- `board/be001-scratchbench-parity.log`
- `scratch-bench/results/be001-scratchbench-parity.json`

## Not implemented

- Direct native machine-code JIT through Binaryen (Binaryen emits WASM,
  not native `ScriptFn` function pointers).
- Full common effectful Scratch executable IR (GC-managed strings/lists,
  audio/rendering/sensing/events/clones) independent of the legacy LLVM and
  WASM lowerers.
- General recursive procedures and arbitrary yield/deopt continuation.
- Browser-side Binaryen packaging and asynchronous compilation workers.

These require completing the shared runtime ABI (IR-001, IR-002 and RT-001)
before expanding the common lowering (IR-003). Existing unsupported blocks
must remain interpreter-driven.
