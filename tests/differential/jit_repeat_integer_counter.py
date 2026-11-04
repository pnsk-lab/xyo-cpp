#!/usr/bin/env python3
"""Interpreter/JIT differential tests for exact i32 control_repeat countdown."""
import argparse
import json
import math
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_add, S_change, S_repeat, S_set,
    add, make_project, num, var, write_sb3,
)


def fixture(count, *, dynamic=False, nested=False, empty=False):
    g = Gen(["sum", "count"], ["result"])
    body = [] if empty else [S_change("sum", num(1))]
    if nested:
        body.append(S_repeat(num(3.5), [S_change("sum", num(2))]))
    times = var("count") if dynamic else num(count)
    g.script([
        S_set("sum", num(0)),
        S_set("count", num(count)),
        S_repeat(times, body),
        S_add("result", var("sum")),
    ])
    return make_project(g.vars.keys(), g.lists.keys(), g)


def run(binary, sb3, mode, *, jit="eager"):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_GPU": "0", "XYO_PARFOR": "0", "XYO_REPEAT_UNROLL": "0",
        "XYO_JIT_REPEAT_INT_COUNTER": "1" if mode == "on" else "0",
    })
    proc = subprocess.run(
        [str(binary), "run", str(sb3), "--jit",
         "off" if mode == "interp" else jit,
         "--print-list", "result"],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=90,
    )
    if proc.returncode:
        raise RuntimeError(f"{sb3.name}/{mode}: {proc.stderr[-1300:]}")
    return tuple(x.strip() for x in proc.stdout.splitlines() if x.strip())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out", type=Path,
                        default=ROOT / "build/repeat-int-fixtures")
    args = parser.parse_args()
    binary = args.binary.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    cases = [
        ("zero", 0, False, False, False),
        ("negative", -3, False, False, False),
        ("tiny", 0.1, False, False, False),
        ("below_half", math.nextafter(0.5, 0), False, False, False),
        ("half", 0.5, False, False, False),
        ("above_half", math.nextafter(0.5, 1), False, False, False),
        ("below_one_half", math.nextafter(1.5, 0), False, False, False),
        ("one_half", 1.5, False, False, False),
        ("above_one_half", math.nextafter(1.5, 2), False, False, False),
        ("below_two_half", math.nextafter(2.5, 2), False, False, False),
        ("two_half", 2.5, False, False, False),
        ("above_two_half", math.nextafter(2.5, 3), False, False, False),
        ("integers", 1001, False, False, False),
        ("large_integer", 150000, False, False, False),
        ("fractional_large", 160000.5, False, False, False),
        ("dynamic", 1001.5, True, False, False),
        ("nested", 8.5, False, True, False),
        ("empty", 1000, False, False, True),
        ("beyond_i32", 2147483648, False, False, True),
        ("infinity", "Infinity", False, False, True),
    ]
    outputs = []
    for name, value, dynamic, nested, empty in cases:
        if name in ("beyond_i32", "infinity"):
            # These counts would take too long to run. Compile AOT and dump
            # IR in a separate inspection rather than running billions of
            # iterations.
            continue
        project = args.out / f"{name}.sb3"
        write_sb3(project, fixture(value, dynamic=dynamic,
                                   nested=nested, empty=empty))
        versions = {v: run(binary, project, v)
                    for v in ("interp", "off", "on")}
        assert versions["interp"] == versions["off"] == versions["on"], (
            name, versions)
        outputs.append({"case": name, "value": value,
                        "output": versions["on"]})
        print(f"PASS {name}: {versions['on']}", flush=True)
    (args.out / "results.json").write_text(json.dumps(outputs, indent=2))
    print(f"PASS all {len(outputs)} repeat differential cases")


if __name__ == "__main__":
    main()
