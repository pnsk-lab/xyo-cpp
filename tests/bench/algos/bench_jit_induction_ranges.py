#!/usr/bin/env python3
"""Paired performance test of static for-each induction range analysis.

The measured work is a normal Scratch warp procedure (not a custom runtime
kernel). No benchmark ID or project-specific rule reaches the JIT optimizer.
"""
import argparse
import json
import os
import random
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_add, S_call, S_change, S_delall, S_repeat, S_set, add, days,
    make_project, mod, mul, num, sub, var, write_sb3,
)


def build_project(n, kind: str):
    g = Gen(["i", "sum", "n", "t0", "t1"], ["times", "checks"])
    d = g.define("bench", [], [
        S_set("sum", num(0)), S_set("n", num(n)), S_set("t0", days())
    ], warp=True)
    last = g.blocks[d]["next"]
    while g.blocks[last]["next"]:
        last = g.blocks[last]["next"]
    loop = g.new("control_for_each", parent=last,
                 fields={"VARIABLE": g.field("VARIABLE", "i")})
    g.blocks[last]["next"] = loop
    # Dynamic limit and fractional limit deliberately exercise fallbacks.
    g.blocks[loop]["inputs"]["VALUE"] = g.emit_value(
        var("n") if kind == "dynamic" else num(n), loop)
    if kind == "simple" or kind == "dynamic":
        body = [S_change("sum", mod(var("i"), num(7)))]
    elif kind == "compound":
        body = [S_change("sum", mod(add(var("i"), num(3)), num(5)))]
    elif kind == "multiply":
        body = [S_change("sum", mod(mul(var("i"), num(3)), num(13)))]
    elif kind == "negative":
        body = [S_change("sum", mod(sub(var("i"), num(500)), num(17)))]
    else:
        raise ValueError(kind)
    g.blocks[loop]["inputs"]["SUBSTACK"] = [2, g.chain(body, loop)]
    g.blocks[loop]["next"] = g.chain([S_set("t1", days())], loop)
    g.script([
        S_delall("times"), S_delall("checks"),
        S_call("bench"),  # pay compilation / warmup before timed repetitions
        S_repeat(num(3), [
            S_call("bench"),
            S_add("times", mul(sub(var("t1"), var("t0")), num(86400000))),
            S_add("checks", var("sum")),
        ])
    ])
    return make_project(g.vars.keys(), g.lists.keys(), g)


def execute(binary, sb3, enabled, cpu):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_GPU": "0", "XYO_PARFOR": "0",
        "XYO_JIT_INDUCTION_RANGES": "1" if enabled else "0",
        "XYO_DUMPVARS": "json",
    })
    args = ["taskset", "-c", str(cpu), str(binary), "run", str(sb3),
            "--jit", "eager"]
    p = subprocess.run(args, cwd=ROOT, env=env, capture_output=True,
                       text=True, timeout=120)
    if p.returncode != 0:
        raise RuntimeError(f"{sb3.name} ON={enabled}: {p.stderr[-1500:]}")
    try:
        obj = json.loads(p.stdout[p.stdout.index("{"):p.stdout.rindex("}") + 1])
        item = obj["Bench"]
        times = [float(v) for v in item["times"]]
        checks = [str(v) for v in item["checks"]]
    except (KeyError, ValueError, IndexError) as exc:
        raise RuntimeError(f"unexpected output: {p.stdout[-900:]}") from exc
    if len(times) != 3 or any(not 0 < x < 1000000 for x in times):
        raise RuntimeError(f"missing or invalid times {times}")
    if len(checks) != 3 or len(set(checks)) != 1:
        raise RuntimeError(f"checksum mismatch between repetitions: {checks}")
    return {"median_ms": statistics.median(times), "times": times,
            "checksum": checks[0]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out", type=Path,
                        default=ROOT / "build/induction-range-paired.json")
    parser.add_argument("--cpu", type=int, default=12)
    parser.add_argument("--blocks", type=int, default=8)
    parser.add_argument("--count", type=int, default=200000)
    parser.add_argument("--seed", type=int, default=20261104)
    args = parser.parse_args()
    if args.cpu not in os.sched_getaffinity(0):
        parser.error("requested CPU is not available")
    binary = args.binary.resolve()
    projects = args.out.parent / "induction-range-bench-projects"
    projects.mkdir(parents=True, exist_ok=True)
    kinds = ["simple", "compound", "multiply", "negative", "dynamic"]
    for name in kinds:
        write_sb3(projects / (name + ".sb3"), build_project(args.count, name))
    rng = random.Random(args.seed)
    rows = []
    for block in range(args.blocks):
        jobs = [(name, toggle) for name in kinds for toggle in (False, True)]
        rng.shuffle(jobs)
        for name, enabled in jobs:
            result = execute(binary, projects / (name + ".sb3"), enabled,
                             args.cpu)
            rows.append({"block": block, "case": name, "enabled": enabled,
                         **result})
            args.out.write_text(json.dumps(rows, indent=2))
            print(block, name, "ON" if enabled else "OFF", result, flush=True)
    for name in kinds:
        on = [r for r in rows if r["case"] == name and r["enabled"]]
        off = [r for r in rows if r["case"] == name and not r["enabled"]]
        assert {r["checksum"] for r in on + off}.__len__() == 1, name
        ratios = [o["median_ms"] / p["median_ms"] for o, p in zip(off, on)]
        print(f"RESULT {name} paired_median={statistics.median(ratios):.4f} "
              f"ratios={','.join(f'{ratio:.3f}' for ratio in ratios)}",
              flush=True)


if __name__ == "__main__":
    main()
