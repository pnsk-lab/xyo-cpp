#!/usr/bin/env python3
"""Deterministic LLVM/WASM/interpreter parity fuzz for the numeric JIT subset.

Each seed generates a distinct legal Scratch SB3 with conditions, arithmetic
and one optional non-warp counted loop. Results are compared through the real
CLI, not only the intermediate representation or a mocked WebAssembly engine.

Unlike a "no crashes" test, this checks final Scratch-visible values and
raises on disagreement. Failed projects are saved under --out for reproduction.
"""
from __future__ import annotations

import argparse
import random
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "bench" / "algos"))
sys.path.insert(0, str(ROOT / "tests" / "differential"))

from gen_algo_bench import (  # noqa: E402
    Gen, S_set, S_change, S_repeat, S_if, S_ifelse,
    num, var, add, sub, mul, div, mod, lt, gt, eq, op, round_,
    make_project, write_sb3,
)
from wasm_hotspot_integration import run, run_native  # noqa: E402

NUMS = (-10, -3, -2, -1, -0.5, -0.0, 0, 0.5, 1, 2, 3, 10, 100)
VARS = ("x", "y", "z")


def number_expr(rng: random.Random, depth: int):
    if depth == 0 or rng.randrange(5) == 0:
        return (var(rng.choice(VARS)) if rng.randrange(2)
                else num(rng.choice(NUMS)))
    if rng.randrange(8) == 0:
        return round_(number_expr(rng, depth - 1))
    if rng.randrange(5) == 0:
        name = rng.choice(("abs", "floor", "ceiling", "sqrt",
                           "sin", "cos", "tan", "asin", "acos", "atan",
                           "ln", "log", "e ^", "10 ^"))
        return op("operator_mathop", {"OPERATOR": name},
                  NUM=number_expr(rng, depth - 1))
    op_ = rng.choice((add, sub, mul, div, mod))
    return op_(number_expr(rng, depth - 1),
               number_expr(rng, depth - 1))


def bool_expr(rng: random.Random, depth: int):
    if depth == 0 or rng.randrange(3):
        cmp_ = rng.choice((lt, gt, eq))
        return cmp_(number_expr(rng, 1), number_expr(rng, 1))
    if rng.randrange(4) == 0:
        return op("operator_not", OPERAND=bool_expr(rng, depth - 1))
    opcode = rng.choice(("operator_and", "operator_or"))
    return op(opcode, OPERAND1=bool_expr(rng, depth - 1),
              OPERAND2=bool_expr(rng, depth - 1))


def statements(rng: random.Random, count: int):
    result = []
    for _ in range(count):
        kind = rng.randrange(5)
        name = rng.choice(VARS)
        if kind == 0:
            result.append(S_change(name, number_expr(rng, 2)))
        elif kind in (1, 2):
            result.append(S_set(name, number_expr(rng, 2)))
        elif kind == 3:
            result.append(S_if(bool_expr(rng, 2), [
                S_change(rng.choice(VARS), number_expr(rng, 1))]))
        else:
            result.append(S_ifelse(
                bool_expr(rng, 2),
                [S_set(rng.choice(VARS), number_expr(rng, 1))],
                [S_change(rng.choice(VARS), number_expr(rng, 1))]))
    return result


def generate(seed: int, path: Path):
    rng = random.Random(seed)
    g = Gen(VARS, [])
    body = statements(rng, 3)
    if seed % 2 == 0:
        body.append(S_repeat(num(rng.randrange(7)), statements(rng, 2)))
    body += statements(rng, 2)
    if seed % 7 == 0:
        # Exercise the second top-level WASM continuation and capture of
        # its own count (including empty and zero-iteration loops).
        body.append(S_repeat(num(rng.randrange(5)), statements(rng, 2)))
        body += statements(rng, 1)
    g.script(body)
    if seed % 18 == 0:
        # Exercise the ForEach induction variable even when its body
        # independently changes the same numeric Scratch variable.
        for block in g.blocks.values():
            if block["opcode"] == "control_repeat":
                block["opcode"] = "control_for_each"
                block["fields"]["VARIABLE"] = g.field("VARIABLE", "z")
                block["inputs"]["VALUE"] = block["inputs"].pop("TIMES")
                break
    write_sb3(str(path), make_project(VARS, [], g))


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("wasm", type=Path)
    p.add_argument("--llvm", type=Path)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--cases", type=int, default=48)
    opts = p.parse_args()
    if opts.cases < 1 or opts.cases > 1000:
        p.error("--cases must be between 1 and 1000")
    opts.out.mkdir(parents=True, exist_ok=True)
    node_loader = ROOT / "web/xyo-node.mjs"
    wasm = opts.wasm.resolve()
    for seed in range(1, opts.cases + 1):
        file = opts.out / f"wasm_compat_seed_{seed:04d}.sb3"
        generate(seed, file)
        try:
            interp, _ = run("node", node_loader, wasm, file, "off")
            hot, log = run("node", node_loader, wasm, file, "hotspot")
            if "xyo-wasm-hot: compiled script=" not in log:
                raise AssertionError(f"seed={seed}: unexpectedly interpreted")
            if seed % 14 == 0 and "loops=2" not in log:
                raise AssertionError(
                    f"seed={seed}: sequential loops not compiled as two WASM loops")
            if hot != interp:
                raise AssertionError(f"seed={seed}: interpreter={interp}, wasm={hot}")
            if opts.llvm:
                llvm = run_native(opts.llvm, file, "hotspot")
                if llvm != interp:
                    raise AssertionError(
                        f"seed={seed}: interpreter={interp}, LLVM={llvm}")
        except Exception:
            print(f"Mismatch fixture: {file}", file=sys.stderr)
            raise
        # Keep the corpus small: failures retain their files, passes are
        # deleted after successful 3-way comparison.
        file.unlink()
    print(f"PASS {opts.cases} seeds: interpreter / WASM"
          + (" / LLVM" if opts.llvm else "") + " parity")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
