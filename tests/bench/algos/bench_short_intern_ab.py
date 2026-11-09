#!/usr/bin/env python3
"""Interleaved short-intern on/off A/B using generated Scratch SB3s."""
import argparse
import json
from pathlib import Path
import random
import statistics
import sys

from bench_scratch_string_storage import generate, run_one, ROOT
from gen_algo_bench import write_sb3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", type=Path, default=ROOT / "build-list-epoch/xyo")
    ap.add_argument("--output", type=Path,
                    default=ROOT / "build-list-epoch/short-intern-ab-interleaved")
    ap.add_argument("--cases", default="short_join,append,branch,contains")
    ap.add_argument("--leaf", choices=["utf16", "latin1"], default="utf16")
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--multiplier", type=int, default=8)
    args = ap.parse_args()
    if args.reps < 1 or args.multiplier < 1:
        ap.error("reps and multiplier must be >= 1")
    args.binary = args.binary.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    rng = random.Random(20261107)
    report = {"config": {"leaf": args.leaf, "reps": args.reps,
                         "multiplier": args.multiplier,
                         "jit": "eager", "transient": False,
                         "random_seed": 20261107},
              "rows": {}}
    for case in args.cases.split(","):
        file = args.output / (case + ".sb3")
        write_sb3(file, generate(case, args.multiplier))
        samples = {(storage, enabled): [] for storage in
                   ("rope", "flat", "append-log") for enabled in (False, True)}
        for rep in range(args.reps):
            order = list(samples)
            rng.shuffle(order)
            for storage, enabled in order:
                r = run_one(args.binary, file, storage, "eager", False,
                            leaf=args.leaf, short_intern=enabled)
                samples[(storage, enabled)].append(r["median_ms"])
        row = {storage: {
            ("on" if enabled else "off"): {
                "median_ms": statistics.median(samples[(storage, enabled)]),
                "samples_ms": samples[(storage, enabled)],
            } for enabled in (False, True)
        } for storage in ("rope", "flat", "append-log")}
        report["rows"][case] = row
        print(case, " ".join(
            f"{storage}: {row[storage]['off']['median_ms']:.1f} -> "
            f"{row[storage]['on']['median_ms']:.1f} ms"
            for storage in ("rope", "flat", "append-log")), flush=True)
        (args.output / "results.json").write_text(
            json.dumps(report, indent=2, ensure_ascii=False) + "\n")


if __name__ == "__main__":
    main()
