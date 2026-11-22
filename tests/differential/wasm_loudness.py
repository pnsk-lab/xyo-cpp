#!/usr/bin/env python3
"""Compiled `sensing_loudness` (pure numeric runtime read).

Headless runs have no audio engine, so the interpreter and compiled paths
must both return -1. The compiled path shares the interpreter's cached
device-read logic through the step-scoped Runtime pointer.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "bench" / "algos"))
sys.path.insert(0, str(ROOT / "tests" / "differential"))
from gen_algo_bench import (  # noqa: E402
    Gen, S_set, S_change, S_if, S_until, add, eq, num, op, make_project,
    write_sb3,
)
from wasm_hotspot_integration import run, run_native  # noqa: E402


def loudness():
    return op("sensing_loudness")


def fixture(path: Path, kind: str) -> None:
    g = Gen(["x", "y"], [])
    if kind == "loud_set":
        g.script([S_set("x", loudness())])
    elif kind == "loud_arith":
        g.script([S_set("x", add(loudness(), num(2)))])
    elif kind == "loud_cond":
        g.script([S_if(op("operator_lt", OPERAND1=loudness(),
                          OPERAND2=num(0)),
                       [S_set("x", num(42))])])
    elif kind == "loud_until":
        # loudness (-1) < 0 is immediately true: zero iterations.
        g.script([S_set("y", num(0)),
                  S_until(op("operator_lt", OPERAND1=loudness(),
                             OPERAND2=num(0)),
                          [S_change("y", num(1))])])
    else:
        raise ValueError(kind)
    write_sb3(str(path), make_project(["x", "y"], [], g))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("wasm", type=Path)
    parser.add_argument("--llvm", required=True, type=Path)
    parser.add_argument("--addon", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    addon = str(args.addon.resolve())
    expected = {"loud_set": '{"x": "-1", "y": "0"}',
                "loud_arith": '{"x": "1", "y": "0"}',
                "loud_cond": '{"x": "42", "y": "0"}',
                "loud_until": '{"x": "0", "y": "0"}'}
    for kind, want in expected.items():
        sb3 = args.out / f"{kind}.sb3"
        fixture(sb3, kind)
        native = run_native(args.llvm, sb3, "hotspot")
        interp, _ = run("node", ROOT / "web/xyo-node.mjs", args.wasm,
                        sb3, "off")
        builtin, blog = run("node", ROOT / "web/xyo-node.mjs", args.wasm,
                            sb3, "hotspot",
                            initial={"XYO_WASM_HOT_EMITTER": "builtin"})
        binaryen, _ = run(
            "node", ROOT / "web/xyo-node.mjs", args.wasm, sb3, "hotspot",
            initial={"XYO_WASM_HOT_EMITTER": "binaryen",
                     "XYO_BINARYEN_NODE_ADDON": addon})
        if len({native, interp, builtin, binaryen}) != 1:
            raise AssertionError(
                f"{kind}: LLVM={native} interp={interp} "
                f"builtin={builtin} binaryen={binaryen}")
        if builtin != want:
            raise AssertionError(f"{kind}: expected {want}, got {builtin}")
        if "compiled script=" not in blog and "hybrid script=" not in blog:
            raise AssertionError(
                f"{kind}: loudness should compile: {blog[-1400:]}")
        print(f"PASS {kind}: {builtin}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
