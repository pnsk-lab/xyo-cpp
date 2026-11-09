# Shared JIT frontend and backend parity (2026-11-07)

## Active architecture

The synchronous JIT captures a single `ir::ScratchCompilationUnit`
(`src/ir/scratch_pipeline.hpp`) and passes that same snapshot through
`JitBackend::compile()` to LLVM or WASM. Procedure definitions reachable
through calls and conservative effects are captured once.

Two execution paths still exist:

1. **Portable numeric CFG (opt-in)**:
   `ScratchScript → ScratchNumericKernel → Executable → lowerExecutable()`
   → `LlvmCfgBackend` or `WasmCfgBackend` → optional Binaryen.
   Uses the same transactional slot ABI and resumable SSA frames.
   Supported operations include +, -, ×, ÷, numeric comparisons,
   abs, Scratch/JS round, and pure Boolean and/or/not expressions. Top-level repeat and selected nested conditionals
   are supported subject to deoptimization guards.
2. **Legacy general-purpose JIT**:
   LLVM uses `jitEmitScript()` and the WASM backend uses
   `wasm::lowerCapturedScript()` followed by `wasm::Program`.
   They consume the same source snapshot, but **do not generate executable
   IR from the same general-purpose lowering**. All unsupported cases
   must preserve the interpreter fallback.

The shared CFG is intentionally opt-in via `XYO_JIT_COMMON_CFG=1`
until the full source feature set and performance are validated. In this
mode neither branch silently claims support for unknown opcodes.

## Verified tests

End-to-end parity fixture generator and runner:

```sh
python3 tests/differential/common_cfg_backend_parity.py \
  --native build-list-epoch/xyo \
  --wasm build-wasm-hotspot-parity/xyo.js \
  --addon build-wasm-hotspot-parity/binaryen-host.node
```

This compares **actual generated code** from LLVM, Builtin WASM and
Binaryen, not just WASM files or IR shape. A compilation log is required
from each backend. It covers static/dynamic repeat counts, negative and
fractional counts, numeric arithmetic, abs, JS rounding and
Boolean conditionals.

Whole ScratchBench parity can be compared using
`tests/differential/wasm_scratchbench_backend_parity.py`.
The headless Node environment does not support every visual parity check;
the three outputs, not only their passed counts, must match.

## Remaining work (not yet implemented)

- Replace `src/jit/codegen.cpp`'s general-purpose Scratch semantics and
  `src/wasm/wasm_compile.cpp`'s independent expression/statement lowering
  with one *effectful*, typed IR.
- Add portable runtime ABI for strings, lists, costumes, audio, sensing,
  events, broadcast, clones and extensions; keep GC roots explicit.
- Model general `wait`, nested yield, recursive procedures, warp and OSR
  as a shared continuation stack and deoptimization map.
- Establish full 2,123-case test coverage in an environment with rendering,
  plus differential fuzzing and performance regressions.
- Do not delete legacy paths until new parity, performance and recovery
  behavior are demonstrably equivalent.

A shared source snapshot is **not** by itself proof of general-purpose
backend equivalence. Only the portable numeric CFG currently provides
identical executable IR to LLVM and WASM.
