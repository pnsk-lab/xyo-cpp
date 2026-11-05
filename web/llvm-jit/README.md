# Browser LLVM JIT pipeline (experimental)

Ported from [pnsk-lab/xyo-editor/packages/llvm-wasm](https://github.com/pnsk-lab/xyo-editor/tree/main/packages/llvm-wasm), MIT licensed.

This provides the **browser-side LLVM IR → relocatable wasm object → linked wasm module** pipeline using **Emscripten-built LLVM llc and wasm-ld**, each in an isolated Web Worker invocation. It does not implement xyo's Scratch-to-LLVM IR generation or dynamically link generated code to the xyo runtime yet.

## Browser usage

Prebuild `llc.mjs/.wasm` and `wasm-ld.mjs/.wasm` for the browser using the build and relink scripts from `xyo-editor/packages/llvm-wasm`. Serve these immutable artifacts over HTTP(S). The worker only imports *trusted* compiler URLs.

```ts
import { createLLVMWorkerClient, LLVMPipeline } from './src/index';
const client = createLLVMWorkerClient(
  new URL('./src/worker.ts', import.meta.url),
  '/llvm/llc.mjs',
  '/llvm/wasm-ld.mjs',
);
const compiler = new LLVMPipeline(client);
try {
  const { binary, diagnostics } = await compiler.compile({
    ir: 'define i32 @answer() { ret i32 42 }',
    exports: ['answer'],
  });
  const { instance } = await WebAssembly.instantiate(binary);
  console.log((instance.exports.answer as () => number)(), diagnostics);
} finally {
  compiler.stop();
}
```

Use an application bundler that compiles the TypeScript Worker (for example Vite). A valid `llc` and `wasm-ld` pair is mandatory; the project does not download or bundle their multi-megabyte .wasm images. The compiler instances run in their own isolated memories and do **not** automatically share xyo runtime memory or table.

## Current limitations

- This pipeline accepts LLVM **IR**, not `.sb3` files. xyo's `src/jit/codegen.cpp` requires a wasm32-aware, ORC-free adapter before its IR can be fed into this pipeline. Do not pass host-ABI IR directly.
- `sideModule: true` requests PIC/wasm32-unknown-emscripten and a shared link, but actual Emscripten side-module loading, helper resolution, GC roots, scheduler, ABI layout and return values still require integration and validation.
- No code is placed in executable native linear memory; `WebAssembly.validate` is only format validation, not runtime/ABI or semantic parity proof.
- Compiling untrusted code should be separated from execution and terminated at Worker level on cancellation.
- The existing direct binary encoder in `src/wasm` and native LLVM ORC JIT remain unchanged.

Run the isolated fake-tool tests with `cd web/llvm-jit && bun test`. These verify protocol and lifecycle, **not** actual LLVM tool binaries. See [docs/wasm.md](../../docs/wasm.md) for the existing xyo runtime and JIT architecture.
