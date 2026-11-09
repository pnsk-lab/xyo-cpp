#!/usr/bin/env python3
"""End-to-end list reporter/edit parity using real LLVM and Binaryen HotSpot.

The first case checks successful staging and Binaryen compilation. The
second deliberately triggers a late guard failure after speculative writes;
the interpreter must observe no speculative side effect.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
sys.path.insert(0, str(ROOT / "tests/differential"))
from gen_algo_bench import (Gen, S_set, S_change, S_add, S_del, S_delall,
                            S_replace, S_if, num, var, add, item, length,
                            op, make_project, write_sb3)
from wasm_hotspot_integration import run, run_native


def generate(path: Path, kind: str) -> None:
    gen = Gen(["x", "y", "z", "q"], ["items"])
    contains = lambda v: op("data_listcontainsitem", {"LIST": "items"}, ITEM=num(v))
    indexof = lambda v: op("data_itemnumoflist", {"LIST": "items"}, ITEM=num(v))
    if kind == "numeric":
        steps = [
            S_delall("items"), S_add("items", num(5)), S_add("items", num(8)),
            S_set("x", length("items")),
            S_set("y", indexof(8)),
            S_if(contains(5), [S_change("z", num(1))]),
            # The generator supports replace but not insert. Mark this block
            # as insert after generating the graph (the inputs are identical).
            S_replace("items", num(2), num(11)),
            S_del("items", num(1)),
            S_set("q", length("items")),
            S_set("x", add(var("x"), item("items", num(1)))),
            S_replace("items", num(1), num(9)),
            S_set("q", add(var("q"), item("items", num(1)))),
            S_del("items", num(2)),
            S_set("y", add(var("y"), indexof(999))),
            S_delall("items"),
            S_set("z", add(var("z"), length("items"))),
        ]
    elif kind == "late_bail":
        steps = [
            S_delall("items"), S_add("items", num(4)),
            S_set("x", length("items")),
            # Invalid indices deopt rather than silently corrupt a list;
            # interpreter is responsible for the exact no-op behavior.
            S_del("items", num(999)),
            S_set("y", length("items")),
            S_set("z", item("items", num(1))),
        ]
    else:
        raise ValueError(kind)
    gen.script(steps)
    if kind == "numeric":
        candidates = [b for b in gen.blocks.values()
                      if b["opcode"] == "data_replaceitemoflist" and
                      b["inputs"]["ITEM"][1][1] == "11"]
        if len(candidates) != 1:
            raise AssertionError("expected one insertion marker")
        candidates[0]["opcode"] = "data_insertatlist"
    project = make_project(["x", "y", "z", "q"], ["items"], gen)
    write_sb3(str(path), project)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("wasm", type=Path)
    parser.add_argument("--llvm", type=Path, required=True)
    parser.add_argument("--addon", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for kind in ("numeric", "late_bail"):
        sb3 = args.out / ("list_" + kind + ".sb3")
        generate(sb3, kind)
        baseline = run_native(args.llvm, sb3, "hotspot")
        interp, _ = run("node", ROOT / "web/xyo-node.mjs",
                        args.wasm, sb3, "off")
        builtin, builtin_log = run("node", ROOT / "web/xyo-node.mjs",
                                   args.wasm, sb3, "hotspot",
                                   initial={"XYO_WASM_HOT_EMITTER": "builtin"})
        binaryen, binaryen_log = run("node", ROOT / "web/xyo-node.mjs",
                                     args.wasm, sb3, "hotspot",
                                     initial={"XYO_WASM_HOT_EMITTER": "binaryen",
                                              "XYO_BINARYEN_NODE_ADDON":
                                                  str(args.addon.resolve())})
        if baseline != interp or interp != builtin or builtin != binaryen:
            raise AssertionError(
                f"{kind}: LLVM={baseline}\ninterp={interp}\n"
                f"builtin={builtin}\nbinaryen={binaryen}")
        if "emitter=binaryen-host" not in binaryen_log:
            raise AssertionError(f"{kind}: Binaryen optimization not invoked")
        if kind == "late_bail" and "wasm return=1" not in binaryen_log:
            raise AssertionError("late-bail fixture failed to exercise rollback")
        print(f"PASS {kind}: LLVM / interpreter / builtin / Binaryen "
              "values identical; real Binaryen optimizer invoked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
