#!/usr/bin/env python3
"""Randomized canonical warp non-tail recursion: verified LIFO return order."""
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
    Gen, S_call, S_change, S_if, S_ifelse, S_repeat, S_set,
    lt, num, var, make_project, write_sb3,
)
from wasm_nested_continuation import run


def make_case(folder: Path, seed: int) -> Path:
    rng = random.Random(seed)
    depth = rng.randint(1, 36)
    prefix = [
        S_change("y", num(1)),
        S_change("x", num(rng.choice((-4, -2, 1, 3)))),
    ]
    if rng.choice((True, False)):
        prefix.append(
            S_repeat(num(rng.randint(1, 4)), [
                S_change("x", num(rng.choice((-3, 1, 2)))),
            ])
        )
    if rng.choice((True, False)):
        prefix.append(
            S_ifelse(lt(var("x"), num(rng.randint(-10, 20))),
                     [S_change("z", num(1))],
                     [S_change("z", num(-2))])
        )
    suffix = [
        S_change("z", num(rng.choice((-4, -1, 2, 3)))),
    ]
    if rng.choice((True, False)):
        suffix.append(S_repeat(num(rng.randint(1, 4)), [
            S_change("x", num(rng.choice((-3, -1, 2)))),
        ]))
    if rng.choice((True, False)):
        suffix.append(
            S_ifelse(lt(var("z"), num(rng.randint(-100, 100))),
                     [S_change("z", num(1))],
                     [S_change("z", num(2))])
        )

    gen = Gen(["x", "y", "z", "i"], [])
    gen.define("leaf", [], [
        *prefix,
        S_if(lt(var("y"), num(depth)), [S_call("leaf")]),
        *suffix,
    ], warp=True)
    gen.define("worker", [], [
        S_set("x", num(0)),
        S_set("y", num(0)),
        S_set("z", num(0)),
        S_repeat(num(1), [S_call("leaf")]),
    ], warp=True)
    hat = gen.script([S_repeat(num(82), [S_call("worker")])])
    first = gen.blocks[hat]["next"]
    wait = gen.new("control_wait", parent=hat)
    gen.blocks[wait]["inputs"]["DURATION"] = gen.emit_value(num(0), wait)
    gen.blocks[hat]["next"] = wait
    gen.blocks[wait]["next"] = first
    gen.blocks[first]["parent"] = wait
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / f"return_{seed:04d}.sb3"
    write_sb3(str(path), make_project(list(gen.vars), list(gen.lists), gen))
    return path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("wasm", type=Path)
    parser.add_argument("--addon", type=Path, required=True)
    parser.add_argument("--llvm", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--seeds", type=int, default=32)
    args = parser.parse_args()

    for seed in range(args.seeds):
        path = make_case(args.out, seed)
        reference, _ = run(args.wasm, path, "off")
        without_nested, _ = run(
            args.wasm, path, "hotspot", addon=args.addon, nested=False
        )
        builtin, builtin_diag = run(args.wasm, path, "hotspot")
        binaryen, binaryen_diag = run(
            args.wasm, path, "hotspot", addon=args.addon
        )
        llvm, _ = run(args.llvm, path, "hotspot", native=True)
        if len({reference, without_nested, builtin, binaryen, llvm}) != 1:
            raise AssertionError(
                f"seed {seed}: return continuation divergence:\n"
                f"interpreter={reference}\nwithout_nested={without_nested}\n"
                f"builtin={builtin}\nbinaryen={binaryen}\nllvm={llvm}"
            )
        for backend, diagnostics in (
            ("builtin", builtin_diag), ("binaryen", binaryen_diag)
        ):
            match = re.search(
                r"return_compiled=(\d+) return_runs=(\d+) return_bails=(\d+)",
                diagnostics,
            )
            if not match:
                raise AssertionError(
                    f"seed {seed}: {backend} lacks return-stack counters"
                )
            compiled, executed, bailed = map(int, match.groups())
            if not (compiled >= 1 and executed >= 5 and bailed == 0):
                raise AssertionError(
                    f"seed {seed}: {backend} didn't run compiled "
                    f"return stack: {match.group(0)}"
                )
        print(
            f"PASS seed={seed:03d}: five-way non-tail parity, "
            "builtin/Binaryen compiled continuation",
            flush=True,
        )
    print(f"PASS {args.seeds} randomized non-tail recursions", flush=True)


if __name__ == "__main__":
    main()
