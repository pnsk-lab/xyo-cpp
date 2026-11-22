# wasm3 (vendored)

- Upstream: https://github.com/wasm3/wasm3
- Version: 0.9.2 (main branch snapshot, 2026-11-12)
- License: MIT (see LICENSE)

`source/` contains the core interpreter files only. WASI / uvwasi / tracer
adapters are intentionally not vendored; the extension host links its own
import table instead.

Used by `src/extensions/wasm_provider.cpp` behind the `XYO_WASM_EXT` CMake
option (default ON for native builds, OFF for Emscripten).
