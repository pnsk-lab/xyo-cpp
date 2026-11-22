#!/usr/bin/env python3
"""Paired CPU-pinned A/B for the opt-in greenflag mega-batch switch(pc)."""
import argparse
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

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("--project", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--runs", type=int, default=8)
    ap.add_argument("--cpu", type=int, default=16)
    a = ap.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    binary = out / "frozen-xyo"
    project = out / "source.sb3"
    shutil.copy2(a.binary.resolve(), binary)
    shutil.copy2(a.project.resolve(), project)
    binary.chmod(binary.stat().st_mode | 0o111)
    with zipfile.ZipFile(project) as f:
        source = json.loads(f.read("project.json"))
    variables = source["targets"][0]["variables"]
    expected = {v[0]: str(i + 5) for i, v in enumerate(variables.values())}
    if hasattr(os, "sched_setaffinity"):
        os.sched_setaffinity(0, {a.cpu})
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_JIT_MODE": "eager",
        "XYO_JIT_OPT_LEVEL": "1",
        "XYO_YIELD_QMODE": "off",
        "XYO_DUMPVARS": "json",
        "XYO_JIT_MEGA_BATCH_TRACE": "1",
    })
    trials = []
    for i in range(1, a.runs + 1):
        for enabled in ((False, True) if i % 2 else (True, False)):
            current = dict(env, XYO_JIT_MEGA_BATCH=str(int(enabled)))
            t0 = time.perf_counter_ns()
            proc = subprocess.run(
                [str(binary), "run", str(project), "--fps", "0"],
                env=current, cwd=out, capture_output=True,
                text=True, timeout=60,
            )
            wall = (time.perf_counter_ns() - t0) / 1e6
            label = f"{i:02d}-{'on' if enabled else 'off'}"
            (out / (label + ".err")).write_text(proc.stderr)
            (out / (label + ".out")).write_text(proc.stdout)
            lines = re.search(r"\{[\s\S]*?\n\}", proc.stdout)
            actual = json.loads(lines.group(0))["Stage"] if lines else None
            if proc.returncode or actual != expected:
                raise AssertionError(
                    f"{label} exit={proc.returncode}, state={actual}, "
                    f"expected={expected}; stderr={proc.stderr[-700:]}")
            if enabled and "installed=" not in proc.stderr:
                raise AssertionError(f"{label} mega-batch not installed: {proc.stderr[-700:]}")
            trials.append({"round": i, "on": enabled, "ms": wall})
            print(f"{label}: {wall:.3f}ms", flush=True)
    ratios = [
        next(r["ms"] for r in trials if r["round"] == i and not r["on"]) /
        next(r["ms"] for r in trials if r["round"] == i and r["on"])
        for i in range(1, a.runs + 1)
    ]
    summary = {
        "off_median_ms": statistics.median(r["ms"] for r in trials if not r["on"]),
        "on_median_ms": statistics.median(r["ms"] for r in trials if r["on"]),
        "paired_speed_ratio": math.exp(statistics.mean(map(math.log, ratios))),
        "on_wins": sum(v > 1 for v in ratios),
        "pairs": len(ratios),
    }
    (out / "results.json").write_text(json.dumps({
        "binary_sha256": sha(binary),
        "project_sha256": sha(project), "cpu": a.cpu,
        "summary": summary, "trials": trials
    }, indent=2) + "\n")
    print("SUMMARY", json.dumps(summary), flush=True)

if __name__ == "__main__":
    main()
