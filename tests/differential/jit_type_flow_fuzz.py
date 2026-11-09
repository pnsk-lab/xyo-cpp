#!/usr/bin/env python3
"""Deterministic differential fuzzing for CFG/JIT type flow.

Generates mixed boxed-value Scratch programs, with no hard-coded benchmark
output. Compare the interpreter with eager/hotspot LLVM execution and with
the experimental CFG numeric-shadow hint enabled.
"""
import json
import os
import random
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_if, S_ifelse, S_repeat, S_set,
    add, gt, lt, num, text, var, join, make_project, write_sb3,
)

LITERALS = (
    "", " ", " 12.5 ", "NaN", "Infinity", "-Infinity", "-0",
    "0x10", "1.7e3", "hello", "false", "100",
)


def random_expr(rng, depth=0):
    basic = [
        lambda: num(rng.choice((-100, -1, 0, 1, 2, 17, 100))),
        lambda: text(rng.choice(LITERALS)),
        lambda: var(rng.choice(("x", "y", "z"))),
    ]
    if depth >= 2:
        return rng.choice(basic)()
    extended = basic + [
        lambda: add(random_expr(rng, depth + 1),
                    random_expr(rng, depth + 1)),
        lambda: join(random_expr(rng, depth + 1),
                     text(rng.choice(("a", "", "1")))),
    ]
    return rng.choice(extended)()


def random_statement(rng):
    target = rng.choice(("x", "y", "z"))
    choice = rng.randrange(5)
    if choice <= 1:
        return S_set(target, random_expr(rng))
    if choice == 2:
        return S_change(target, random_expr(rng))
    if choice == 3:
        return S_ifelse(
            gt(random_expr(rng), random_expr(rng)),
            [S_set(target, random_expr(rng))],
            [S_set(target, random_expr(rng))])
    return S_if(
        lt(random_expr(rng), random_expr(rng)),
        [S_set(target, random_expr(rng))])


def build(path: Path, case: int, rng: random.Random):
    warp = case % 2 == 0
    count = (0, 1, 2, 63, 64, 65, 127, 129)[case % 8]
    gen = Gen(("x", "y", "z", "out", "done"), [])
    statements = [
        S_set("x", num(0)), S_set("y", text(" 2 ")),
        S_set("z", num(-1)),
    ]
    body = [random_statement(rng) for _ in range(6)]
    body += [S_set("out", add(var("x"), var("y"))),
             S_change("z", var("out"))]
    statements.extend([S_repeat(num(count), body),
                       S_set("done", num(1))])
    gen.define("mixed", [], statements, warp=warp)
    gen.script([S_call("mixed")])
    write_sb3(path, make_project(gen.vars.keys(), gen.lists.keys(), gen))
    return ("x", "y", "z", "out", "done")


def execute(binary, path, keys, tier, extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update(XYO_DUMPVARS="json", XYO_PAR="0", XYO_GPU="0",
               XYO_JIT_CONDITIONAL_SSA="0")
    env.update(extra)
    proc = subprocess.run(
        [str(binary), "run", str(path), "--jit", tier, "--frames", "900"],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=100)
    if proc.returncode:
        raise RuntimeError((path.name, tier, extra, proc.returncode,
                            proc.stderr[-1200:]))
    stdout = proc.stdout
    result = json.loads(
        stdout[stdout.index("{"):stdout.rindex("}") + 1])["Bench"]
    return tuple(result.get(key) for key in keys)


def main():
    binary = (Path(sys.argv[1]).resolve() if len(sys.argv) > 1
              else ROOT / "build/xyo")
    root = ROOT / "build/type-flow-fuzz"
    root.mkdir(parents=True, exist_ok=True)
    rng = random.Random(0x5381F10A)
    combinations = [
        ("eager", {"XYO_JIT_CFG_TYPE_HINTS": "0"}),
        ("eager", {"XYO_JIT_CFG_TYPE_HINTS": "1"}),
        ("eager", {"XYO_JIT_LAZY_TYPED_SSA": "1"}),
        ("hotspot", {"XYO_JIT_CFG_TYPE_HINTS": "1"}),
        ("eager", {"XYO_JIT_IF_PHI": "1", "XYO_JIT_PHI_DEMAND": "1",
                   "XYO_JIT_LAZY_TYPED_SSA": "1"}),
        ("hotspot", {"XYO_JIT_IF_PHI": "1", "XYO_JIT_PHI_DEMAND": "1",
                     "XYO_JIT_LAZY_TYPED_SSA": "1"}),
        ("eager", {"XYO_JIT_DUPLICATE_SCALAR_NUM": "1"}),
        ("hotspot", {"XYO_JIT_DUPLICATE_SCALAR_NUM": "1"}),
    ]
    for case in range(24):
        path = root / f"case-{case:02d}.sb3"
        keys = build(path, case, rng)
        expected = execute(binary, path, keys, "off", {})
        if expected[-1] != "1":
            raise AssertionError(("fixture incomplete", case, expected))
        for tier, flags in combinations:
            actual = execute(binary, path, keys, tier, flags)
            if actual != expected:
                raise AssertionError((case, tier, flags, expected, actual))
        print(f"PASS case={case:02d} warp={case % 2 == 0}",
              flush=True)


if __name__ == "__main__":
    main()
