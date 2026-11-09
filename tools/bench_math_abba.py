#!/usr/bin/env python3
"""ABBA/BAAB ScratchBench Math SSA benchmark with drift diagnostics.

Each four-run group brackets two treatment runs with two control runs.
The ratio uses geometric means. Unstable groups are reported separately
rather than quietly pooled into an inflated apparent speedup.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import random
import statistics
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
import scratchbench_probe as probe

CASES = {
    "arith": ("math_arith", 5000, None),
    "vars_many": ("vars_many", 2000, None),
    "primes": ("algo_primes", 600, None),
    "round": ("math_round", 8000, None),
    "trig": ("math_trig", 5000, None),
    "expsqrt": ("math_expsqrt", 5000, None),
    "nbody": ("math_nbody", 2000, None),
}


def read_cpu_freq(cpu: int) -> int | None:
    path = Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_cur_freq")
    try:
        return int(path.read_text())
    except (OSError, ValueError):
        return None


def summarize(rows: list[dict], threshold: float) -> dict:
    groups = sorted({r["group"] for r in rows})
    good = []
    all_groups = []
    for group in groups:
        records = [r for r in rows if r["group"] == group]
        assert len(records) == 4
        on = [r["rate"] for r in records if r["enabled"]]
        off = [r["rate"] for r in records if not r["enabled"]]
        assert len(on) == len(off) == 2
        speedup = math.sqrt((on[0] / off[0]) * (on[1] / off[1]))
        drift = max(abs(math.log(off[0] / off[1])),
                    abs(math.log(on[0] / on[1])))
        stable = drift <= math.log(1.0 + threshold)
        g = {"group": group, "ratio": speedup, "drift": drift,
             "stable": stable, "on": on, "off": off}
        all_groups.append(g)
        if stable:
            good.append(speedup)
    return {
        "n_groups": len(groups), "n_stable": len(good),
        "median_stable": statistics.median(good) if good else None,
        "median_all": statistics.median(g["ratio"] for g in all_groups),
        "groups": all_groups,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path,
                        default=ROOT / "build/math006-final2-xyo")
    parser.add_argument("--cases", default="round,trig,expsqrt")
    parser.add_argument("--groups", type=int, default=8)
    parser.add_argument("--toggle", choices=("float", "pow10", "log", "exp", "both", "log-shared", "nan-facts", "nan-power", "mod32", "mod-signed", "i32-iter"), default="float",
                        help="JIT feature to A/B: float SSA, pow10 or log table")
    parser.add_argument("--cpu", type=int, default=12)
    parser.add_argument("--max-drift", type=float, default=0.08)
    parser.add_argument("--out", type=Path,
                        default=ROOT / "build/math007-abba.json")
    args = parser.parse_args()
    if args.groups < 1 or not 0 < args.max_drift < 1:
        parser.error("--groups >= 1 and 0 < --max-drift < 1 required")
    names = [name.strip() for name in args.cases.split(",")]
    for name in names:
        if name not in CASES:
            parser.error(f"unsupported case: {name}")
        probe.CASES[name] = CASES[name]
        probe.build(name)
    probe.XYO = args.binary.resolve()
    rows: list[dict] = []
    rng = random.Random(20261108)
    # Rotate case order to avoid systematic thermal/frequency bias.
    for group in range(args.groups):
        order = names[:]
        rng.shuffle(order)
        for name in order:
            pattern = (False, True, True, False) if group % 2 == 0 else (
                True, False, False, True)
            for index, enabled in enumerate(pattern):
                env = {
                    "XYO_JIT_NUMERIC_REPEAT_FLOAT":
                        "1" if args.toggle != "float"
                        else ("1" if enabled else "0"),
                    "XYO_JIT_POW10_TABLE":
                        ("1" if enabled else "0")
                        if args.toggle == "pow10" else "1",
                    "XYO_JIT_LOG_TABLE":
                        ("1" if enabled else "0")
                        if args.toggle in ("log", "both") else
                        ("1" if args.toggle == "log-shared" else "0"),
                    "XYO_JIT_LOG_SHARED_TABLE":
                        ("1" if enabled else "0")
                        if args.toggle == "log-shared" else "0",
                    "XYO_JIT_EXP_TABLE":
                        ("1" if enabled else "0")
                        if args.toggle in ("exp", "both") else "0",
                    "XYO_JIT_NUMERIC_REPEAT_SSA": "",
                    "XYO_JIT_FLOAT_NAN_FACTS":
                        ("1" if enabled else "0")
                        if args.toggle == "nan-facts" else "1",
                    "XYO_JIT_FLOAT_NAN_POWER_FACTS":
                        ("1" if enabled else "0")
                        if args.toggle == "nan-power" else "1",
                    "XYO_JIT_NUMERIC_REPEAT_U32_MOD":
                        ("1" if enabled else "0")
                        if args.toggle == "mod32" else
                        ("1" if args.toggle == "mod-signed" else "0"),
                    "XYO_JIT_NUMERIC_REPEAT_SIGNED_MOD":
                        ("1" if enabled else "0")
                        if args.toggle == "mod-signed" else "0",
                    "XYO_JIT_NUMERIC_REPEAT_I32_ITER":
                        ("1" if enabled else "0")
                        if args.toggle == "i32-iter" else "1",
                    "XYO_GPU": "0", "XYO_PAR": "0", "XYO_PARFOR": "0",
                }
                if args.toggle == "i32-iter":
                    env["XYO_JIT_NUMERIC_REPEAT_U32_MOD"] = "1"
                    env.pop("XYO_JIT_NUMERIC_REPEAT_SIGNED_MOD", None)
                before_freq = read_cpu_freq(args.cpu)
                sample = probe.run_case(name, "xyo", env)
                after_freq = read_cpu_freq(args.cpu)
                if (sample["exit"] != 0 or not sample["done"]
                        or sample["rate"] is None):
                    raise RuntimeError(f"{name} {enabled}: {sample}")
                row = {
                    "name": name, "group": group, "slot": index,
                    "enabled": enabled, "rate": sample["rate"],
                    "checksum": sample["checksum"],
                    "elapsed_s": sample["elapsed_s"],
                    "cpu": args.cpu, "freq_before_khz": before_freq,
                    "freq_after_khz": after_freq,
                }
                rows.append(row)
                args.out.parent.mkdir(parents=True, exist_ok=True)
                args.out.write_text(json.dumps(rows, indent=2) + "\n")
                print(group, name, "ON" if enabled else "OFF",
                      round(sample["rate"], 2), sample["checksum"],
                      flush=True)
            summary = summarize([r for r in rows if r["name"] == name],
                                args.max_drift)
            print("GROUP", name, "ratio",
                  round(summary["groups"][-1]["ratio"], 4),
                  "stable", summary["groups"][-1]["stable"], flush=True)

    print("\nFinal summaries:", flush=True)
    for name in names:
        selected = [r for r in rows if r["name"] == name]
        checksums = {r["checksum"] for r in selected}
        assert len(checksums) == 1, (name, checksums)
        result = summarize(selected, args.max_drift)
        print(name, f"stable {result['n_stable']}/{result['n_groups']}",
              "median_stable", result["median_stable"],
              "median_all", result["median_all"],
              "checksum", next(iter(checksums)), flush=True)


if __name__ == "__main__":
    main()
