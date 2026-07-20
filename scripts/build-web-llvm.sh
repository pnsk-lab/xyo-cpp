#!/usr/bin/env bash
set -euo pipefail

: "${LLVM_SOURCE_DIR:?Set LLVM_SOURCE_DIR to an llvm-project checkout}"
: "${LLVM_BUILD_DIR:?Set LLVM_BUILD_DIR to a writable LLVM build directory}"

command -v emcmake >/dev/null 2>&1 || {
    echo "emcmake was not found. Activate an Emscripten SDK first." >&2
    exit 1
}
command -v cmake >/dev/null 2>&1 || {
    echo "cmake was not found." >&2
    exit 1
}

# A cached build tree (e.g. restored by actions/cache) is only reusable when
# it was configured against the same Emscripten SDK. setup-emsdk may extract
# the SDK to a fresh path on every run, leaving the restored CMakeCache.txt
# pointing at a compiler that no longer exists; reconfiguring such a tree
# fails in CMakeDetermineCCompiler. Detect the mismatch and start over.
cmake_cache="${LLVM_BUILD_DIR}/CMakeCache.txt"
if [[ -f "${cmake_cache}" ]]; then
    emscripten_root="$(cd "$(dirname "$(command -v emcmake)")" && pwd)"
    current_toolchain="${emscripten_root}/cmake/Modules/Platform/Emscripten.cmake"
    cached_toolchain="$(sed -n 's/^CMAKE_TOOLCHAIN_FILE:[A-Za-z]*=//p' "${cmake_cache}" | head -n 1)"
    if [[ "${cached_toolchain}" != "${current_toolchain}" ]]; then
        echo "LLVM build tree was configured with a different Emscripten SDK:" >&2
        echo "  cached:  ${cached_toolchain:-<unknown>}" >&2
        echo "  current: ${current_toolchain}" >&2
        echo "Removing ${LLVM_BUILD_DIR} and reconfiguring from scratch." >&2
        rm -rf "${LLVM_BUILD_DIR}"
    fi
fi

configure_args=(
    -S "${LLVM_SOURCE_DIR}/llvm"
    -B "${LLVM_BUILD_DIR}"
    -DCMAKE_BUILD_TYPE=Release
    -DLLVM_ENABLE_PROJECTS=lld
    -DLLVM_TARGETS_TO_BUILD=WebAssembly
    -DLLVM_BUILD_TOOLS=OFF
    -DLLVM_BUILD_UTILS=OFF
    -DLLVM_BUILD_EXAMPLES=OFF
    -DLLVM_INCLUDE_TESTS=OFF
    -DLLVM_INCLUDE_DOCS=OFF
    -DLLVM_INCLUDE_EXAMPLES=OFF
    -DLLVM_INCLUDE_BENCHMARKS=OFF
    -DLLVM_INCLUDE_RUNTIMES=OFF
    -DLLVM_BUILD_RUNTIME=OFF
    -DLLVM_BUILD_RUNTIMES=OFF
    -DLLVM_ENABLE_ASSERTIONS=OFF
    -DLLVM_ENABLE_EH=OFF
    -DLLVM_ENABLE_RTTI=ON
    -DLLVM_ENABLE_THREADS=OFF
    -DLLVM_ENABLE_PIC=OFF
    -DLLVM_ENABLE_PLUGINS=OFF
    -DLLVM_ENABLE_ZLIB=OFF
    -DLLVM_ENABLE_TERMINFO=OFF
    -DLLVM_ENABLE_LIBXML2=OFF
    -DLLVM_ENABLE_LTO=OFF
    -DLLVM_TOOL_LTO_BUILD=OFF
    -DLLVM_BUILD_LLVM_DYLIB=OFF
    -DLLVM_BUILD_LLVM_C_DYLIB=OFF
    -DLLD_BUILD_TOOLS=OFF
)

if [[ -n "${LLVM_CMAKE_GENERATOR:-}" ]]; then
    configure_args+=(-G "${LLVM_CMAKE_GENERATOR}")
fi

emcmake cmake "${configure_args[@]}"

build_args=(--build "${LLVM_BUILD_DIR}" --target "${LLVM_BUILD_TARGET:-lldWasm}" --parallel)
if [[ -n "${LLVM_BUILD_PARALLEL:-}" ]]; then
    build_args+=("${LLVM_BUILD_PARALLEL}")
fi
cmake "${build_args[@]}"

compgen -G "${LLVM_BUILD_DIR}/lib/libLLVM*.a" >/dev/null || {
    echo "LLVM static libraries were not produced under ${LLVM_BUILD_DIR}/lib." >&2
    exit 1
}
test -f "${LLVM_BUILD_DIR}/lib/liblldWasm.a" || {
    echo "liblldWasm.a was not produced under ${LLVM_BUILD_DIR}/lib." >&2
    exit 1
}
