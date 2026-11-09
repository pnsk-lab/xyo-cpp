#!/usr/bin/env python3
"""Seeded multi-return-PC numeric argument-frame execution parity."""
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
    Gen, S_call, S_change, S_if, S_repeat, S_set,
    add, arg, lt, num, make_project, write_sb3,
)
from wasm_nested_continuation import run


def generate(folder: Path, seed: int) -> Path:
    rng = random.Random(seed)
    argc = 1 + seed % 2
    names = ["a", "b"][:argc]
    code = "worker " + " ".join("%s" for _ in names)
    limit1 = rng.randint(3, 6)
    limit2 = rng.randint(1, 3)
    prefix = [
        S_change("x", num(rng.choice((-4, -2, 1, 3)))),
        S_change("y", num(1)),
    ]
    if rng.choice((True, False)):
        prefix.append(S_repeat(num(rng.randint(1, 3)), [
            S_change("z", num(rng.choice((-1, 1, 3)))),
        ]))
    mid = [S_change("x", arg("a"))]
    if rng.choice((True, False)):
        mid.append(S_repeat(num(rng.randint(1, 3)), [
            S_change("y", num(rng.choice((1, 2, 3)))),
        ]))
    suffix = [
        S_change("z", arg("a")),
        S_change("z", arg("b")) if argc == 2 else
        S_change("z", num(1)),
    ]
    child1 = [add(arg("a"), num(1))]
    child2 = [add(arg("a"), num(2))]
    if argc == 2:
        child1.append(add(arg("b"), arg("a")))
        child2.append(add(arg("b"), num(rng.choice((-2, 1, 3)))))
    procedure = [
        *prefix,
        S_if(lt(arg("a"), num(limit1)), [S_call(code, child1)]),
        *mid,
        S_if(lt(arg("a"), num(limit2)), [S_call(code, child2)]),
        *suffix,
    ]
    gen = Gen(["x", "y", "z", "i"], [])
    gen.define(code, names, procedure, warp=True)
    gen.define("top", [], [
        S_set("x", num(0)),
        S_set("y", num(0)),
        S_set("z", num(0)),
        S_repeat(num(1), [
            S_call(code, [num(0)] if argc == 1 else
                   [num(0), num(rng.randint(-2, 3))]),
        ]),
    ], warp=True)
    hat = gen.script([S_repeat(num(95), [S_call("top")])])
    first = gen.blocks[hat]["next"]
    wait = gen.new("control_wait", parent=hat)
    gen.blocks[wait]["inputs"]["DURATION"] = gen.emit_value(num(0), wait)
    gen.blocks[hat]["next"] = wait
    gen.blocks[wait]["next"] = first
    gen.blocks[first]["parent"] = wait
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / f"pc_{seed:04d}.sb3"
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
        sb3 = generate(args.out, seed)
        interpreter, _ = run(args.wasm, sb3, "off")
        nested_off, _ = run(
            args.wasm, sb3, "hotspot", addon=args.addon, nested=False
        )
        builtin, builtin_diag = run(args.wasm, sb3, "hotspot")
        binaryen, binaryen_diag = run(
            args.wasm, sb3, "hotspot", addon=args.addon
        )
        llvm, _ = run(args.llvm, sb3, "hotspot", native=True)
        if len({interpreter, nested_off, builtin, binaryen, llvm}) != 1:
            raise AssertionError(
                f"seed {seed}: output mismatch\ninterp={interpreter}\n"
                f"nested_off={nested_off}\nbuiltin={builtin}\n"
                f"binaryen={binaryen}\nllvm={llvm}"
            )
        for backend, diag in (("builtin", builtin_diag),
                              ("binaryen", binaryen_diag)):
            found = re.search(
                r"pc_compiled=(\d+) pc_runs=(\d+) pc_bails=(\d+)",
                diag,
            )
            if not found:
                raise AssertionError(f"seed {seed}: {backend}: missing stats")
            compiled, executed, bailed = map(int, found.groups())
            if compiled < 1 or executed < 10 or bailed != 0:
                raise AssertionError(
                    f"seed {seed}: {backend}: did not execute multi-PC "
                    f"return stack: {found.group(0)}"
                )
        print(
            f"PASS seed {seed:03d}: 5-way parity and real builtin/Binaryen "
            f"{1+seed%2}-argument two-return-PC execution",
            flush=True,
        )
    print(f"PASS {args.seeds} seeded return-PC recursion programs", flush=True)


if __name__ == "__main__":
    main()
