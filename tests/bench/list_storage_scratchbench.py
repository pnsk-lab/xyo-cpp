#!/usr/bin/env python3
"""A/B the selected SList storage modes through upstream scratch-bench.

Requires an existing local scratch-bench checkout with Bun dependencies.
Each mode runs the same xyo binary, project and JIT mode. Scores are ratios of
geometric-mean ops/ms against flat, not the site's weighted total score.
"""
import argparse
import json
import math
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
MODES = ("flat", "sbo4", "sbo8", "flat15", "deque", "gap", "chunked", "adaptive")


def geometric_mean(xs):
    return math.exp(sum(math.log(x) for x in xs) / len(xs)) if xs else float("nan")


def read_scores(folder):
    files = list(folder.glob("performance-xyo-cpp*.json"))
    if len(files) != 1:
        raise RuntimeError(f"expected one result in {folder}, got {files}")
    data = json.loads(files[0].read_text())
    if data.get("error"):
        raise RuntimeError(f"{folder}: {data['error']}")
    if not any(run.get("ok") for run in data["runs"]):
        raise RuntimeError(f"{folder}: no successful run")
    return {
        name: entry["ops_per_ms"]["median"]
        for name, entry in data["results"].items()
        if entry.get("ops_per_ms") and entry["ops_per_ms"]["median"] > 0
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bench", type=Path, default=ROOT / "scratch-bench")
    parser.add_argument("--binary", type=Path, default=ROOT / "build/xyo")
    parser.add_argument("--out", type=Path,
                        default=ROOT / "scratch-bench/results/list-storage-20261107")
    parser.add_argument("--runs", type=int, default=2)
    parser.add_argument("--modes", default=",".join(MODES))
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    args.out = args.out.resolve()
    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error(f"binary not found: {binary}")
    modes = [s.strip() for s in args.modes.split(",") if s.strip()]
    if not modes or any(mode not in MODES for mode in modes):
        parser.error(f"modes must be from {MODES}")
    scores = {}
    for mode in modes:
        out = args.out / mode
        out.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, XYO_BIN=str(binary), XYO_LIST_STORAGE=mode)
        cmd = ["bun", "run", "bench", "--runtime", "xyo-cpp",
               "--suite", "performance", "--runs", str(args.runs),
               "--no-variants", "--offline", "--out", str(out)]
        print(f"[{mode}] {' '.join(cmd)}", flush=True)
        logpath = out / "console.log"
        with logpath.open("w") as log:
            process = subprocess.run(cmd, cwd=args.bench, env=env,
                                     stdout=log, stderr=subprocess.STDOUT,
                                     check=False)
        if process.returncode:
            print(logpath.read_text()[-6000:], file=sys.stderr)
            raise RuntimeError(f"{mode} failed (exit {process.returncode})")
        scores[mode] = read_scores(out)
        print(f"[{mode}] {len(scores[mode])} workloads", flush=True)
    common = set.intersection(*(set(v) for v in scores.values()))
    if "flat" not in scores:
        raise RuntimeError("include flat as the baseline")
    list_cases = sorted(x for x in common if x.startswith("list_"))
    summary = {}
    print("backend   all-relative   lists-relative   " +
          " ".join(list_cases), flush=True)
    for mode in modes:
        ratios = {name: scores[mode][name] / scores["flat"][name]
                  for name in sorted(common)}
        row = {
            "all_geomean_vs_flat": geometric_mean(list(ratios.values())),
            "lists_geomean_vs_flat": geometric_mean([ratios[x] for x in list_cases]),
            "list_ratios": {x: ratios[x] for x in list_cases},
            "ops_per_ms": scores[mode],
        }
        summary[mode] = row
        print(f"{mode:8s} {row['all_geomean_vs_flat']:11.4f}x"
              f" {row['lists_geomean_vs_flat']:15.4f}x  " +
              " ".join(f"{ratios[x]:.3f}x" for x in list_cases), flush=True)
    (args.out / "summary.json").write_text(json.dumps({
        "binary": str(binary), "runs": args.runs, "modes": modes,
        "common_workloads": sorted(common), "summary": summary,
    }, indent=2) + "\n")
    print(f"wrote {args.out / 'summary.json'}")


if __name__ == "__main__":
    main()
