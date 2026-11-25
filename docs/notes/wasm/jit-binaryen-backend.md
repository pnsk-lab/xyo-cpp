# Binaryen WebAssembly backend (LLVM-free)

## Overview

xyo-cpp can emit standalone WASM without LLVM by lowering eligible Scratch blocks into a backend-neutral wasm::Program and encoding a module. The Binaryen backend optimizes this binary **directly in-process** using the Binaryen C API, when available. It includes the system header binaryen-c.h and uses libbinaryen.so through a local, deep-bound dlopen on Linux; on other platforms it links the Binaryen C library directly. No wasm-opt subprocess or temporary files are needed in this configuration.

The Linux loader arrangement is intentional: the Arch package of libbinaryen.so exports LLVM symbols as well, which can conflict with xyo's LLVM ORC JIT if added as a normal DT_NEEDED dependency. The Binaryen C ABI is resolved with dlsym to keep the LLVM symbols from interposing on xyo's LLVM.

When the C library is absent, the former wasm-opt command-line implementation is used as an optional fallback. No optimized bytes are claimed if the selected engine fails.

## Build on Arch Linux

~~~sh
sudo pacman -S binaryen

cmake -S . -B build-no-llvm \
  -DXYO_JIT=ON \
  -DXYO_JIT_BACKEND=none \
  -DXYO_SDL2=OFF \
  -DXYO_BINARYEN_C_API=ON

cmake --build build-no-llvm --target xyo-cli -j8
~~~

CMake switch XYO_BINARYEN_C_API: AUTO (default, enable C API if available), ON (require header and library), OFF (disable library). This option is independent of XYO_JIT_BACKEND; linking Binaryen never requires LLVM.

Engine selection through environment variable XYO_BINARYEN_ENGINE:
- auto (default): prefer available in-process Binaryen C API, otherwise wasm-opt external.
- library: require in-process Binaryen; do not spawn wasm-opt.
- external: require wasm-opt from PATH or XYO_WASM_OPT and spawn it as a subprocess. 30-second timeout.

## Compile an eligible Scratch script

~~~sh
XYO_BINARYEN_ENGINE=library build-no-llvm/xyo wasm project.sb3 \
  --script TOP_BLOCK_ID \
  -o script.wasm \
  --backend binaryen \
  --opt 2

build-no-llvm/xyo wasm project.sb3 --script TOP_BLOCK_ID \
  -o builtin.wasm --backend builtin
~~~

The result is a standard WASM binary plus a script.wasm.json manifest with selected script, target index, variable slot order, backend and ABI.

The function ABI is f(i32 slots) -> i32, with imported env.memory (at least 256 pages) and packed 64-bit Scratch values. Return status 0 means completion; 1 indicates a bailout. Neither this binary nor its ABI can be passed directly as a native JitBackend::CompiledScript ScriptFn.

The C API implementation reads the existing WASM binary into a Binaryen module, validates it, runs optimization at O0–O3, validates again and serializes the result to a byte vector. The Binaryen optimization settings are global, so this path holds a mutex and restores previous settings. All Binaryen buffers and module objects are freed correctly.

## Supported subset and safety

Only eligible green-flag scripts with numeric variables, arithmetic, rounding, numeric literal operations, and bounded repeat loops are supported. Unsupported Scratch operations reject compilation. Multiple compilable scripts require an explicit --script selector.

Generated code must not be applied directly to live Scratch variable storage without a transactional snapshot/commit strategy: a late bailout could otherwise duplicate side effects when returning to the interpreter. Variable IDs are stored in the manifest so an external host can bind the correct slot ordering.

## Validation

~~~sh
XYO_TEST_BINARYEN_NATIVE=1 \
python3 tests/differential/wasm_binaryen_backend.py \
  build-no-llvm/xyo --out build-no-llvm

ctest --test-dir build-no-llvm --output-on-failure -R xyo-wasm-binaryen-backend
~~~

This differential test runs generated code in Node.js, checking output from the builtin, C API and external Binaryen engines where available. It also verifies script-selection errors, input validation, and external-compiler failure behavior.

## Follow-up work

For a true in-browser JIT, package the Binaryen C library for Emscripten or port the chosen passes into an in-browser compilation worker. Integrate the generated module with the runtime's callable representation, code ownership, state/GC safety, and yield/bailout behavior. Extend coverage beyond numeric-only operations and compare end-to-end compile + instantiate + execute latency with LLVM.

## Standalone WASM microbenchmark (2026-11-06)

Run `python3 tools/benchmark_binaryen.py build/xyo --runs 7 --iterations 20000 --output build/binaryen-benchmark-20261106.json`. The benchmark generates an identical SB3 script (a 1000-iteration numeric loop) and checks the resulting variable values in Node.js. It times a fresh xyo CLI process including WASM emission, Node WebAssembly.Module compilation/instantiation, and repeated execution. It is **not** a ScratchBench score or a native LLVM JIT comparison.

| Backend | WASM bytes | xyo subprocess+compile median (ms) | Node execute median (ns/call) |
|---|---:|---:|---:|
| builtin | 553 | 16.34 | 3369.55 |
| Binaryen C API, O2 | 414 | 20.71 | 3607.26 |
| wasm-opt subprocess, O2 | 414 | 26.57 | 3515.32 |

These single-host figures include process startup in the xyo compile column, and Node runtime JIT tiering and system variability in the execute column. The Binaryen binary is 25.1% smaller, but neither optimized variant was faster for this particular workload. Do not enable Binaryen solely on the basis of WASM binary size; repeat this test on several scripts, distinguish process startup from compiler-only time, and compare end-to-end lifetime for HotSpot promotion.

A single backend-neutral numeric Program is already used by the builtin WASM emitter and the Binaryen optimizer. The native LLVM JIT still lowers Scratch directly to LLVM IR, so **cross-LLVM typed SSA unification is not yet implemented**. Such a refactor requires value representation, deoptimization/scheduler continuity, and GC roots to be modeled explicitly before LLVM or WASM consume it.
