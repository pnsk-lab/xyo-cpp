#!/usr/bin/env python3
"""Long-running LLVM CallFrame loop: no per-callsite dynamic stack growth."""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
sys.path.insert(0, str(ROOT / "tests/differential"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_repeat, S_set,
    add, arg, make_project, num, write_sb3,
)
from native_callframe_mirror import execute


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--calls", type=int, default=20000)
    args = parser.parse_args()
    if not 1 <= args.calls <= 200000:
        raise ValueError("--calls must be in 1..200000")
    args.out.mkdir(parents=True, exist_ok=True)
    gen = Gen(["x", "y"], [])
    gen.define("inner %s", ["a"], [
        S_change("x", arg("a"))
    ], warp=True)
    gen.define("outer", [], [
        S_repeat(num(args.calls), [
            S_call("inner %s", [add(num(1), num(0))])
        ])
    ], warp=True)
    gen.script([S_set("x", num(0)), S_call("outer")])
    project = args.out / "native-callframe-repeated-call.sb3"
    write_sb3(
        str(project),
        make_project(list(gen.vars), list(gen.lists), gen),
    )
    native = args.native.resolve()
    ordinary, _, _ = execute(native, project, "eager", False)
    mirrored, rows, diagnostics = execute(native, project, "eager", True)
    if ordinary != mirrored or '"x": "' + str(args.calls) + '"' not in mirrored:
        raise AssertionError(
            f"loop output mismatch normal={ordinary}, mirrored={mirrored}"
        )
    if not rows:
        raise AssertionError(f"no LLVM frame counters:\n{diagnostics[-800:]}")
    calls = sum(row[0][0] for row in rows)
    returns = sum(row[0][1] for row in rows)
    maximum = max(row[0][3] for row in rows)
    if calls != args.calls + 1 or returns != calls or maximum != 3 or \
       not all(row[0][4] == 1 for row in rows):
        raise AssertionError(
            f"wrong repeated-call frame behavior: {rows}"
        )
    print(
        f"PASS: {args.calls} repetitions plus enclosing procedure; "
        f"calls={calls}, returns={returns}, max_depth={maximum}, "
        f"normal/mirrored outputs identical",
        flush=True,
    )


if __name__ == "__main__":
    main()
