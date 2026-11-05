#!/usr/bin/env bash
# Reproducible wasm32 LLVM development-library bootstrap for xyo-browser-ir.
# Based on xyo-editor/packages/llvm-wasm (MIT, 2026 Shotaro Nakamura).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
WORK="${XYO_LLVM_WORK:-$ROOT/web/llvm-jit/native/.llvm-work}"
VERSION="19.1.7"
ARCHIVE="$WORK/llvm-project-$VERSION.src.tar.xz"
SOURCE="$WORK/llvm-project-$VERSION.src/llvm"
URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-$VERSION/llvm-project-$VERSION.src.tar.xz"
SHA256="82401fea7b79d0078043f7598b835284d6650a75b93e64b6f761ea7b63097501"

mkdir -p "$WORK"
if [[ ! -e "$ARCHIVE" ]]; then curl --fail --location --retry 3 "$URL" --output "$ARCHIVE.tmp"; mv "$ARCHIVE.tmp" "$ARCHIVE"; fi
printf '%s  %s\n' "$SHA256" "$ARCHIVE" | sha256sum --check
if [[ ! -f "$SOURCE/CMakeLists.txt" ]]; then
  tar -xf "$ARCHIVE" -C "$WORK"
fi

# LLVM TableGen must be native and must match the headers being generated.
cmake -G Ninja -S "$SOURCE" -B "$WORK/host" \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_TARGETS_TO_BUILD=WebAssembly \
  -DLLVM_ENABLE_PROJECTS="" \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF \
  -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF
cmake --build "$WORK/host" --target llvm-tblgen -j "${XYO_LLVM_JOBS:-4}"

# Use the SDK supplied by the developer; never link host x86_64 LLVM to wasm.
: "${EMSDK:?Source emsdk_env.sh before running this script}"
emcmake cmake -G Ninja -S "$SOURCE" -B "$WORK/wasm" \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DLLVM_TARGET_ARCH=wasm32-unknown-emscripten \
  -DLLVM_TARGETS_TO_BUILD=WebAssembly \
  -DLLVM_DEFAULT_TARGET_TRIPLE=wasm32-unknown-emscripten \
  -DLLVM_TABLEGEN="$WORK/host/bin/llvm-tblgen" \
  -DLLVM_ENABLE_THREADS=OFF -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF \
  -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF \
  -DLLVM_BUILD_TOOLS=OFF -DLLVM_BUILD_UTILS=OFF \
  '-DCMAKE_EXE_LINKER_FLAGS=-sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=web,worker,node'

# Build the libraries used by jitEmitScript, not the entire LLVM tool set.
cmake --build "$WORK/wasm" --target LLVMPasses LLVMWebAssemblyCodeGen \
  -j "${XYO_LLVM_JOBS:-4}"

emcmake cmake -G Ninja -S "$ROOT/web/llvm-jit/native" -B "$ROOT/web/llvm-jit/native/build" \
  -DCMAKE_BUILD_TYPE=Release -DXYO_BROWSER_BUILD_COMPILER=ON \
  -DLLVM_DIR="$WORK/wasm/lib/cmake/llvm"
cmake --build "$ROOT/web/llvm-jit/native/build" --target xyo_browser_ir \
  -j "${XYO_LLVM_JOBS:-4}"

echo "Browser C++ IR compiler: $ROOT/web/llvm-jit/native/build/xyo_browser_ir.mjs"
echo "Next: cd web/llvm-jit && bun run copy:host"

# Build the browser-native LLVM command-line drivers too. They run with MEMFS
# inside dedicated Web Workers (not NODERAWFS or a host filesystem).
emcmake cmake -G Ninja -S "$SOURCE" -B "$WORK/wasm" \
  -DLLVM_BUILD_TOOLS=ON -DLLVM_ENABLE_PROJECTS=lld \
  '-DCMAKE_EXE_LINKER_FLAGS=-sALLOW_MEMORY_GROWTH=1 -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,worker,node -sFORCE_FILESYSTEM=1 -sEXPORTED_RUNTIME_METHODS=FS,callMain -sINVOKE_RUN=0'
cmake --build "$WORK/wasm" --target llc lld -j "${XYO_LLVM_JOBS:-4}"

node "$ROOT/web/llvm-jit/scripts/copy-toolchain.mjs"
node "$ROOT/web/llvm-jit/scripts/copy-host.mjs"
echo 'Browser llvm assets installed. Run: cd web/llvm-jit && bun run test:browser'
