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
# it was configured with the same source tree, Emscripten SDK, emulator, and
# CMake generator. setup-emsdk may extract the SDK to a fresh path on every
# run, and the cache fallback may restore a tree made before Ninja was used.
# Reusing either kind of stale CMakeCache.txt makes configuration fail before
# the actual CMake error is useful, so detect the mismatches and start over.
cmake_cache="${LLVM_BUILD_DIR}/CMakeCache.txt"
if [[ -f "${cmake_cache}" ]]; then
    emscripten_root="$(cd "$(dirname "$(command -v emcmake)")" && pwd)"
    current_toolchain="${emscripten_root}/cmake/Modules/Platform/Emscripten.cmake"
    expected_source="$(cd "${LLVM_SOURCE_DIR}/llvm" && pwd)"
    expected_generator="${LLVM_CMAKE_GENERATOR:-}"
    # em-config prints NODE_JS as a Python-style list on some emsdk
    # versions (for example: ['.../node']); CMakeCache.txt stores only the
    # path, so normalize both forms before comparing them.
    current_emulator="$(em-config NODE_JS 2>/dev/null | sed -e "s/^\['//" -e "s/'\]$//" || true)"

    cached_toolchain="$(sed -n 's/^CMAKE_TOOLCHAIN_FILE:[A-Za-z]*=//p' "${cmake_cache}" | head -n 1)"
    cached_source="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${cmake_cache}" | head -n 1)"
    cached_generator="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "${cmake_cache}" | head -n 1)"
    cached_emulator="$(sed -n 's/^CMAKE_CROSSCOMPILING_EMULATOR:[A-Za-z]*=//p' "${cmake_cache}" | head -n 1)"

    cache_mismatch=""
    if [[ "${cached_toolchain}" != "${current_toolchain}" ]]; then
        cache_mismatch="Emscripten toolchain"
    elif [[ "${cached_source}" != "${expected_source}" ]]; then
        cache_mismatch="LLVM source directory"
    elif [[ -n "${expected_generator}" && "${cached_generator}" != "${expected_generator}" ]]; then
        cache_mismatch="CMake generator"
    elif [[ -n "${current_emulator}" && "${cached_emulator}" != "${current_emulator}" ]]; then
        cache_mismatch="CMake cross-compiling emulator"
    fi

    if [[ -n "${cache_mismatch}" ]]; then
        echo "LLVM build cache is stale (${cache_mismatch})." >&2
        echo "  cached toolchain: ${cached_toolchain:-<unknown>}" >&2
        echo "  current toolchain: ${current_toolchain}" >&2
        echo "  cached source: ${cached_source:-<unknown>}" >&2
        echo "  current source: ${expected_source}" >&2
        echo "  cached generator: ${cached_generator:-<unknown>}" >&2
        echo "  current generator: ${expected_generator:-<default>}" >&2
        echo "  cached emulator: ${cached_emulator:-<unknown>}" >&2
        echo "  current emulator: ${current_emulator:-<default>}" >&2
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
