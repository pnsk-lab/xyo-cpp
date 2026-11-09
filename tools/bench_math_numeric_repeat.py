#!/usr/bin/env python3
"""Paired ScratchBench pure-number repeat SSA measurements, with checksums."""
import argparse
import json
import random
import statistics
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
import scratchbench_probe as probe

CASES = {
    "arith": ("math_arith", 5000, None),
    "nbody": ("math_nbody", 2000, None),
    "mandel": ("math_mandel", 1957, None),
    "collatz": ("math_collatz", 3142, None),
    "compare": ("math_compare", 5000, None),
    "many": ("vars_many", 2000, None),
}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", type=Path, default=ROOT / "build/xyo")
    ap.add_argument("--cases", default="arith,nbody")
    ap.add_argument("--pairs", type=int, default=4)
    ap.add_argument("--toggle", choices=["integer", "ssa"], default="integer")
    ap.add_argument("--out", type=Path, default=ROOT / "build/math003-paired.json")
    args = ap.parse_args()
    names = args.cases.split(",")
    for name in names:
        probe.CASES[name] = CASES[name]
        probe.build(name)
    probe.XYO = args.binary.resolve()
    rng = random.Random(20261108)
    records = []
    for pair in range(args.pairs):
        jobs = [(name, on) for name in names for on in (False, True)]
        rng.shuffle(jobs)
        for name, on in jobs:
            env = {
                "XYO_JIT_NUMERIC_REPEAT_SSA":
                    "1" if args.toggle != "ssa" else ("1" if on else "0"),
                "XYO_JIT_NUMERIC_REPEAT_INT":
                    ("1" if on else "0") if args.toggle == "integer" else "1",
                "XYO_GPU": "0", "XYO_PAR": "0", "XYO_PARFOR": "0"
            }
            result = probe.run_case(name, "xyo", env)
            if result["exit"] != 0 or not result["done"] or result["rate"] is None:
                raise RuntimeError(f"{name} {on}: {result}")
            records.append({"pair": pair, "name": name, "enabled": on, **result})
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_text(json.dumps(records, indent=2) + "\n")
            print(f"{pair} {name:10} {'ON ' if on else 'OFF'} "
                  f"{result['rate']:.2f} checksum {result['checksum']}", flush=True)
    for name in names:
        rows = [r for r in records if r["name"] == name]
        checks = {r["checksum"] for r in rows}
        assert len(checks) == 1, (name, checks)
        on = {r["pair"]: r["rate"] for r in rows if r["enabled"]}
        off = {r["pair"]: r["rate"] for r in rows if not r["enabled"]}
        ratios = [on[i]/off[i] for i in range(args.pairs)]
        print(f"RESULT {name}: median ratio {statistics.median(ratios):.4f} "
              f"pairs {[round(x,4) for x in ratios]} checksum {checks.pop()}",flush=True)

if __name__ == "__main__":
    main()
