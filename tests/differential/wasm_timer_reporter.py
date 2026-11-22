#!/usr/bin/env python3
"""`sensing_timer` in compiled WASM regions.

The project timer is a pure runtime read. Compiled modules receive a
per-step snapshot through host import 27, so a deterministic comparison
(`timer > -1`) must produce identical values in native LLVM, the native
interpreter and both WASM emitters.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "bench" / "algos"))
sys.path.insert(0, str(ROOT / "tests" / "differential"))
from gen_algo_bench import (  # noqa: E402
    Gen, S_set, S_change, S_if, S_until, num, op, gt,
    make_project, write_sb3,
)
from wasm_hotspot_integration import run, run_native  # noqa: E402


def timer():
    return op("sensing_timer")


def fixture(path: Path, kind: str) -> None:
    g = Gen(["x", "y"], [])
    if kind == "timer_true":
        g.script([S_set("x", num(0)),
                  S_if(gt(timer(), num(-1)),
                       [S_set("x", num(42))])])
    elif kind == "timer_false":
        g.script([S_set("x", num(0)),
                  S_if(gt(timer(), num(100000)),
                       [S_set("x", num(42))])])
    elif kind == "timer_until":
        g.script([S_set("y", num(0)),
                  S_until(gt(timer(), num(-1)),
                         [S_change("y", num(1))])])
    elif kind == "timer_arith":
        # 7 + timer * 0 == 7 regardless of the clock value.
        g.script([S_set("x", op("operator_add", NUM1=num(7),
                                NUM2=op("operator_multiply",
                                        NUM1=timer(), NUM2=num(0))))])
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
    for kind in ("timer_true", "timer_false", "timer_until", "timer_arith"):
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
        if "compiled script=" not in blog and "hybrid script=" not in blog:
            raise AssertionError(
                f"{kind}: timer condition should compile: {blog[-1400:]}")
        print(f"PASS {kind}: {builtin}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
