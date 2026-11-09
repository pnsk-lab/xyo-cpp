// Binaryen-backed optimization for xyo's standalone WebAssembly modules.
// The input is the backend-neutral wasm::Program, not LLVM IR.
#pragma once
#include "wasm_compile.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace xyo::wasm {

enum class WasmBackend : uint8_t { Builtin, Binaryen };

struct WasmCompileResult {
    std::vector<uint8_t> bytes;
    std::string error;
};

// The Emscripten HotSpot JIT can use in-process Binaryen or a synchronous
// host-provided optimizer (Node addon / browser integration). Both execute
// the same generated module in the normal xyo WASM runtime.
struct HotWasmBackendSelection {
    WasmBackend emitter = WasmBackend::Builtin;
    bool hostOptimize = false;
    std::string error;
    bool valid() const { return error.empty(); }
    bool usesBinaryen() const {
        return emitter == WasmBackend::Binaryen || hostOptimize;
    }
    const char* name() const {
        return emitter == WasmBackend::Binaryen ? "binaryen-c" :
               hostOptimize ? "binaryen-host" : "builtin";
    }
};

// Pure selection logic shared by full, hybrid and resumable-CFG compilation.
// No implicit downgrade is allowed when Binaryen was explicitly requested.
HotWasmBackendSelection selectHotWasmBackend(
    const char* requested, bool hasCAPI, bool hasHostOptimizer);

// Accept already-lowered WASM (including the portable Scratch CFG) without
// re-entering the separate wasm::Program frontend.
WasmCompileResult optimizeWasmWithBackend(
    std::vector<uint8_t> input, WasmBackend backend,
    int optimizationLevel = 1);

// Binaryen uses its C API in-process when linked. Otherwise it runs
// wasm-opt from XYO_WASM_OPT or PATH. Set XYO_BINARYEN_ENGINE=library|external
// to require either backend explicitly (default: auto).
// The builtin backend never spawns an external process.
// Supported optimization levels: 0..3.
WasmCompileResult compileWithBackend(
    const Program& program, WasmBackend backend, int optimizationLevel = 1);

} // namespace xyo::wasm
