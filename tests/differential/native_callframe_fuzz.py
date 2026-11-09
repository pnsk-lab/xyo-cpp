#!/usr/bin/env python3
"""Seeded end-to-end LLVM JIT CallFrame test, including recursive yields."""
from __future__ import annotations

import argparse
from pathlib import Path
import random
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
sys.path.insert(0, str(ROOT / "tests/differential"))

from gen_algo_bench import (
    Gen, S_call, S_change, S_if, S_repeat, S_set, add, arg, lt,
    make_project, num, write_sb3,
)
from native_callframe_mirror import execute


def fixture(directory: Path, seed: int) -> Path:
    rng = random.Random(seed)
    argc = 1 if seed % 3 else 2
    names = ["a", "b"][:argc]
    code = "recurse " + " ".join("%s" for _ in names)
    warp = (seed % 2 == 0)
    target = rng.randint(2, 5)
    step = rng.randint(-3, 3)
    branch = [add(arg("a"), num(1))]
    initial = [add(num(0), num(0))]
    if argc == 2:
        branch.append(add(arg("b"), num(2)))
        initial.append(add(num(1), num(2)))
    body = [
        S_change("x", arg("a")),
        S_if(lt(arg("a"), num(target)), [S_call(code, branch)]),
        S_change("y", arg("a")),
        S_change("z", num(step)),
    ]
    if argc == 2:
        body.append(S_change("z", arg("b")))
    gen = Gen(["x", "y", "z"], [])
    gen.define(code, names, body, warp=warp)
    gen.script([
        S_set("x", num(0)), S_set("y", num(0)), S_set("z", num(0)),
        S_repeat(num(rng.randint(3, 8)), [
            S_call(code, initial)
        ]),
    ])
    directory.mkdir(parents=True, exist_ok=True)
    out = directory / f"native-callframe-fuzz-{seed:03}.sb3"
    write_sb3(str(out), make_project(list(gen.vars), list(gen.lists), gen))
    return out


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--seeds", type=int, default=16)
    args = parser.parse_args()
    native = args.native.resolve()
    for seed in range(args.seeds):
        project = fixture(args.out, seed)
        interpreter, _, _ = execute(native, project, "off", False)
        regular, _, _ = execute(native, project, "eager", False)
        portable, stats, stderr = execute(native, project, "eager", True)
        if len({interpreter, regular, portable}) != 1:
            raise AssertionError(
                f"seed={seed}: output mismatch: "
                f"{interpreter} vs {regular} vs {portable}"
            )
        calls = sum(row[0][0] for row in stats)
        returns = sum(row[0][1] for row in stats)
        yields = sum(row[0][2] for row in stats)
        depth = max((row[0][3] for row in stats), default=0)
        if calls < 3 or calls != returns or depth < 3 or \
           not all(row[0][4] == 1 for row in stats):
            raise AssertionError(f"seed={seed}: invalid call stack: {stats}\n{stderr[-1200:]}")
        if seed % 2 and yields < 1:
            raise AssertionError(f"seed={seed}: missing native recursive yield: {stats}")
        print(
            f"PASS {seed:02d}: warp={seed%2==0} argc={1 if seed%3 else 2} "
            f"calls={calls} depth={depth} yields={yields}", flush=True
        )
    print(f"PASS {args.seeds} seeded LLVM CallFrame recursive programs")


if __name__ == "__main__":
    main()
