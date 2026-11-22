#!/usr/bin/env python3
"""Measure source-certified broadcast-wait Mega Scheduler against native Fiber.

Alternating OFF/ON pairs on the same frozen binary/SB3 with CPU affinity.
Every run must match the complete Scratch receiver and sender variable state.
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

def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    a = argparse.ArgumentParser(description=__doc__)
    a.add_argument("--binary", type=Path, required=True)
    a.add_argument("--project", type=Path, required=True)
    a.add_argument("--out", type=Path, required=True)
    a.add_argument("--runs", type=int, default=8)
    a.add_argument("--cpu", type=int, default=16)
    args = a.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    exe = out / "frozen-xyo"
    project = out / "source.sb3"
    shutil.copy2(args.binary.resolve(), exe)
    exe.chmod(exe.stat().st_mode | 0o111)
    shutil.copy2(args.project.resolve(), project)
    with zipfile.ZipFile(project) as z:
        source = json.loads(z.read("project.json"))
    blocks = source["targets"][0]["blocks"]
    expected = {"v0": Decimal(10), "v1": Decimal(20)}
    for i in range(2):
        if "recv" + str(i) in blocks:
            n = Decimal(blocks["repeat" + str(i)]["inputs"]["TIMES"][1][1])
            key = "sendLoop" + str(i)
            sends = Decimal(blocks[key]["inputs"]["TIMES"][1][1]) if key in blocks else Decimal(1)
            expected["rx" + str(i)] = n * (i+1) * sends
        else:
            expected["rx" + str(i)] = Decimal(0)
    env = {k:v for k,v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_JIT_MODE": "eager",
        "XYO_JIT_OPT_LEVEL": "1",
        "XYO_YIELD_QMODE": "off",
        "XYO_DUMPVARS": "json",
        "XYO_JIT_MEGA_BATCH_TRACE": "1"
    })
    if hasattr(os, "sched_setaffinity"):
        os.sched_setaffinity(0, {args.cpu})
    timings = []
    for n in range(1, args.runs+1):
        for active in ([False, True] if n%2 else [True, False]):
            flags = dict(env, XYO_JIT_MEGA_BATCH_YIELD=str(int(active)))
            started = time.perf_counter_ns()
            run = subprocess.run(
                [str(exe), "run", str(project), "--fps", "0"],
                cwd=out, env=flags, capture_output=True,
                text=True, timeout=80)
            wall = (time.perf_counter_ns() - started) / 1e6
            label = f"{n:02d}-{'on' if active else 'off'}"
            (out / f"{label}.out").write_text(run.stdout)
            (out / f"{label}.err").write_text(run.stderr)
            m = re.search(r"\{\s*\"Stage\"\s*:\s*\{.*?\}\s*\}", run.stdout, re.S)
            values = json.loads(m.group())["Stage"] if m else None
            valid = values is not None and all(
                Decimal(values.get(k, "NaN")) == val
                for k,val in expected.items())
            if run.returncode or not valid:
                raise RuntimeError(f"{label} rc={run.returncode}, values={values}, expected={expected}\n{run.stderr[-800:]}")
            if active and not re.search(r"\[mega-yield\] slots=2 definitions=1 states=4",run.stderr):
                raise RuntimeError(f"{label} missing LLVM lowering proof: {run.stderr[-1000:]}")
            timings.append({"pair":n, "on":active, "wall_ms":wall})
            print(f"{label}: {wall:.3f}ms", flush=True)
    ratios = [
        next(v["wall_ms"] for v in timings if v["pair"]==n and not v["on"]) /
        next(v["wall_ms"] for v in timings if v["pair"]==n and v["on"])
        for n in range(1, args.runs+1)
    ]
    summary = {
        "off_median_ms": statistics.median(v["wall_ms"] for v in timings if not v["on"]),
        "on_median_ms": statistics.median(v["wall_ms"] for v in timings if v["on"]),
        "paired_ratio": math.exp(statistics.mean(math.log(r) for r in ratios)),
        "on_wins": sum(r>1 for r in ratios),
        "pairs": args.runs
    }
    (out / "results.json").write_text(json.dumps({
        "binary_sha256": digest(exe),
        "project_sha256": digest(project),
        "cpu": args.cpu,
        "summary":summary,
        "timings":timings
    },indent=2)+"\n")
    print("SUMMARY",json.dumps(summary),flush=True)

if __name__ == "__main__":
    main()
