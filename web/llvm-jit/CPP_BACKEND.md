# Browser C++ LLVM frontend

The current xyo-cpp C++ `jitEmitScript` frontend is compiled to wasm32 using LLVM 19.1.7 and Emscripten 6.0.12. The browser loads `xyo_browser_ir.mjs/.wasm`, calls its C++ API to compile admitted SB3 scripts, verifies its ABI metadata, and uses the browser-Wasm LLVM `llc` and `lld` tools to produce dynamically linked side modules.

**Verified:** complete SB3 → Wasm C++ codegen → Wasm LLVM/LLD → shared xyo runtime execution under both Bun/Node and headless Google Chrome, with variable value `42`. See [README.md](README.md) for build, setup, test and limitations.

The complete bootstrap script has not been rerun from a clean checkout, but every underlying build stage and the resulting artifacts have been tested. The compiler and toolchain images are build outputs, not committed binaries.

The C++ compiler is restricted by the existing admission gate to short, nonyielding, numeric set/change scripts. Unsupported projects run in the interpreter. If the compiler artifact pair is missing, a restricted TypeScript lowering remains available. The LLVM side module shares the host's linear memory; this is not a sandbox.

Native x86_64 C++ LLVM output must not be used as wasm32 code, because the structure offsets differ. The ABI verifier prevents this mismatched combination.

Derived from [pnsk-lab/xyo-editor](https://github.com/pnsk-lab/xyo-editor), with attribution and MIT license in [THIRD_PARTY_LICENSE](THIRD_PARTY_LICENSE).
