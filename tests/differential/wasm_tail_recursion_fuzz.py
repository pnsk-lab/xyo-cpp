#!/usr/bin/env python3
"""Randomized numeric-parameter warp-tail recursion parity and actual WASM execution."""
from __future__ import annotations

import argparse
from pathlib import Path
import random
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
sys.path.insert(0, str(ROOT / "tests/differential"))

from gen_algo_bench import (
    Gen, S_call, S_change, S_if, S_repeat, S_set, add, arg,
    lt, num, var, make_project, write_sb3,
)
from wasm_nested_continuation import run


def make_case(folder: Path, seed: int) -> Path:
    rng = random.Random(seed)
    count = seed % 5  # Exercise zero, one, two, three and four parameters.
    formal = list("abcd"[:count])
    code = "leaf" + (" " + " ".join("%s" for _ in formal) if formal else "")
    depth = rng.randint(2, 26)
    body = [
        S_change("i", num(1)),
        S_change("x", num(rng.choice((1, 2, 3, -1)))),
        S_change("y", num(rng.choice((1, 2, 4, -3)))),
    ]
    if rng.choice((False, True)):
        body.append(S_repeat(num(2), [S_change("z", num(1))]))
    next_args = [
        add(arg(name), num(rng.choice((-2, -1, 1, 2, 4))))
        for name in formal
    ]
    body.append(S_if(
        lt(var("i"), num(depth)),
        [S_call(code, next_args)],
    ))
    gen = Gen(["x", "y", "z", "i"], [])
    gen.define(code, formal, body, warp=True)
    initial = [num(rng.randint(-4, 4)) for _ in formal]
    gen.define("worker", [], [
        S_set("x", num(0)),
        S_set("y", num(0)),
        S_set("z", num(0)),
        S_set("i", num(0)),
        S_repeat(num(1), [S_call(code, initial)]),
    ], warp=True)
    hat = gen.script([S_repeat(num(82), [S_call("worker")])])
    first = gen.blocks[hat]["next"]
    wait = gen.new("control_wait", parent=hat)
    gen.blocks[wait]["inputs"]["DURATION"] = gen.emit_value(num(0), wait)
    gen.blocks[hat]["next"] = wait
    gen.blocks[wait]["next"] = first
    gen.blocks[first]["parent"] = wait
    folder.mkdir(parents=True, exist_ok=True)
    sb3 = folder / f"tail_{seed:04d}.sb3"
    write_sb3(str(sb3), make_project(list(gen.vars), list(gen.lists), gen))
    return sb3


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("wasm", type=Path)
    parser.add_argument("--addon", type=Path, required=True)
    parser.add_argument("--llvm", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--seeds", type=int, default=32)
    args = parser.parse_args()
    for seed in range(args.seeds):
        sb3 = make_case(args.out, seed)
        reference, _ = run(args.wasm, sb3, "off")
        without_nested, _ = run(
            args.wasm, sb3, "hotspot", addon=args.addon, nested=False
        )
        builtin, builtin_diag = run(args.wasm, sb3, "hotspot")
        binaryen, binaryen_diag = run(
            args.wasm, sb3, "hotspot", addon=args.addon
        )
        llvm, _ = run(args.llvm, sb3, "hotspot", native=True)
        if len({reference, without_nested, builtin, binaryen, llvm}) != 1:
            raise AssertionError(
                f"seed {seed}: cross-backend tail recursion divergence\n"
                f"interpreter={reference}\n"
                f"disabled={without_nested}\n"
                f"builtin={builtin}\n"
                f"binaryen={binaryen}\n"
                f"llvm={llvm}\n"
            )
        for name, diagnostics in (
            ("builtin", builtin_diag), ("binaryen", binaryen_diag)
        ):
            match = re.search(
                r"tail_compiled=(\d+) tail_runs=(\d+) tail_bails=(\d+)",
                diagnostics,
            )
            if not match:
                raise AssertionError(f"seed {seed} {name}: stats absent")
            compiled, executed, bailed = map(int, match.groups())
            if not (compiled >= 1 and executed > 5 and bailed == 0):
                raise AssertionError(
                    f"seed {seed} {name}: expected real tail recursion "
                    f"module execution, got {match.group(0)}"
                )
        print(
            f"PASS seed={seed:03d} argc={seed % 5} "
            "5-way parity, actual builtin/Binaryen tail recursion",
            flush=True,
        )
    print(f"PASS: {args.seeds} numeric tail-recursion seeds", flush=True)


if __name__ == "__main__":
    main()
