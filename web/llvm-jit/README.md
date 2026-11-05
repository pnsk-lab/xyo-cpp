# xyo-cpp Browser LLVM JIT

The experimental browser JIT compiles Scratch SB3 scripts using **xyo-cpp's actual C++ `jitEmitScript`**, with both LLVM's `llc` and `lld` running as WebAssembly modules. Emscripten `MAIN_MODULE=2` loads the generated PIC side module into the **same xyo::Runtime** that interprets unsupported scripts.

The browser compiler, LLVM toolchain, and runtime are separate Wasm instances; the generated side module dynamically links into the runtime's Wasm memory.

## Verified (2026-11-05)

- Built LLVM **19.1.7** WebAssembly target libraries using Emscripten 6.0.12.
- Built `xyo_browser_ir.mjs/.wasm` (~11 MiB) containing the real `jitEmitScript`.
- Built browser `llc-browser.mjs/.wasm` (~17 MiB) and `lld-browser.mjs/.wasm` (~21 MiB).
- Built `xyo_browser_unified.mjs/.wasm` (~692 KiB) from the current xyo-cpp tree.
- Node all-Wasm E2E: SB3 → C++ IR emitter → Wasm llc → Wasm LLD → side-module `dlopen` → variable value 42.
- **Chrome headless browser E2E**: the same pipeline runs inside browser Workers and updates the actual xyo runtime variable to `42`. The UI reports the `C++ jitEmitScript` backend.
- Non-admitted scripts fall back to the xyo interpreter without suppressing their execution.

## Build

To regenerate **all** browser artifacts, with a working Emscripten SDK, Ninja, CMake, curl and a native C++ compiler, from repository root:

```bash
source ~/.emsdk/emsdk_env.sh
bash web/llvm-jit/scripts/build-wasm-llvm.sh
```

The helper downloads the LLVM 19.1.7 source release (SHA-256 pinned), builds matching host `llvm-tblgen`, cross-compiles wasm32 LLVM development libraries, builds `xyo_browser_ir`, builds the browser llc/LLD CLI images, then copies the artifacts into `web/llvm-jit/public/llvm-assets`. The underlying build stages were performed and validated individually; the complete helper has not yet been rerun from a clean tree.

Alternatively, when the build outputs already exist:

```bash
cd web/llvm-jit
bun install
bun run copy:host
bun run copy:toolchain
bun run dev
```

The legacy `bun run copy:artifacts -- /path/to/xyo-editor` can still copy xyo-editor's separately built browser LLVM images.

## Tests

```bash
cd web/llvm-jit
bun test
bun run check
bun run build
bun run test:browser
```

`bun run test:browser` requires `playwright-core`, system Chrome at `/usr/bin/google-chrome-stable`, and all compiled browser artifacts. It launches Vite and a headless Chrome instance, imports a real zipped SB3, clicks the UI and asserts the LLVM/C++ backend and state.

`tests/wasm-llvm-e2e.test.ts` tests the full four-component WebAssembly pipeline under Bun/Node. If the generated artifacts are absent, the toolchain integration test is skipped; normal fake-client and native LLVM tests still run.

## Execution model

1. The UI loads an SB3 and creates a dedicated runtime Worker.
2. The Worker loads the Emscripten xyo runtime and optional wasm32 C++ compiler.
3. Admission limits LLVM to a single, short, straight-line green-flag script of numeric set/change blocks. All other programs execute via xyo's interpreter.
4. The C++ compiler emits LLVM IR and binding metadata; the host verifies pointer size, struct layout, NaN tags and slot identities.
5. A nested LLVM Worker runs wasm32 `llc` and `lld` and creates a PIC side module.
6. The interpreter loads the project with the admitted script body removed. The side module is loaded with `dlopen`, its slots are bound to the live runtime, and its entry is invoked on green flag.
7. The snapshot reads the common xyo runtime's values. Workers terminate after the run or timeout.

The older `src/runtime-ir.ts` restricted TypeScript IR emitter remains as a fallback **only when the C++ compiler artifacts are absent**. When C++ compilation itself rejects the script, execution falls back to the interpreter. The UI indicates which IR backend was used.

## Boundaries

- This is not arbitrary-SB3 LLVM JIT yet. Loops, yields, event scheduling, clones and other complex scripts execute via interpreter; the compiled subset does not yield.
- The minimal browser host does not expose the editor's complete rendering, pen, audio and user-input bridges. It currently surfaces variable snapshots.
- Side modules share host linear memory and must be treated as trusted code, **not as a sandbox for malicious modules**. ABI validation helps prevent accidental incompatibilities but is not a security boundary.
- Some performance paths and full Scratch behavioral parity remain untested. No browser benchmark claims are made here.
- Binary artifacts and LLVM build products are intentionally Git-ignored.

Components adapted from [pnsk-lab/xyo-editor](https://github.com/pnsk-lab/xyo-editor/tree/main/packages/llvm-wasm) are distributed under its MIT license; see [THIRD_PARTY_LICENSE](THIRD_PARTY_LICENSE). The xyo-cpp source tree remains the one used to build the runtime and compiler.
