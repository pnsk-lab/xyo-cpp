#!/usr/bin/env python3
"""Compare GC allocation and rope-marking strategies without changing SB3 code.

Example:
    python3 tests/bench/algos/bench_gc_allocator_ab.py \
      --binary build/xyo --blocks 8 --cpu 12 \
      --out build/v1-gc-ab.json

Each run pins to one CPU, uses a fresh process and the same Release binary.
The checksum must agree across all four variants before a result is accepted.
"""
from __future__ import annotations

import argparse
import json
import os
import random
import re
import statistics
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
PROJECTS = ROOT / "build" / "scratchbench-cause-probes" / "projects"
CASES = (
    "str_join", "str_cast", "str_letter", "str_contains",
    "list_append", "list_read", "algo_primes", "math_arith",
)
VARIANTS = {
    "baseline": ("0", "0"),
    "run": ("1", "0"),
    "mark": ("0", "1"),
    "combined": ("1", "1"),
}
OUTPUT = re.compile(
    r"^(\w+) \| [^\n|]+ \| [^\n|]+ \| ([\d.]+) \| [^\n|]+ \| (\S+)$",
    re.MULTILINE,
)


def measure(binary: Path, project: Path, case: str, cpu: int,
            variant: str) -> dict:
    run, mark = VARIANTS[variant]
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("XYO_")}
    env["XYO_GC_RUN_ALLOC"] = run
    env["XYO_GC_FAST_CONS_MARK"] = mark
    args = [
        "taskset", "-c", str(cpu), str(binary), "run", str(project),
        "--jit", "hotspot", "--fps", "30", "--print-list", "output",
    ]
    result = subprocess.run(
        args, cwd=ROOT, env=env, capture_output=True, text=True, timeout=90,
    )
    found = OUTPUT.search(result.stdout)
    if (result.returncode != 0 or not result.stdout.rstrip().endswith("DONE")
            or found is None or found.group(1) != case):
        raise RuntimeError(
            f"{case}/{variant} failed ({result.returncode}): "
            f"{result.stderr[-1000:]} {result.stdout[-1000:]}"
        )
    return {
        "case": case,
        "variant": variant,
        "ops_per_ms": float(found.group(2)),
        "checksum": found.group(3),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True,
                        help="Release-built xyo executable (required)")
    parser.add_argument("--projects", type=Path, default=PROJECTS)
    parser.add_argument("--blocks", type=int, default=8)
    parser.add_argument("--cpu", type=int, default=None)
    parser.add_argument("--seed", type=int, default=20261104)
    parser.add_argument("--out", type=Path, default=ROOT / "build" / "gc-ab.json")
    parser.add_argument("--cases", nargs="+", default=list(CASES))
    args = parser.parse_args()
    binary = args.binary.resolve()
    projects = args.projects.resolve()
    if not binary.is_file():
        parser.error(f"missing binary: {binary}")
    if args.blocks < 1:
        parser.error("--blocks must be positive")
    cpus = os.sched_getaffinity(0)
    cpu = args.cpu if args.cpu is not None else min(cpus)
    if cpu not in cpus:
        parser.error(f"CPU {cpu} unavailable (allowed: {sorted(cpus)})")
    for case in args.cases:
        if not (projects / f"{case}.sb3").is_file():
            parser.error(f"missing isolated ScratchBench project: {case}")

    rng = random.Random(args.seed)
    data: list[dict] = []
    args.out.parent.mkdir(parents=True, exist_ok=True)
    for block in range(args.blocks):
        cases = args.cases[:]
        rng.shuffle(cases)
        for case in cases:
            variants = list(VARIANTS)
            rng.shuffle(variants)
            readings = []
            for variant in variants:
                reading = measure(binary, projects / f"{case}.sb3",
                                  case, cpu, variant)
                reading["block"] = block
                data.append(reading)
                readings.append(reading)
            assert len({r["checksum"] for r in readings}) == 1, (
                f"checksum mismatch at block {block}, case {case}: {readings}"
            )
            args.out.write_text(json.dumps(data, indent=2))
            baseline = next(r for r in readings if r["variant"] == "baseline")
            combined = next(r for r in readings if r["variant"] == "combined")
            print(
                f"block={block} case={case} combined/baseline="
                f"{combined['ops_per_ms']/baseline['ops_per_ms']:.4f}",
                flush=True,
            )

    for case in args.cases:
        ratios = {variant: [] for variant in VARIANTS if variant != "baseline"}
        for block in range(args.blocks):
            readings = {
                r["variant"]: r for r in data
                if r["case"] == case and r["block"] == block
            }
            baseline = readings["baseline"]["ops_per_ms"]
            for variant in ratios:
                ratios[variant].append(
                    readings[variant]["ops_per_ms"] / baseline
                )
        print(
            "RESULT", case,
            *(f"{name}={statistics.median(values):.4f}"
              for name, values in ratios.items()),
            flush=True,
        )


if __name__ == "__main__":
    main()
