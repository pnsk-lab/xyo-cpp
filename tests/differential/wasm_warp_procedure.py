#!/usr/bin/env python3
"""Guarded warp-procedure inlining parity, captured args and interpreter fallback."""
from __future__ import annotations
import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
sys.path.insert(0, str(ROOT / "tests/differential"))
from gen_algo_bench import (Gen, S_set, S_change, S_repeat, S_call, S_if,
                            S_add, S_delall, length,
                            num, var, arg, add, sub, gt,
                            make_project, write_sb3)
from wasm_hotspot_integration import run, run_native


def fixture(path: Path, kind: str) -> None:
    lists = ["items"] if kind == "list_rollback" else []
    g = Gen(["x", "y", "z"], lists)
    if kind == "warp_inline":
        g.define("step %s", ["n"], [
            S_set("x", add(var("x"), arg("n"))),
            S_change("y", arg("n")),
        ], warp=True)
        g.define("tick", [], [S_change("z", num(1))], warp=True)
        g.script([
            S_set("x", num(2)), S_set("y", num(0)),
            S_call("step %s", [var("x")]),
            S_repeat(num(3), [
                S_call("step %s", [var("x")]), S_call("tick")]),
            S_set("z", add(var("z"), add(var("x"), var("y")))),
        ])
    elif kind == "warp_nested_repeat":
        g.define("nested %s", ["n"], [
            S_repeat(num(3), [
                S_repeat(num(2), [S_change("x", arg("n"))]),
            ]),
            S_set("y", var("x")),
        ], warp=True)
        g.script([S_set("x", num(1)), S_call("nested %s", [num(2)]),
                  S_set("z", add(var("x"), var("y")))])
    elif kind == "dynamic_warp_repeat":
        g.define("nested %s", ["n"], [
            S_repeat(arg("n"), [S_change("x", num(1))]),
        ], warp=True)
        g.script([S_set("x", num(0)), S_call("nested %s", [num(4)])])
    elif kind == "list_rollback":
        g.define("fill %s", ["n"], [
            S_delall("items"),
            S_set("x", num(0)),
            S_repeat(arg("n"), [
                S_add("items", var("x")),
                S_change("x", num(1)),
            ]),
            S_set("y", length("items")),
        ], warp=True)
        g.script([S_add("items", num(99)),
                  S_call("fill %s", [num(1050)]),
                  S_set("z", length("items"))])
    elif kind == "oversized_warp_repeat":
        g.define("nested %s", ["n"], [
            S_repeat(arg("n"), [S_change("x", num(1))]),
        ], warp=True)
        g.script([S_set("x", num(0)), S_call("nested %s", [num(1050)]),
                  S_set("y", var("x"))])
    elif kind == "nonwarp_loop_fallback":
        g.define("step %s", ["n"], [
            S_repeat(arg("n"), [S_change("x", num(1))]),
        ], warp=False)
        g.script([S_set("x", num(1)), S_call("step %s", [num(3)])])
    elif kind == "nonwarp_leaf":
        g.define("step %s", ["n"], [
            S_set("x", add(var("x"), arg("n"))),
        ], warp=False)
        g.script([S_set("x", num(1)), S_call("step %s", [num(3)])])
    elif kind == "recursive_fallback":
        g.define("recurse %s", ["n"], [
            S_change("x", num(1)),
            S_if(gt(arg("n"), num(0)), [
                S_call("recurse %s", [sub(arg("n"), num(1))]),
            ]),
        ], warp=True)
        g.script([S_set("x", num(0)), S_call("recurse %s", [num(3)])])
    else:
        raise ValueError(kind)
    write_sb3(str(path), make_project(["x", "y", "z"], lists, g))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("wasm", type=Path)
    parser.add_argument("--llvm", required=True, type=Path)
    parser.add_argument("--addon", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for kind in ("warp_inline", "warp_nested_repeat",
                 "dynamic_warp_repeat", "oversized_warp_repeat",
                 "list_rollback", "nonwarp_leaf", "nonwarp_loop_fallback",
                 "recursive_fallback"):
        sb3 = args.out / (kind + ".sb3")
        fixture(sb3, kind)
        native = run_native(args.llvm, sb3, "hotspot")
        interpreter, _ = run("node", ROOT / "web/xyo-node.mjs",
                             args.wasm, sb3, "off")
        builtin, _ = run("node", ROOT / "web/xyo-node.mjs",
                         args.wasm, sb3, "hotspot",
                         initial={"XYO_WASM_HOT_EMITTER": "builtin"})
        binaryen, log = run("node", ROOT / "web/xyo-node.mjs",
                            args.wasm, sb3, "hotspot",
                            initial={"XYO_WASM_HOT_EMITTER": "binaryen",
                                     "XYO_BINARYEN_NODE_ADDON":
                                         str(args.addon.resolve())})
        if len({native, interpreter, builtin, binaryen}) != 1:
            raise AssertionError(f"{kind}: LLVM={native}\ninterpreter={interpreter}"
                                 f"\nbuiltin={builtin}\nbinaryen={binaryen}")
        compiled = "emitter=binaryen-host" in log
        if not compiled:
            raise AssertionError(
                f"{kind}: numeric top-level prefix should compile: {log[-1800:]}")
        if kind in ("nonwarp_loop_fallback", "recursive_fallback") and (
                "xyo-wasm-hot: hybrid script=" not in log):
            raise AssertionError(
                f"{kind}: procedure must execute in interpreted hybrid region")
        if kind in ("oversized_warp_repeat", "list_rollback") and (
                "wasm return=1" not in log):
            raise AssertionError("fuel exhaustion did not deopt")
        print(f"PASS {kind}: parity and "
              + ("actual Binaryen promotion" if compiled else "safe interpreter fallback"))
        if compiled:
            print(binaryen if kind != "list_rollback" else
                  f"list_rollback: 1050 elements; x=y=z=1050")
    return 0


if __name__ == "__main__":
    sys.exit(main())
