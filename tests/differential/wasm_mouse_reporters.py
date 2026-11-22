#!/usr/bin/env python3
"""Compiled `sensing_mousex` / `sensing_mousey` / `sensing_mousedown`.

The mouse state lives in the Runtime; compiled modules read it through the
step-scoped runtime pointer published in Runtime::step (host imports 28-30).
Headless runs have no mouse input, so x/y are 0 and the button is up.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "bench" / "algos"))
sys.path.insert(0, str(ROOT / "tests" / "differential"))
from gen_algo_bench import (  # noqa: E402
    Gen, S_set, S_if, S_ifelse, add, eq, num, op, make_project, write_sb3,
)
from wasm_hotspot_integration import run, run_native  # noqa: E402


def mouse_x():
    return op("sensing_mousex")


def mouse_y():
    return op("sensing_mousey")


def mouse_down_value():
    return op("sensing_mousedown")


def mouse_down():
    # Scratch's boolean block is a value reporter in the catalog; compare
    # it to 1 to obtain a hexagonal condition the generator accepts.
    return eq(mouse_down_value(), num(1))


def fixture(path: Path, kind: str) -> None:
    g = Gen(["x", "y"], [])
    if kind == "mouse_xy":
        g.script([S_set("x", mouse_x()), S_set("y", mouse_y())])
    elif kind == "mouse_arith":
        g.script([S_set("x", add(mouse_x(), num(7)))])
    elif kind == "mouse_down":
        g.script([S_ifelse(mouse_down(),
                           [S_set("x", num(1))], [S_set("x", num(2))])])
    elif kind == "mouse_not":
        g.script([S_if(op("operator_not", OPERAND=mouse_down()),
                       [S_set("x", num(42))])])
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
    expected = {"mouse_xy": '{"x": "0", "y": "0"}',
                "mouse_arith": '{"x": "7", "y": "0"}',
                "mouse_down": '{"x": "2", "y": "0"}',
                "mouse_not": '{"x": "42", "y": "0"}'}
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
                f"{kind}: mouse reporter should compile: {blog[-1400:]}")
        print(f"PASS {kind}: {builtin}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
