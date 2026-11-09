#!/usr/bin/env python3
"""Real LLVM / interpreter / builtin WASM / Binaryen parity for read-only strings and clock."""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
sys.path.insert(0, str(ROOT / "tests/differential"))
from gen_algo_bench import (Gen, S_set, S_change, num, text, var, op,
                            add, days, round_, make_project, write_sb3)
from wasm_hotspot_integration import run, run_native


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("wasm", type=Path)
    parser.add_argument("--llvm", required=True, type=Path)
    parser.add_argument("--addon", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    def length(argument):
        return op("operator_length", STRING=argument)

    g = Gen(["s", "x", "y", "z", "q"], [])
    g.script([
        S_set("x", length(var("s"))),
        S_change("x", length(var("s"))),
        S_set("y", length(text("日本😀"))),
        S_set("z", length(num(12345))),
        S_set("q", round_(days())),
    ])
    project = make_project(["s", "x", "y", "z", "q"], [], g)
    project["targets"][1]["variables"][g.vars["s"]] = ["s", "Ab😀"]
    sb3 = args.out / "wasm_string_length_days.sb3"
    write_sb3(str(sb3), project)

    baseline = run_native(args.llvm, sb3, "hotspot")
    interp, _ = run("node", ROOT/"web/xyo-node.mjs",
                    args.wasm, sb3, "off")
    builtin, builtin_log = run("node", ROOT/"web/xyo-node.mjs",
                               args.wasm, sb3, "hotspot",
                               initial={"XYO_WASM_HOT_EMITTER": "builtin"})
    binaryen, binaryen_log = run("node", ROOT/"web/xyo-node.mjs",
                                 args.wasm, sb3, "hotspot",
                                 initial={"XYO_WASM_HOT_EMITTER": "binaryen",
                                          "XYO_BINARYEN_NODE_ADDON":
                                              str(args.addon.resolve())})
    if len({baseline, interp, builtin, binaryen}) != 1:
        raise AssertionError(
            f"LLVM={baseline}\ninterp={interp}\nbuiltin={builtin}\n"
            f"binaryen={binaryen}")
    if "emitter=binaryen-host" not in binaryen_log or (
            "xyo-wasm-hot: committed" not in binaryen_log):
        raise AssertionError("missing real Binaryen promotion and commit")
    print("PASS string length + days since 2000: "
          "LLVM / interpreter / builtin / Binaryen identical, "
          "compiled and committed")
    print(binaryen)
    return 0


if __name__ == "__main__":
    sys.exit(main())
