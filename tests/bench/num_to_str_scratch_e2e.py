#!/usr/bin/env python3
"""End-to-end A/B of ECMAScript double-to-string formatting in real Scratch.

Constructs a Scratch project that formats a different fractional or
scientific numeric value in every iteration and joins it with a string.
Runs the same SB3 against two binaries built from the same source with
only XYO_ASM_ECMA_FORMAT changed. Compares all variables before timing.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_repeat, S_set, add, div, join,
    mul, num, text, var, make_project, write_sb3,
)


def make_fixture(path: Path, iterations: int, workload: str) -> None:
    g = Gen(["i", "value", "done"], [])
    if workload == "fraction":
        source = div(var("i"), num(7))
    elif workload == "exponent":
        source = div(num(1), mul(var("i"), num(1000000)))
    elif workload == "mixed":
        source = add(div(var("i"), num(7)),
                     div(num(1), mul(var("i"), num(1000000))))
    else:
        raise ValueError(workload)

    body = [
        S_change("i", num(1)),
        S_set("value", join(text("value="), source)),
    ]
    g.define("stringify", [], [
        S_set("i", num(0)),
        S_set("value", text("")),
        S_repeat(num(iterations), body),
        S_set("done", num(1)),
    ], warp=True)
    g.script([S_call("stringify")])
    write_sb3(path, make_project(g.vars.keys(), g.lists.keys(), g))


def execute(binary: Path, project: Path, frames: int) -> tuple[float, dict]:
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("XYO_")}
    env.update(XYO_PAR="0", XYO_GPU="0", XYO_DUMPVARS="json")
    begin = time.perf_counter()
    p = subprocess.run(
        [str(binary), "run", str(project), "--jit", "eager",
         "--frames", str(frames)],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=180,
    )
    ms = (time.perf_counter() - begin) * 1000
    if p.returncode:
        raise RuntimeError((binary.name, p.returncode, p.stderr[-1500:]))
    data = p.stdout
    variables = json.loads(data[data.index("{"):data.rindex("}") + 1])["Bench"]
    if variables.get("done") != "1":
        raise ValueError(("project incomplete", binary.name, variables))
    return ms, variables


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--before", type=Path, required=True)
    ap.add_argument("--after", type=Path, required=True)
    ap.add_argument("--workload", choices=("fraction", "exponent", "mixed"),
                    default="fraction")
    ap.add_argument("--iterations", type=int, default=100000)
    ap.add_argument("--runs", type=int, default=7)
    ap.add_argument("--frames", type=int, default=5000)
    ap.add_argument("--output", type=Path,
                    default=Path("build-to-num-cold/ecma-scratch-e2e.json"))
    args = ap.parse_args()
    out = args.output.resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    sb3 = out.parent / ("ecma-" + args.workload + ".sb3")
    make_fixture(sb3, args.iterations, args.workload)
    binaries = {"before": args.before.resolve(), "after": args.after.resolve()}
    samples: dict[str, list[float]] = {"before": [], "after": []}
    expected = None
    for index in range(args.runs+1):
        order = ("before", "after") if index % 2 == 0 else ("after", "before")
        for mode in order:
            elapsed, values = execute(binaries[mode], sb3, args.frames)
            if expected is None:
                expected = values
            elif expected != values:
                raise AssertionError((mode, expected, values))
            if index > 0:
                samples[mode].append(round(elapsed, 4))
    result = {
        "workload": args.workload,
        "iterations": args.iterations,
        "runs": args.runs,
        "before_ms": samples["before"],
        "after_ms": samples["after"],
        "before_median_ms": statistics.median(samples["before"]),
        "after_median_ms": statistics.median(samples["after"]),
        "variables_equal": True,
    }
    out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2), flush=True)


if __name__ == "__main__":
    main()
