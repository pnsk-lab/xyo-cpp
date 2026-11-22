#!/usr/bin/env python3
"""JIT daysSince2000() double fast path: exact Scratch parity regression.

The wall clock advances between runs, so each expression is chosen to have
a time-invariant outcome. Compare Interpreter with both JIT tiers and the
boxed compatibility path, without mocking or reusing the clock sample.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_ifelse, S_repeat, S_set,
    add, days, div, gt, lt, mul, num, sub, make_project, write_sb3,
)


def pos_inf():
    return div(num(1), num(0))


def neg_inf():
    return div(num(-1), num(0))


# Avoid non-reproducible comparisons to the exact value of the wall clock.
CASES = [
    ("sub_pos_inf", lambda: sub(days(), pos_inf())),
    ("sub_neg_inf", lambda: sub(days(), neg_inf())),
    ("sub_overflow", lambda: sub(days(), mul(num(1e308), num(1e308)))),
    ("add_pos_inf", lambda: add(days(), pos_inf())),
    ("add_neg_inf", lambda: add(days(), neg_inf())),
    ("mul_zero", lambda: mul(days(), num(0))),
    ("div_zero", lambda: div(num(0), days())),
    ("gt_zero", lambda: gt(days(), num(0))),
    ("lt_zero", lambda: lt(days(), num(0))),
    ("gt_future", lambda: gt(days(), num(1000000))),
    ("lt_future", lambda: lt(days(), num(1000000))),
    ("gt_inf_minus", lambda: gt(sub(days(), pos_inf()), num(0))),
    ("lt_inf_minus", lambda: lt(sub(days(), pos_inf()), num(0))),
]


def fixture(path: Path, *, warp: bool) -> list[str]:
    keys = [name for name, _ in CASES]
    gen = Gen(keys + ["done"], [])
    blocks = [
        S_ifelse(expr(), [S_set(name, num(1))], [S_set(name, num(0))])
        if name.startswith(("gt_", "lt_"))
        else S_set(name, expr())
        for name, expr in CASES
    ]
    gen.define("days_fastpath", [],
               [S_repeat(num(200), blocks), S_set("done", num(1))], warp=warp)
    gen.script([S_call("days_fastpath")])
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(path, make_project(gen.vars.keys(), gen.lists.keys(), gen))
    return keys


def run(binary: Path, project: Path, *, mode: str, fast: bool) -> dict[str, str]:
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_DUMPVARS": "json",
        "XYO_GPU": "0",
        "XYO_PAR": "0",
        "XYO_PARFOR": "0",
        "XYO_JIT_DAYS_SHARED_CLOCK": "0",
        "XYO_JIT_DAYS_F64": "1" if fast else "0",
    })
    result = subprocess.run(
        [str(binary), "run", str(project), "--jit", mode, "--frames", "20000"],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=90,
    )
    if result.returncode:
        raise RuntimeError(
            f"mode={mode}, fast={fast}, exit={result.returncode}: "
            f"{result.stderr[-1600:]} {result.stdout[-500:]}"
        )
    try:
        state = json.loads(
            result.stdout[result.stdout.index("{"):result.stdout.rindex("}") + 1]
        )["Bench"]
    except (ValueError, KeyError) as error:
        raise AssertionError(result.stdout[-1300:]) from error
    assert state["done"] == "1", (mode, fast, "unfinished", state)
    return state


def main() -> int:
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} XYO_BINARY OUTPUT_DIRECTORY", file=sys.stderr)
        return 2
    binary, out = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    comparisons = 0
    for warp in (True, False):
        fixture_path = out / ("warp.sb3" if warp else "nonwarp.sb3")
        keys = fixture(fixture_path, warp=warp)
        expected = run(binary, fixture_path, mode="off", fast=False)
        for mode, fast in (
            ("off", True),
            ("eager", False), ("eager", True),
            ("hotspot", False), ("hotspot", True),
        ):
            actual = run(binary, fixture_path, mode=mode, fast=fast)
            mismatches = {
                key: (expected[key], actual[key])
                for key in keys if expected[key] != actual[key]
            }
            assert not mismatches, (warp, mode, fast, mismatches)
            comparisons += len(keys)
            print(f"PASS warp={warp} mode={mode} f64={fast} cases={len(keys)}",
                  flush=True)
    print(f"PASS daysSince2000 f64 exact invariant parity: {comparisons} comparisons")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
