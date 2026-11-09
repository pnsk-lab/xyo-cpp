# cmake/emscripten.cmake — wasm32-emscripten ビルド用 toolchain。
#
# 事前に emsdk を有効化しておく（EMSDK/EMSCRIPTEN 環境変数を設定）:
#   source $HOME/.emsdk/emsdk_env.sh
#
# 使い方（emcmake 推奨 — CMAKE_CROSSCOMPILING_EMULATOR=node も自動設定）:
#   emcmake cmake -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-wasm
#
# このファイルを直接渡す場合（emcmake を使わない場合）は
# CMAKE_CROSSCOMPILING_EMULATOR を node へ設定すること。
if(NOT DEFINED ENV{EMSDK})
  message(FATAL_ERROR "EMSDK が未設定です。先に: source $HOME/.emsdk/emsdk_env.sh")
endif()
include("$ENV{EMSDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")

# Native LLVM ORC cannot run inside WASM. Use the dedicated browser
# WebAssembly HotSpot backend or retain the interpreter-only build.
if(XYO_JIT_BACKEND STREQUAL "wasm")
  set(XYO_JIT ON CACHE BOOL "" FORCE)
else()
  set(XYO_JIT OFF CACHE BOOL "" FORCE)
endif()
