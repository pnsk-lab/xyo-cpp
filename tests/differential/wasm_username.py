#!/usr/bin/env python3
"""Compiled `sensing_username` with a Runtime-rooted tagged string.

Headless runs have an empty username. The host helper keeps the SStr in the
Runtime root provider, so the compiled module may hold the tagged value across
further host calls without a GC hazard. String comparisons go through the
tagged compare import.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "bench" / "algos"))
sys.path.insert(0, str(ROOT / "tests" / "differential"))
from gen_algo_bench import (  # noqa: E402
    Gen, S_set, S_if, S_ifelse, eq, lt, num, op, text, join, make_project,
    write_sb3,
)
from wasm_hotspot_integration import run, run_native  # noqa: E402


def user():
    return op("sensing_username")


def fixture(path: Path, kind: str) -> None:
    g = Gen(["r"], [])
    if kind == "empty_eq":
        g.script([S_set("r", num(0)),
                  S_if(eq(user(), text("")), [S_set("r", num(42))])])
    elif kind == "nonempty_eq":
        g.script([S_set("r", num(0)),
                  S_ifelse(eq(user(), text("abc")),
                           [S_set("r", num(42))], [S_set("r", num(7))])])
    elif kind == "string_lt":
        g.script([S_set("r", num(0)),
                  S_if(lt(user(), text("a")), [S_set("r", num(42))])])
    elif kind == "join_value":
        g.script([S_set("r", join(user(), text("x")))])
    else:
        raise ValueError(kind)
    write_sb3(str(path), make_project(["r"], [], g))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("wasm", type=Path)
    parser.add_argument("--llvm", required=True, type=Path)
    parser.add_argument("--addon", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    addon = str(args.addon.resolve())
    expected = {"empty_eq": '{"r": "42"}',
                "nonempty_eq": '{"r": "7"}',
                "string_lt": '{"r": "42"}',
                "join_value": '{"r": "x"}'}
    for kind, want in expected.items():
        sb3 = args.out / f"{kind}.sb3"
        fixture(sb3, kind)
        native = run_native(args.llvm, sb3, "hotspot")
        interp, _ = run("node", ROOT / "web/xyo-node.mjs", args.wasm,
                        sb3, "off")
        builtin, blog = run("node", ROOT / "web/xyo-node.mjs", args.wasm,
                            sb3, "hotspot",
                            initial={"XYO_WASM_HOT_EMITTER": "builtin",
                                     "XYO_WASM_EFFECT_TRACE": "1"})
        binaryen, _ = run(
            "node", ROOT / "web/xyo-node.mjs", args.wasm, sb3, "hotspot",
            initial={"XYO_WASM_HOT_EMITTER": "binaryen",
                     "XYO_BINARYEN_NODE_ADDON": addon,
                     "XYO_WASM_EFFECT_TRACE": "1"})
        if len({native, interp, builtin, binaryen}) != 1:
            raise AssertionError(
                f"{kind}: LLVM={native} interp={interp} "
                f"builtin={builtin} binaryen={binaryen}")
        if builtin != want:
            raise AssertionError(f"{kind}: expected {want}, got {builtin}")
        if kind != "join_value":
            if "compiled script=" not in blog and "hybrid script=" not in blog:
                raise AssertionError(
                    f"{kind}: username should compile: {blog[-1400:]}")
            if "[wasm-effect] username" not in blog:
                raise AssertionError(
                    f"{kind}: username helper was not reached from WASM: "
                    f"{blog[-1400:]}")
        print(f"PASS {kind}: {builtin}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
