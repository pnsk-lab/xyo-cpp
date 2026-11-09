#!/usr/bin/env python3
"""Compare flat/adaptive list reads before and after JIT changes."""
import argparse
import json
import os
import statistics
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from bench_list_read import generate
from bench import run_xyo
from gen_algo_bench import write_sb3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--before", type=Path, required=True)
    ap.add_argument("--after", type=Path, required=True)
    ap.add_argument("--reads", type=int, default=20_000_000)
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--cpu", type=int)
    ap.add_argument("--out", type=Path, default=ROOT / "build-list-read-ir/ab.json")
    args = ap.parse_args()
    if args.reads < 1000 or args.reads % 1000:
        ap.error("--reads must be a positive multiple of 1000")
    if args.reps < 1:
        ap.error("--reps must be positive")
    if args.cpu is not None:
        if args.cpu not in os.sched_getaffinity(0):
            ap.error("CPU not in process affinity mask")
        os.sched_setaffinity(0, {args.cpu})
    output = args.out.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    modes = [
        ("before-flat", args.before.resolve(), "flat"),
        ("after-flat", args.after.resolve(), "flat"),
        ("before-adaptive", args.before.resolve(), "adaptive"),
        ("after-adaptive", args.after.resolve(), "adaptive"),
    ]
    report = {"reads": args.reads, "reps": args.reps, "cpu": args.cpu, "rows": {}}
    fixture_dir = output.parent / "fixtures"
    fixture_dir.mkdir(exist_ok=True)
    for kind in ("number_same", "number_varying", "string_same", "string_varying"):
        project, expected = generate(kind, args.reads)
        file = fixture_dir / (kind + ".sb3")
        write_sb3(str(file), project)
        samples = {name: [] for name, _, _ in modes}
        for rep in range(args.reps):
            rotation = rep % len(modes)
            for name, exe, storage in modes[rotation:] + modes[:rotation]:
                os.environ["XYO_LIST_STORAGE"] = storage
                result = run_xyo(str(exe), str(file), "xyo-jit")
                if str(result["check"]) != str(expected):
                    raise RuntimeError(f"{kind}/{name}: {result['check']} != {expected}")
                samples[name].append(statistics.median(result["times"]))
        medians = {name: statistics.median(vals) for name, vals in samples.items()}
        ratios = {
            "flat_after_over_before": medians["before-flat"] / medians["after-flat"],
            "adaptive_after_over_before": medians["before-adaptive"] / medians["after-adaptive"],
            "adaptive_vs_flat_before": medians["before-flat"] / medians["before-adaptive"],
            "adaptive_vs_flat_after": medians["after-flat"] / medians["after-adaptive"],
        }
        report["rows"][kind] = {
            "expected": expected, "medians_ms": medians,
            "ratios": ratios, "samples_ms": samples,
        }
        print(kind, "medians=", medians, "ratios=", ratios, flush=True)
    output.write_text(json.dumps(report, indent=2) + "\n")
    print("saved", output, flush=True)


if __name__ == "__main__":
    main()
