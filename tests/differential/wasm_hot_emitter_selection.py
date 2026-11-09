#!/usr/bin/env python3
"""E2E selection/fallback contract for the Emscripten Binaryen backend.

Exercise *executed* shared-CFG WASM, not just module emission.
The optimizer is deliberately optional: an explicit missing Binaryen must
fall back to Scratch without silently compiling an unoptimized module.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from common_cfg_backend_parity import create_fixture

ROOT = Path(__file__).resolve().parents[2]


def run(wasm: Path, project: Path, mode: str, addon: Path | None):
    env = os.environ.copy()
    env.update(
        XYO_JIT_COMMON_CFG="1",
        XYO_JIT_VERBOSE="1",
        XYO_HOT_INVOCATIONS="1",
        XYO_EXIT_ON_DONE="1",
        XYO_PAR="0",
        XYO_DUMPVARS="json",
        XYO_BINARYEN_DIAGNOSTICS="1",
        XYO_WASM_HOT_EMITTER=mode,
    )
    env.pop("XYO_BINARYEN_JS", None)
    env.pop("XYO_BINARYEN_NODE_ADDON", None)
    if addon is not None:
        env["XYO_BINARYEN_NODE_ADDON"] = str(addon)
    proc = subprocess.run(
        ["node", str(ROOT / "web/xyo-node.mjs"), str(wasm), "run",
         str(project), "--jit", "hotspot", "--fps", "30", "--frames", "50"],
        cwd=ROOT, env=env, text=True, capture_output=True, timeout=120,
    )
    if proc.returncode:
        raise AssertionError(f"{mode} failed:\n{proc.stderr[-1800:]}")
    try:
        state, _ = json.JSONDecoder().raw_decode(
            proc.stdout[proc.stdout.index("{"):])
    except (ValueError, IndexError) as exc:
        raise AssertionError(f"{mode}: no state: {proc.stdout}") from exc
    if float(state["Bench"]["sum"]) != 21.0:
        raise AssertionError(f"{mode}: wrong state {state}")
    return proc.stderr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--wasm", required=True, type=Path)
    parser.add_argument("--addon", required=True, type=Path)
    args = parser.parse_args()
    wasm, addon = args.wasm.resolve(), args.addon.resolve()
    if not wasm.is_file() or not addon.is_file():
        parser.error("compiled Emscripten xyo and Binaryen addon required")
    with tempfile.TemporaryDirectory(dir=wasm.parent) as tmp:
        fixture = Path(tmp) / "binaryen-emitter.sb3"
        create_fixture(fixture, 7, "3", False)
        cases = [
            ("builtin", addon, "builtin", False),
            ("auto", addon, "binaryen", True),
            ("binaryen", addon, "binaryen", True),
            ("auto", None, "builtin", False),
            ("binaryen", None, None, False),
            ("invalid", addon, None, False),
        ]
        for mode, extension, expected, optimized in cases:
            log = run(wasm, fixture, mode, extension)
            compiled = [line for line in log.splitlines()
                        if "compiled shared CFG" in line]
            if expected is None:
                assert not compiled, (mode, compiled)
            else:
                assert len(compiled) == 1 and f"emitter={expected}" in compiled[0], (
                    mode, compiled)
            called = "[binaryen-native] optimized #" in log
            assert called == optimized, (mode, log[-1400:])
            print(f"PASS: {mode}, addon={extension is not None}, "
                  f"compiled={expected or 'fallback'}, optimized={called}")
    print(f"PASS: {len(cases)} Binaryen backend-selection cases")


if __name__ == "__main__":
    main()
