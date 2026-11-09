#!/usr/bin/env python3
"""A/B an identical real Scratch radix workload with two xyo binaries.

Both xyo builds have the same C++ sources except jsnum.cpp. Runs alternate
orders and compare complete variable dumps before reporting timings.
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
sys.path.insert(0, str(ROOT / "tests/differential"))
from jit_to_num_radix import make_fixture
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_call, S_change, S_repeat, S_set,
    join, mod, num, text, var, make_project, write_sb3,
)


def make_digit_fixture(path: Path, iterations: int) -> None:
    # Join creates fresh numeric strings; this exercises the cold
    # toNumber parser rather than numeric/boxed immediate arithmetic.
    generator = Gen(["i", "s", "sum", "done"], [])
    loop = [
        S_change("i", num(1)),
        S_set("s", join(text(""), mod(var("i"), num(10)))),
        S_change("sum", var("s")),
    ]
    generator.define("digits", [], [
        S_set("i", num(0)), S_set("sum", num(0)),
        S_repeat(num(iterations), loop), S_set("done", num(1)),
    ], warp=True)
    generator.script([S_call("digits")])
    write_sb3(path, make_project(generator.vars.keys(),
                                generator.lists.keys(), generator))



def run(binary: Path, fixture: Path, frames: int) -> tuple[float, dict]:
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("XYO_")}
    env.update(XYO_PAR="0", XYO_GPU="0", XYO_DUMPVARS="json",
               XYO_JIT_TO_NUM_COLD_HELPER="0")
    start = time.perf_counter()
    process = subprocess.run(
        [str(binary), "run", str(fixture), "--jit", "eager",
         "--frames", str(frames)],
        cwd=ROOT, env=env, text=True, capture_output=True, timeout=180,
    )
    milliseconds = (time.perf_counter() - start) * 1000
    if process.returncode:
        raise RuntimeError(
            (binary.name, process.returncode, process.stderr[-1300:]))
    raw = process.stdout
    start_json = raw.find("{")
    end_json = raw.rfind("}")
    if start_json == -1 or end_json == -1:
        raise ValueError((binary.name, raw[-500:]))
    values = json.loads(raw[start_json:end_json + 1])["Bench"]
    if values.get("done") != "1":
        raise RuntimeError(("not finished", binary.name, values))
    return milliseconds, values


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--workload", choices=("radix", "digit"),
                        default="radix")
    parser.add_argument("--iterations", type=int, default=40000)
    parser.add_argument("--frames", type=int, default=4000)
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--output", type=Path,
                        default=Path("build-to-num-cold/radix-e2e.json"))
    args = parser.parse_args()
    before, after = args.before.resolve(), args.after.resolve()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    fixture = output.parent / (args.workload + "-e2e.sb3")
    if args.workload == "digit":
        make_digit_fixture(fixture, args.iterations)
    else:
        make_fixture(fixture, warp=True, iterations=args.iterations)
    samples: dict[str, list[float]] = {"before": [], "after": []}
    reference = None
    for index in range(args.runs + 1):
        # Round zero preloads the filesystem and JIT setup, not measured.
        order = (("before", before), ("after", after))
        if index % 2:
            order = order[::-1]
        for name, binary in order:
            ms, variables = run(binary, fixture, args.frames)
            if reference is None:
                reference = variables
            elif reference != variables:
                raise AssertionError(("Scratch mismatch", name,
                                      reference, variables))
            if index > 0:
                samples[name].append(round(ms, 3))
        print(f"completed pair {index}/{args.runs}", flush=True)
    result = {
        "workload": args.workload,
        "iterations": args.iterations,
        "runs": args.runs,
        "samples_ms": samples,
        "before_median_ms": statistics.median(samples["before"]),
        "after_median_ms": statistics.median(samples["after"]),
        "variables_equal": True,
    }
    output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2), flush=True)


if __name__ == "__main__":
    main()
