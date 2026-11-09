#!/usr/bin/env python3
"""Paired, isolated ScratchBench Math conditional-SSA signed/unsigned modulo comparison.

The source suite is not edited. Each benchmark is copied unchanged into a
one-kernel project by scratchbench_probe. Checksums must match between modes.
"""
import argparse
import json
import os
import random
import statistics
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
import scratchbench_probe as probe


def main():
    cli = argparse.ArgumentParser()
    cli.add_argument("--binary", type=Path, default=ROOT / "build/xyo")
    cli.add_argument("--pairs", type=int, default=4)
    cli.add_argument("--cases", default="compare,arith,collatz")
    cli.add_argument("--toggle", choices=["urem", "ssa", "i32"], default="urem")
    cli.add_argument("--output", type=Path, default=ROOT / "build/math002-paired.json")
    args = cli.parse_args()
    cases = {
        "compare": ("math_compare", 5000, None),
        "ifelse": ("loops_ifelse", 8000, None),
        "arith": ("math_arith", 5000, None),
        "collatz": ("math_collatz", 3142, None),
        "mandel": ("math_mandel", 1957, None),
    }
    names = args.cases.split(",")
    for name in names:
        if name not in cases:
            cli.error("unknown case " + name)
        probe.CASES[name] = cases[name]
        probe.build(name)
    probe.XYO = args.binary.resolve()
    tasks = [(rep, name, toggle) for rep in range(args.pairs)
             for name in names for toggle in (0, 1)]
    rng = random.Random(20261108)
    results = []
    for rep in range(args.pairs):
        pair = [item for item in tasks if item[0] == rep]
        rng.shuffle(pair)
        for _, name, toggle in pair:
            flags = {
                "XYO_JIT_CONDITIONAL_SSA": str(toggle) if args.toggle == "ssa" else "1",
                "XYO_JIT_CONDITIONAL_UREM": str(toggle) if args.toggle == "urem" else "1",
                "XYO_JIT_CONDITIONAL_I32": str(toggle) if args.toggle == "i32" else "0",
                "XYO_GPU": "0", "XYO_PAR": "0", "XYO_PARFOR": "0",
            }
            result = probe.run_case(name, "xyo", flags)
            if result["exit"] != 0 or not result["done"] or result["rate"] is None:
                raise RuntimeError(f"{name} toggle={toggle}: {result}")
            results.append({"pair": rep, "case": name, "optimized": bool(toggle), **result})
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(results, indent=2) + "\n")
            print(f"{name}: {'ON ' if toggle else 'OFF'} "
                  f"rate={result['rate']:.3f} checksum={result['checksum']}", flush=True)
    for name in names:
        entries = [r for r in results if r["case"] == name]
        if len({r["checksum"] for r in entries}) != 1:
            raise AssertionError(f"checksum disagreement {name}")
        on = {r["pair"]: r["rate"] for r in entries if r["optimized"]}
        off = {r["pair"]: r["rate"] for r in entries if not r["optimized"]}
        ratios = [on[i]/off[i] for i in range(args.pairs)]
        print(f"RESULT {name} median speedup={statistics.median(ratios):.4f}x "
              f"pairs={[round(v,4) for v in ratios]} checksum={entries[0]['checksum']}", flush=True)


if __name__ == "__main__":
    main()
