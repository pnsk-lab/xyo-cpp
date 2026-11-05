# Browser LLVM execution for Scratch SB3 (experimental)

This directory contains the browser LLVM toolchain transport ported from [xyo-editor/packages/llvm-wasm](https://github.com/pnsk-lab/xyo-editor/tree/main/packages/llvm-wasm), plus an **independent, restricted SB3 frontend**.

## What works

For a project containing **exactly one green-flag script**, with up to eight straight-line `data_setvariableto` / `data_changevariableby` blocks and numeric-shadow constants, the frontend:

1. Extracts `project.json` from the SB3 ZIP with bounded input size.
2. Verifies the green-flag script and its initial numeric variables, rejecting unsupported input before execution.
3. Emits wasm32 LLVM IR using double values in a flat variable array.
4. Compiles IR to an object in a browser Worker using **LLVM llc compiled to WebAssembly**.
5. Links it using **wasm-ld compiled to WebAssembly**, then instantiates the resulting module via the native browser WebAssembly API.
6. Seeds the module's memory from the SB3 variables, invokes its compiled function and displays the resulting values.

The same end-to-end code was tested with **native LLVM llc + wasm-ld**, not with browser tool binaries. The 16 total unit/integration tests pass, including two actual ZIP→LLVM→Wasm executions.

## Run locally

This is a separate experimental viewer, not the main xyo CLI or full Scratch editor.

```sh
cd web/llvm-jit
bun install
# Build the browser llc and LLD artifacts in xyo-editor first, following
# packages/llvm-wasm/README.md there. Then:
bun run copy:artifacts -- /path/to/xyo-editor
bun run dev
```

Open the printed Vite URL, select an SB3 file and choose **LLVMで実行**. The copy script expects these four artifact files from the xyo-editor build:

- `llc-browser.mjs` and `llc-browser.wasm`
- `lld-browser.mjs` and `lld-browser.wasm`

The compiler toolchain's Wasm binary assets are deliberately **not committed**. Opening the page without them is possible, but pressing run will fail. In particular, building this repository alone does not build or download LLVM in WebAssembly format.

```sh
bun run check
bun test
bun run build
```

The native integration tests require `llc` and `wasm-ld` on PATH.

## Scope and limitations

**This does not yet execute arbitrary SB3 programs through xyo-cpp's full native `src/jit/codegen.cpp` on the browser.** It is a genuine LLVM→WebAssembly execution path, but the subset frontend generates its own IR and runs a separate memory instance. It does not run alongside xyo::Runtime or update a live Scratch stage. Unsupported projects are **refused** with an interpreter-required message; they are not silently dropped or incorrectly reported as compiled. The `split.ts` port is reserved for future integrated scheduling.

General integration still needs the upstream xyo codegen's wasm32 ABI, real Emscripten runtime hosting, dynamic side-module loading, helper/slot binding, GC roots, scheduler integration and a proper interpreter fallback. xyo-editor's `unified` prototype provides a more complete experimental model, but relies on a pinned, patched old xyo-cpp revision and cannot be copied blindly into the current v1.0.2 runtime.

This is not a sandbox for untrusted machine code, and LLVM compiling in a Worker is not equivalent to runtime parity validation.
