#!/usr/bin/env python3
"""Compare the compiled procedure CallFrame ABI on native C++ and WASM.

Both executables must be built from tests/platform/call_frame_abi_smoke.cpp:
  cmake --build build-list-epoch --target xyo-call-frame-abi-smoke
  source /home/kicky/.emsdk/emsdk_env.sh
  em++ -std=c++20 -O2 -DNDEBUG tests/platform/call_frame_abi_smoke.cpp \
      -sENVIRONMENT=node -sEXIT_RUNTIME=1 -o scratch-bench/results/agent-board-callframe-wasm.cjs
  python3 tests/differential/call_frame_abi_parity.py \
      build-list-epoch/tests/xyo-call-frame-abi-smoke \
      scratch-bench/results/agent-board-callframe-wasm.cjs
"""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess


def run(executable: Path, *, wasm: bool) -> str:
    argv = (["node", str(executable)] if wasm else [str(executable)])
    proc = subprocess.run(
        argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, timeout=30, check=True,
    )
    if not proc.stdout.startswith("PASS call-frame ABI:") or \
       "frame_digest=" not in proc.stdout:
        raise AssertionError(f"unexpected output from {executable}: {proc.stdout}")
    return proc.stdout


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("native", type=Path)
    parser.add_argument("wasm", type=Path)
    args = parser.parse_args()
    native, wasm = args.native.resolve(), args.wasm.resolve()
    if not native.is_file() or not wasm.is_file():
        raise FileNotFoundError(f"missing binary: {native} or {wasm}")
    left = run(native, wasm=False)
    right = run(wasm, wasm=True)
    if left != right:
        raise AssertionError(
            f"CallFrame ABI differs on native and WASM:\n"
            f"native={left}\nwasm={right}"
        )
    print(left, end="")
    print("PASS: byte-identical frame digest on native C++ and Emscripten WASM")


if __name__ == "__main__":
    main()
