#!/bin/sh
# Rebuild ext.wasm from src/ext.c. Requires clang with the wasm32 target and
# wasm-ld (both are part of the usual LLVM install).
set -eu
cd "$(dirname "$0")"
clang --target=wasm32 -O2 -nostdlib -Wl,--no-entry \
  -Wl,--export=xyo_ext_abi \
  -Wl,--export=xyo_ext_alloc \
  -Wl,--export=xyo_ext_call \
  -o ext.wasm src/ext.c
