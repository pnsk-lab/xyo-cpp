#!/usr/bin/env python3
"""Measure proof-based i32 repeat counters in ordinary Scratch loops.

Run with one fixed Release binary and alternating ON/OFF across independent
processes. The compiler never recognizes the benchmark name or project ID.
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
    Gen, S_add, S_call, S_change, S_delall, S_repeat, S_set,
    add, days, make_project, mod, mul, num, sub, var, write_sb3,
)


def project(count, kind):
    g = Gen(["sum", "count", "t0", "t1"], ["times", "checks"])
    repeats = var("count") if kind == "dynamic" else (
        num(count + 0.5) if kind == "fractional" else num(count)
    )
    if kind == "arith":
        body = [S_change("sum", add(mod(var("sum"), num(9)), num(1)))]
    else:
        body = [S_change("sum", num(1))]
    g.define("bench", [], [
        S_set("sum", num(0)),
        S_set("count", num(count)),
        S_set("t0", days()),
        S_repeat(repeats, body),
        S_set("t1", days())
    ], warp=True)
    g.script([
        S_delall("times"), S_delall("checks"),
        S_call("bench"),
        S_repeat(num(3), [
            S_call("bench"),
            S_add("times", mul(sub(var("t1"), var("t0")), num(86400000))),
            S_add("checks", var("sum")),
        ])
    ])
    return make_project(g.vars.keys(), g.lists.keys(), g)


def run(binary, sb3, enabled, cpu):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_GPU": "0",
        "XYO_PARFOR": "0",
        "XYO_REPEAT_UNROLL": "0",
        "XYO_JIT_REPEAT_INT_COUNTER": "1" if enabled else "0",
        "XYO_DUMPVARS": "json",
    })
    p = subprocess.run(
        ["taskset", "-c", str(cpu), str(binary), "run", str(sb3),
         "--jit", "hotspot"],
        cwd=ROOT, env=env, text=True, capture_output=True, timeout=120,
    )
    if p.returncode:
        raise RuntimeError(p.stderr[-1200:])
    try:
        data = json.loads(p.stdout[p.stdout.index("{"):p.stdout.rindex("}") + 1])
        times = list(map(float, data["Bench"]["times"]))
        checks = list(map(str, data["Bench"]["checks"]))
    except (ValueError, KeyError, IndexError) as exc:
        raise RuntimeError(f"invalid JSON: {p.stdout[-1200:]}") from exc
    if len(times) != 3 or min(times) <= 0 or len(set(checks)) != 1:
        raise RuntimeError(f"missing result: {times}, {checks}")
    return {"median_ms": statistics.median(times), "checksum": checks[0],
            "timings": times}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--cpu", type=int, default=12)
    parser.add_argument("--blocks", type=int, default=8)
    parser.add_argument("--count", type=int, default=12000000)
    parser.add_argument("--out", type=Path,
                        default=ROOT / "build/repeat-int-paired.json")
    args = parser.parse_args()
    if args.cpu not in os.sched_getaffinity(0):
        parser.error("CPU not available")
    binary = args.binary.resolve()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    sbdir = args.out.parent / "repeat-int-bench-projects"
    sbdir.mkdir(parents=True, exist_ok=True)
    kinds = ["simple", "arith", "fractional", "dynamic"]
    for kind in kinds:
        write_sb3(sbdir / f"{kind}.sb3", project(args.count, kind))
    rows = []
    rng = random.Random(20261104)
    for block in range(args.blocks):
        jobs = [(kind, on) for kind in kinds for on in (False, True)]
        rng.shuffle(jobs)
        for kind, on in jobs:
            data = run(binary, sbdir / f"{kind}.sb3", on, args.cpu)
            rows.append({"block": block, "case": kind, "on": on, **data})
            args.out.write_text(json.dumps(rows, indent=2))
            print(block, kind, "ON" if on else "OFF",
                  f"{data['median_ms']:.3f}ms", data["checksum"], flush=True)
    for kind in kinds:
        on = [r for r in rows if r["case"] == kind and r["on"]]
        off = [r for r in rows if r["case"] == kind and not r["on"]]
        assert len({r["checksum"] for r in on + off}) == 1, kind
        ratios = [a["median_ms"]/b["median_ms"] for a,b in zip(off,on)]
        print("RESULT", kind, "speedup",
              f"{statistics.median(ratios):.4f}", "pairs",
              ",".join(f"{n:.3f}" for n in ratios), flush=True)


if __name__ == "__main__":
    main()
