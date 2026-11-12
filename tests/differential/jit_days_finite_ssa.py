#!/usr/bin/env python3
"""Exact synchronous daysSince2000 finite-SSA numeric regression.

Compare interpreter, eager LLVM and hotspot LLVM using invariant results.
Never compare wall-clock readings across runs.
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
    Gen, S_call, S_ifelse, S_repeat, S_set, add, days, div, gt, lt,
    mul, num, sub, make_project, write_sb3,
)

POS_INF = lambda: div(num(1), num(0))
NEG_INF = lambda: div(num(-1), num(0))
# Infinite inputs are generated as expressions (not numeric literal shadows).
CASES = [
    ("sub_pos_inf", lambda: sub(days(), POS_INF())),
    ("sub_neg_inf", lambda: sub(days(), NEG_INF())),
    ("sub_overflow", lambda: sub(days(), mul(num(1e308), num(1e308)))),
    ("add_pos_inf", lambda: add(days(), POS_INF())),
    ("add_neg_inf", lambda: add(days(), NEG_INF())),
    ("mul_zero", lambda: mul(days(), num(0))),
    ("div_zero", lambda: div(num(0), days())),
    ("gt_zero", lambda: gt(days(), num(0))),
    ("lt_zero", lambda: lt(days(), num(0))),
    ("gt_far_future", lambda: gt(days(), num(1000000))),
    ("lt_far_future", lambda: lt(days(), num(1000000))),
    ("gt_finite_sub", lambda: gt(sub(days(), num(1)), num(0))),
    ("lt_finite_sub", lambda: lt(sub(days(), num(1)), num(0))),
    ("gt_inf_minus", lambda: gt(sub(days(), POS_INF()), num(0))),
    ("lt_inf_minus", lambda: lt(sub(days(), POS_INF()), num(0))),
]

def fixture(path: Path, warp: bool) -> list[str]:
    keys = [name for name, _ in CASES]
    g = Gen(keys + ["done"], [])
    body = [
        S_ifelse(expression(), [S_set(name, num(1))], [S_set(name, num(0))])
        if name.startswith(("gt_", "lt_"))
        else S_set(name, expression())
        for name, expression in CASES
    ]
    g.define(
        "finite_days_test", [],
        [S_repeat(num(200), body), S_set("done", num(1))], warp=warp,
    )
    g.script([S_call("finite_days_test")])
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(path, make_project(g.vars.keys(), g.lists.keys(), g))
    return keys

def run(binary: Path, project: Path, mode: str, enabled: bool) -> dict[str, str]:
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_DUMPVARS": "json",
        "XYO_GPU": "0",
        "XYO_PAR": "0",
        "XYO_PARFOR": "0",
        "XYO_JIT_DAYS_SHARED_CLOCK": "0",
        "XYO_JIT_DAYS_F64": "1",
        "XYO_JIT_DAYS_FINITE_SSA": "1" if enabled else "0",
    })
    process = subprocess.run(
        [str(binary), "run", str(project), "--jit", mode, "--frames", "20000"],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=180,
    )
    if process.returncode:
        raise RuntimeError(
            f"mode={mode} finite={enabled} exit={process.returncode}: "
            f"{process.stderr[-1800:]} {process.stdout[-500:]}"
        )
    output = process.stdout
    state = json.loads(output[output.index("{"):output.rindex("}") + 1])["Bench"]
    if state["done"] != "1":
        raise AssertionError((mode, enabled, "unfinished"))
    return state

def main() -> int:
    binary = (Path(sys.argv[1]).resolve() if len(sys.argv) > 1
              else ROOT / "build/xyo")
    out = (Path(sys.argv[2]).resolve() if len(sys.argv) > 2
           else ROOT / "build/days-finite-differential")
    tested = 0
    for warp in (True, False):
        path = out / ("warp.sb3" if warp else "nonwarp.sb3")
        keys = fixture(path, warp)
        expected = run(binary, path, "off", False)
        for mode, enabled in (
            ("eager", False), ("eager", True),
            ("hotspot", False), ("hotspot", True),
        ):
            result = run(binary, path, mode, enabled)
            errors = {key: (expected[key], result[key])
                      for key in keys if expected[key] != result[key]}
            if errors:
                raise AssertionError((warp, mode, enabled, errors))
            tested += len(keys)
            print(f"PASS warp={warp} mode={mode} finite={enabled} "
                  f"cases={len(keys)}", flush=True)
    print(f"PASS days finite SSA differential: {tested} comparisons", flush=True)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
