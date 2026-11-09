#!/usr/bin/env python3
"""Paired ScratchBench N-body floating SSA A/B, fixed core, same executable."""
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
    "nbody": ("math_nbody", 2000, None),
    "arith": ("math_arith", 5000, None),
    "expsqrt": ("math_expsqrt", 5000, None),
    "round": ("math_round", 8000, None),
    "trig": ("math_trig", 5000, None),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", type=Path, default=ROOT / "build/xyo")
    ap.add_argument("--cases", default="nbody")
    ap.add_argument("--pairs", type=int, default=8)
    ap.add_argument("--out", type=Path, default=ROOT/"build/math005-paired.json")
    args = ap.parse_args()
    names = args.cases.split(",")
    for name in names:
        probe.CASES[name] = CASES[name]
        probe.build(name)
    probe.XYO = args.binary.resolve()
    rng = random.Random(20261108)
    samples = []
    for pair in range(args.pairs):
        jobs = [(name, enabled) for name in names for enabled in (False, True)]
        rng.shuffle(jobs)
        for name, enabled in jobs:
            env = {
                "XYO_JIT_NUMERIC_REPEAT_SSA": "",
                "XYO_JIT_NUMERIC_REPEAT_FLOAT": "1" if enabled else "0",
                "XYO_GPU": "0", "XYO_PAR": "0", "XYO_PARFOR": "0",
            }
            result = probe.run_case(name, "xyo", env)
            if result["exit"] or not result["done"] or result["rate"] is None:
                raise RuntimeError(f"{name} {enabled}: {result}")
            samples.append({"pair": pair, "case": name, "enabled": enabled,
                            **result})
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_text(json.dumps(samples, indent=2) + "\n")
            print(pair, name, "ON" if enabled else "OFF",
                  f"{result['rate']:.3f}", result["checksum"], flush=True)
    for name in names:
        cases = [r for r in samples if r["case"] == name]
        checks = {r["checksum"] for r in cases}
        assert len(checks) == 1, (name, checks)
        on = {r["pair"]: r["rate"] for r in cases if r["enabled"]}
        off = {r["pair"]: r["rate"] for r in cases if not r["enabled"]}
        ratios = [on[i] / off[i] for i in range(args.pairs)]
        print(f"RESULT {name}: median {statistics.median(ratios):.4f}x "
              f"pairs {[round(r,4) for r in ratios]}, "
              f"checksum={checks.pop()}", flush=True)


if __name__ == "__main__":
    main()
