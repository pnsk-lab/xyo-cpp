#!/usr/bin/env python3
"""Hybrid WASM HotSpot: exact interpreter handoff with real Binaryen optimization.

A complete Scratch script may alternate supported and unsupported blocks.
The tests check that neither earlier speculative writes nor scheduler yields
are replayed or elided when crossing these block boundaries.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
sys.path.insert(0, str(ROOT / "tests/differential"))
from gen_algo_bench import (
    Gen, S_set, S_change, S_call, S_repeat, num, var, text, arg,
    add, eq, make_project, write_sb3,
)
from wasm_hotspot_integration import run, run_native


def fixture(directory: Path, kind: str) -> Path:
    g = Gen(["x", "y", "z", "flag"], [])
    if kind == "nested_procedure":
        g.define("work %s", ["n"], [
            S_repeat(arg("n"), [S_change("x", num(1))]),
            S_change("y", num(5)),
        ], warp=False)
        g.script([
            S_set("x", num(2)),
            S_call("work %s", [num(3)]),
            S_set("z", add(var("x"), var("y"))),
            S_repeat(num(2), [S_change("x", num(2))]),
            S_set("y", add(var("z"), var("x"))),
        ])
    elif kind == "wait_bridge":
        hat = g.script([
            S_set("x", num(1)),
            S_set("y", add(var("x"), num(2))),
            S_set("z", add(var("y"), num(3))),
        ])
        first = g.blocks[hat]["next"]
        after = g.blocks[first]["next"]
        wait = g.new("control_wait", parent=first)
        g.blocks[wait]["inputs"]["DURATION"] = g.emit_value(num(0), wait)
        g.blocks[first]["next"] = wait
        g.blocks[wait]["next"] = after
        g.blocks[after]["parent"] = wait
    elif kind == "string_comparison":
        g.script([
            S_set("x", text("cat")),
            S_set("y", text("dog")),
            S_set("flag", num(0)),
            S_set("z", add(var("flag"), num(5))),
        ])
        setter = next((k for k, b in g.blocks.items()
                       if b["opcode"] == "data_setvariableto"
                       and b["fields"]["VARIABLE"][1] == g.vars["flag"]),
                      None)
        assert setter
        g.blocks[setter]["inputs"]["VALUE"] = g.emit_value(
            eq(var("x"), var("y")), setter, bool_slot=True)
    elif kind == "boolean_copy":
        g.script([
            S_set("x", num(0)),
            S_set("y", num(0)),
            S_set("x", var("y")),
            S_set("z", add(var("x"), num(5))),
        ])
        setter = next((k for k, b in g.blocks.items()
                       if b["opcode"] == "data_setvariableto"
                       and b["fields"]["VARIABLE"][1] == g.vars["y"]),
                      None)
        assert setter
        g.blocks[setter]["inputs"]["VALUE"] = g.emit_value(
            eq(num(3), num(4)), setter, bool_slot=True)
    else:
        raise ValueError(kind)
    project = make_project(["x", "y", "z", "flag"], [], g)
    path = directory / (kind + ".sb3")
    write_sb3(str(path), project)
    return path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("wasm", type=Path)
    parser.add_argument("--addon", type=Path, required=True)
    parser.add_argument("--llvm", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for kind in ("nested_procedure", "wait_bridge", "string_comparison",
                 "boolean_copy"):
        source = fixture(args.out, kind)
        interp, _ = run("node", ROOT/"web/xyo-node.mjs",
                        args.wasm, source, "off")
        builtin, _ = run("node", ROOT/"web/xyo-node.mjs",
                         args.wasm, source, "hotspot",
                         initial={"XYO_WASM_HOT_EMITTER": "builtin"})
        binaryen, log = run("node", ROOT/"web/xyo-node.mjs",
                            args.wasm, source, "hotspot",
                            initial={
                                "XYO_WASM_HOT_EMITTER": "binaryen",
                                "XYO_BINARYEN_NODE_ADDON":
                                    str(args.addon.resolve())})
        if interp != builtin or builtin != binaryen:
            raise AssertionError(
                f"{kind}: interpreter={interp}\n"
                f"builtin={builtin}\nbinaryen={binaryen}")
        if args.llvm:
            native = run_native(args.llvm, source, "hotspot")
            if native != interp:
                raise AssertionError(
                    f"{kind}: LLVM={native}\ninterpreter={interp}")
        is_hybrid = "xyo-wasm-hot: hybrid script=" in log
        is_whole = "xyo-wasm-hot: compiled script=" in log
        if (not (is_hybrid or is_whole) or
                "emitter=binaryen-host" not in log):
            raise AssertionError(f"{kind}: real Binaryen not invoked")
        if kind in ("nested_procedure", "wait_bridge") and not is_hybrid:
            raise AssertionError(f"{kind}: expected hybrid interpreter bridge")
        if "xyo-wasm-hot: committed" not in log:
            raise AssertionError(f"{kind}: no WASM region was committed")
        print(f"PASS {kind}: all results identical; real hybrid Binaryen "
              "execution, correct scheduler/typed behavior")
        print("  " + binaryen)
    return 0


if __name__ == "__main__":
    sys.exit(main())
