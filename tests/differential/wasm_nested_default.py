#!/usr/bin/env python3
"""Default-on nested warp-loop compilation (and its explicit opt-out).

Nested numeric loops inside an active warp frame are correctness-validated
and compiled by default now. This locks in both sides:
  - default (XYO_NESTED_WASM unset): the nested loop executes in WASM and the
    final values match LLVM/Interpreter/Builtin/Binaryen;
  - XYO_NESTED_WASM=0: no nested cache is created and the values still match.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "bench" / "algos"))
sys.path.insert(0, str(ROOT / "tests" / "differential"))
from gen_algo_bench import (  # noqa: E402
    Gen, S_set, S_change, S_repeat, S_call, num, var, make_project, write_sb3,
)
from wasm_hotspot_integration import run, run_native  # noqa: E402

STATS = re.compile(r"warp_runs=(\d+)")


def fixture(path: Path) -> None:
    g = Gen(["x", "y"], [])
    # Large enough that the interpreter reaches the nested-JIT promotion
    # sample after the whole-script fuel bail; a tiny loop can finish first.
    g.define("work %s", ["n"], [
        S_repeat(num(200), [
            S_repeat(num(500), [S_change("x", num(1))]),
        ]),
    ], warp=True)
    g.script([S_set("x", num(0)), S_call("work %s", [num(1)]),
              S_set("y", var("x"))])
    write_sb3(str(path), make_project(["x", "y"], [], g))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("wasm", type=Path)
    parser.add_argument("--llvm", required=True, type=Path)
    parser.add_argument("--addon", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    sb3 = args.out / "nested-default.sb3"
    fixture(sb3)
    addon = str(args.addon.resolve())

    native = run_native(args.llvm, sb3, "hotspot")
    interpreter, _ = run("node", ROOT / "web/xyo-node.mjs", args.wasm,
                         sb3, "off")
    builtin, blog = run("node", ROOT / "web/xyo-node.mjs", args.wasm,
                        sb3, "hotspot",
                        initial={"XYO_WASM_HOT_EMITTER": "builtin",
                                 "XYO_NESTED_WASM_STATS": "1"})
    binaryen, clog = run(
        "node", ROOT / "web/xyo-node.mjs", args.wasm, sb3, "hotspot",
        initial={"XYO_WASM_HOT_EMITTER": "binaryen",
                 "XYO_BINARYEN_NODE_ADDON": addon,
                 "XYO_NESTED_WASM_STATS": "1"})
    if len({native, interpreter, builtin, binaryen}) != 1:
        raise AssertionError(
            f"default nested: LLVM={native}\ninterp={interpreter}\n"
            f"builtin={builtin}\nbinaryen={binaryen}")
    match = STATS.search(blog)
    if not match or int(match.group(1)) == 0:
        raise AssertionError(
            "default run did not execute the nested warp loop in WASM: "
            + blog[-1500:])
    if STATS.search(clog) and int(STATS.search(clog).group(1)) == 0:
        raise AssertionError("binaryen run did not execute nested WASM")
    print(f"PASS default: nested warp runs={match.group(1)}; {builtin}")

    off_b, off_log = run("node", ROOT / "web/xyo-node.mjs", args.wasm,
                         sb3, "hotspot",
                         initial={"XYO_WASM_HOT_EMITTER": "builtin",
                                  "XYO_NESTED_WASM": "0",
                                  "XYO_NESTED_WASM_STATS": "1"})
    if "warp_runs=" in off_log:
        raise AssertionError(
            "XYO_NESTED_WASM=0 still created a nested cache: "
            + off_log[-1200:])
    if off_b != native:
        raise AssertionError(f"opt-out mismatch: {off_b} != {native}")
    print(f"PASS opt-out: XYO_NESTED_WASM=0 keeps interpreter parity; {off_b}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
