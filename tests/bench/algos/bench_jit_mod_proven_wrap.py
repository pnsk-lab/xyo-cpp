#!/usr/bin/env python3
"""Paired, CPU-pinned in-Scratch timing for proven modulo small-wrap lowering.

Change only XYO_JIT_MOD_PROVEN_SMALL_WRAP between paired runs of the same
binary. Both implementations use the same immutable signed-i32 range proof.
"""
from __future__ import annotations
import argparse
import json
import os
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT / "tests/differential"),
                str(ROOT / "tests/bench/algos")]
from gen_algo_bench import (
    S_add, S_call, S_change, S_delall, S_repeat, S_set,
    days, make_project, mod, mul, num, sub, var, write_sb3
)
from jit_list_proven_i32_index import ForEachGen


def fixture(m: int, n: int, reps: int):
    g = ForEachGen(["i", "sum", "start", "finish"],
                   ["times", "checks"])
    g.define(
        "work", [],
        [S_set("sum", num(0)),
         ("foreach", "i", num(n), [
             S_change("sum", mod(var("i"), num(m)))
         ])],
        warp=True
    )
    g.script([
        S_delall("times"), S_delall("checks"), S_call("work"),
        S_repeat(num(3), [
            S_set("start", days()),
            S_repeat(num(reps), [S_call("work")]),
            S_set("finish", days()),
            S_add("times", mul(sub(var("finish"), var("start")),
                               num(86400000))),
            S_add("checks", var("sum"))
        ])
    ])
    return make_project(g.vars.keys(), g.lists.keys(), g)


def run(binary: Path, project: Path, mode: int):
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update(
        XYO_JIT_MOD_PROVEN_SMALL_WRAP=str(int(mode in (1, 2))),
        XYO_JIT_MOD_PROVEN_I32_WRAP=str(int(mode == 2)),
        XYO_JIT_MOD_SMALL_WRAP="1",
        XYO_JIT_MOD_REAL_WRAP="1",
        XYO_JIT_LAZY_MODE="off",
        XYO_DUMPVARS="json",
        XYO_PARFOR="0", XYO_GPU="0",
    )
    p = subprocess.run(
        ["taskset", "-c", "8", str(binary), "run", str(project),
         "--jit", "eager"], cwd=ROOT, env=env, text=True,
        capture_output=True, timeout=150
    )
    if p.returncode:
        raise RuntimeError((project.name, mode, p.returncode,
                            p.stderr[-600:], p.stdout[-600:]))
    result = json.loads(p.stdout[p.stdout.index("{"):
                                 p.stdout.rindex("}")+1])["Bench"]
    times = [float(x) for x in result["times"]]
    checks = [str(x) for x in result["checks"]]
    assert (len(times) == 3 and len(set(checks)) == 1
            and statistics.median(times) > 0
            and all(0 <= t < 150000 for t in times)), (
        project.name, mode, times, checks
    )
    return {"median_ms": statistics.median(times),
            "samples_ms": times, "checksum": checks[0]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("--out", type=Path, default=ROOT /
                    "build/multi-bench-20261111/proven-small-mod/benchmark.json")
    ap.add_argument("--pairs", type=int, default=8)
    ap.add_argument("--scale", type=int, default=1)
    ap.add_argument("--cases", default="m3,m7,m12,m255,m256,m65536")
    ap.add_argument("--modes", default="0,1",
                    help="0=original i32 remainder, 1=f64 select, 2=i32 select")
    args = ap.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    binary = args.binary.resolve()
    cases = {
        "m2": (2, 4, 60000),
        "m3": (3, 6, 55000),
        "m7": (7, 14, 30000),
        "m12": (12, 24, 20000),
        "m31": (31, 62, 8000),
        "m63": (63, 126, 4000),
        "m127": (127, 254, 2000),
        "m254": (254, 508, 1000),
        "m255": (255, 510, 1000),
        "m256": (256, 512, 1000),
        "m257": (257, 514, 1000),
        "m511": (511, 1022, 500),
        "m512": (512, 1024, 500),
        "m1000": (1000, 2000, 250),
        "m1001": (1001, 2002, 250),
        "m1023": (1023, 2046, 250),
        "m1024": (1024, 2048, 250),
        "m2047": (2047, 4094, 128),
        "m4095": (4095, 8190, 64),
        "m4096": (4096, 8192, 64),
        "m8191": (8191, 16382, 32),
        "m16383": (16383, 32766, 16),
        "m32767": (32767, 65534, 8),
        "m65535": (65535, 131070, 4),
        "m65536": (65536, 131072, 4),
        "m12_small": (12, 10, 35000),
        "m12_outside": (12, 25, 20000),
    }
    selected = args.cases.split(",")
    modes = [int(x) for x in args.modes.split(",")]
    assert len(modes) >= 2 and len(set(modes)) == len(modes)
    assert all(k in (0, 1, 2) for k in modes), modes
    assert all(k in cases for k in selected), selected
    for key in selected:
        m, n, reps = cases[key]
        project = args.out.parent / f"bench-{key}.sb3"
        write_sb3(project, fixture(m, n, max(1, args.scale*reps)))
    rows = []
    for i in range(args.pairs):
        for key in selected:
            schedule = modes if i % 2 == 0 else list(reversed(modes))
            for mode in schedule:
                value = run(binary, args.out.parent /
                            f"bench-{key}.sb3", mode)
                row = {"pair": i+1, "case": key, "mode": mode, **value}
                rows.append(row)
                args.out.write_text(json.dumps(rows, indent=2)+"\n")
                print("RUN", i+1, key, mode,
                      round(value["median_ms"], 4),
                      value["checksum"], flush=True)
    for key in selected:
        per_mode = {mode: [r for r in rows if r["case"] == key
                          and r["mode"] == mode] for mode in modes}
        assert len({r["checksum"] for v in per_mode.values()
                    for r in v}) == 1, key
        for new_mode in modes:
            for old_mode in modes:
                if new_mode <= old_mode: continue
                old, new = per_mode[old_mode], per_mode[new_mode]
                ratios = [a["median_ms"]/b["median_ms"]
                          for a,b in zip(old,new)]
                print("SUMMARY", key, f"{new_mode}/{old_mode}",
                      "geomean",round(statistics.geometric_mean(ratios),6),
                      "wins",sum(x>1 for x in ratios),"/",len(ratios),
                      "median_old",statistics.median(x["median_ms"] for x in old),
                      "median_new",statistics.median(x["median_ms"] for x in new),
                      flush=True)


if __name__ == "__main__":
    main()
