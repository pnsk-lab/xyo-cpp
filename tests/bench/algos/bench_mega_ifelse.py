#!/usr/bin/env python3
"""Paired pinned-CPU A/B for finite-number gt/lt/eq Scratch branches.

The same native binary and original SB3 run OFF/ON in alternating order.
Both modes must produce exactly the expected full Scratch Stage variables.
"""
import argparse
from decimal import Decimal
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import time
import zipfile

def file_sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("--project", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--cpu", type=int, default=16)
    ap.add_argument("--runs", type=int, default=8)
    a = ap.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    binary = out / "frozen-xyo"
    project = out / "source.sb3"
    shutil.copy2(a.binary.resolve(), binary)
    shutil.copy2(a.project.resolve(), project)
    binary.chmod(binary.stat().st_mode | 0o111)
    with zipfile.ZipFile(project) as z:
        source = json.loads(z.read("project.json"))
    blocks = source["targets"][0]["blocks"]
    expected = {}
    for k in blocks:
        if k.startswith("repeat"):
            index = k[len("repeat"):]
            n = int(blocks[k]["inputs"]["TIMES"][1][1])
            branch = blocks["branch" + index]
            value = (max(0, 3 - n) if branch["opcode"] == "control_if"
                     else [3, 2, 1, 0][n % 4])
            expected["v" + index] = Decimal(value)
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_JIT_MODE": "eager",
        "XYO_JIT_OPT_LEVEL": "1",
        "XYO_YIELD_QMODE": "off",
        "XYO_DUMPVARS": "json",
        "XYO_JIT_MEGA_BATCH_YIELD": "1",
        "XYO_JIT_MEGA_DIRECT_SCALAR": "1",
        "XYO_JIT_MEGA_SCALAR_BATCH": "1",
        "XYO_JIT_MEGA_MULTI_PC_BATCH": "1",
        "XYO_JIT_MEGA_BULK_STATS": "1",
        "XYO_PAR": "0",
    })
    if hasattr(os, "sched_setaffinity"):
        os.sched_setaffinity(0, {a.cpu})
    measurements = []
    for pair in range(1, a.runs + 1):
        for active in ((False, True) if pair % 2 else (True, False)):
            setting = dict(env, XYO_JIT_MEGA_IF_ELSE_BATCH="1" if active else "0")
            t0 = time.perf_counter_ns()
            p = subprocess.run(
                [str(binary), "run", str(project), "--fps", "0"],
                cwd=out, env=setting, text=True, capture_output=True, timeout=100,
            )
            wall = (time.perf_counter_ns() - t0) / 1e6
            label = f"{pair:02d}-{'on' if active else 'off'}"
            (out / (label + ".out")).write_text(p.stdout)
            (out / (label + ".err")).write_text(p.stderr)
            match = re.search(r"\{\s*\"Stage\"\s*:\s*\{.*?\}\s*\}", p.stdout, re.S)
            actual = json.loads(match.group())["Stage"] if match else None
            valid = actual is not None and all(
                Decimal(actual.get(k, "NaN")) == expected[k] for k in expected
            )
            if p.returncode or not valid:
                raise RuntimeError(
                    f"{label} exit={p.returncode} state={actual} expected={expected}"
                    f"\nstderr={p.stderr[-1000:]}")
            if active:
                stats = re.search(r"\[mega-bulk\] calls=(\d+) threads=(\d+)", p.stderr)
                if not stats or int(stats.group(1)) == 0 or int(stats.group(2)) == 0:
                    raise RuntimeError(f"{label} did not execute any multi-PC LLVM batches")
            elif "[mega-bulk]" in p.stderr:
                raise RuntimeError(f"{label} used a bulk path while disabled")
            measurements.append({"pair": pair, "on": active, "wall_ms": wall})
            print(f"{label} wall={wall:.3f} ms", flush=True)
    ratios = []
    for pair in range(1, a.runs+1):
        off = next(r["wall_ms"] for r in measurements if r["pair"] == pair and not r["on"])
        on = next(r["wall_ms"] for r in measurements if r["pair"] == pair and r["on"])
        ratios.append(off / on)
    summary = {
        "off_median_ms": statistics.median(r["wall_ms"] for r in measurements if not r["on"]),
        "on_median_ms": statistics.median(r["wall_ms"] for r in measurements if r["on"]),
        "paired_speed_ratio": math.exp(statistics.mean(math.log(r) for r in ratios)),
        "on_wins": sum(r > 1 for r in ratios),
        "pairs": a.runs
    }
    (out / "results.json").write_text(json.dumps({
        "binary_sha256": file_sha256(binary),
        "project_sha256": file_sha256(project),
        "summary": summary,
        "cpu": a.cpu,
        "measurements": measurements,
    }, indent=2) + "\n")
    print("SUMMARY", json.dumps(summary), flush=True)

if __name__ == "__main__":
    main()
