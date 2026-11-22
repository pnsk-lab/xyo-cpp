#!/usr/bin/env python3
"""Edge-activated hats must fire in headless runs without any other thread.

A project whose only script is `when timer > -1` has no active thread, so the
old headless loop exited before ever calling Runtime::step and the hat never
fired. The loop now keeps stepping while any executable target has an
edge-activated hat script. This compares native LLVM, the native interpreter
and both WASM modes for a firing and a non-firing timer hat.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "bench" / "algos"))
sys.path.insert(0, str(ROOT / "tests" / "differential"))
from gen_algo_bench import Gen, S_set, num, make_project, write_sb3  # noqa: E402
from wasm_hotspot_integration import run, run_native  # noqa: E402


def fixture(path: Path, threshold: float) -> None:
    g = Gen(["x"], [])
    hat = g.new("event_whengreaterthan", top=True,
                fields={"WHENGREATERTHANMENU": ["TIMER", None]})
    g.blocks[hat]["inputs"]["VALUE"] = g.emit_value(num(threshold), hat)
    stmt = g.emit_stmt(S_set("x", num(42)), parent=hat)
    g.blocks[hat]["next"] = stmt
    write_sb3(str(path), make_project(["x"], [], g))


def native(executable: Path, sb3: Path, jit: str) -> str:
    env = dict(os.environ, XYO_DUMPVARS="json", XYO_EXIT_ON_DONE="1",
               XYO_PAR="0", XYO_JIT_WARP_CLOCK="0")
    proc = subprocess.run(
        [str(executable.resolve()), "run", str(sb3), "--frames", "6",
         "--jit", jit], cwd=ROOT, env=env, text=True,
        capture_output=True, timeout=90)
    if proc.returncode:
        raise AssertionError(f"native {jit} exited {proc.returncode}: "
                             + proc.stderr[-900:])
    start = proc.stdout.find("{")
    values = json.JSONDecoder().raw_decode(proc.stdout[start:])[0]
    return json.dumps(values["Bench"], sort_keys=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("wasm", type=Path)
    parser.add_argument("--llvm", required=True, type=Path)
    parser.add_argument("--addon", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    addon = str(args.addon.resolve())
    for label, threshold, expected in (
            ("fires", -1, '{"x": "42"}'),
            ("idle", 999, '{"x": "0"}')):
        sb3 = args.out / f"edge-{label}.sb3"
        fixture(sb3, threshold)
        interp_native = native(args.llvm, sb3, "off")
        jit_native = native(args.llvm, sb3, "hotspot")
        wasm_interp, _ = run("node", ROOT / "web/xyo-node.mjs", args.wasm,
                             sb3, "off")
        builtin, _ = run("node", ROOT / "web/xyo-node.mjs", args.wasm,
                         sb3, "hotspot",
                         initial={"XYO_WASM_HOT_EMITTER": "builtin"})
        binaryen, _ = run(
            "node", ROOT / "web/xyo-node.mjs", args.wasm, sb3, "hotspot",
            initial={"XYO_WASM_HOT_EMITTER": "binaryen",
                     "XYO_BINARYEN_NODE_ADDON": addon})
        values = {interp_native, jit_native, wasm_interp, builtin, binaryen}
        if len(values) != 1:
            raise AssertionError(
                f"{label}: native interp={interp_native} "
                f"native jit={jit_native} wasm interp={wasm_interp} "
                f"builtin={builtin} binaryen={binaryen}")
        if builtin != expected:
            raise AssertionError(
                f"{label}: expected {expected}, got {builtin}")
        print(f"PASS edge hat {label}: all engines {builtin}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
