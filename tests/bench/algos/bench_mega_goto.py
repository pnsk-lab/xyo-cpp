#!/usr/bin/env python3
"""Paired A/B benchmark for opt-in LLVM mega-goto custom-block fusion."""
from __future__ import annotations
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

def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", required=True, type=Path)
    ap.add_argument("--project", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--expected", type=str, required=True)
    ap.add_argument("--runs", type=int, default=8)
    ap.add_argument("--cpu", type=int, default=16)
    ap.add_argument("--inline-limit", type=int, default=0)
    ap.add_argument("--opt-level", type=int, default=1)
    ap.add_argument("--timeout", type=int, default=45)
    args = ap.parse_args()
    if args.runs < 1:
        ap.error("--runs must be positive")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    frozen_binary = out / "frozen-xyo"
    frozen_project = out / "source.sb3"
    shutil.copy2(args.binary.resolve(), frozen_binary)
    shutil.copy2(args.project.resolve(), frozen_project)
    frozen_binary.chmod(frozen_binary.stat().st_mode | 0o111)
    if hasattr(os, "sched_setaffinity"):
        os.sched_setaffinity(0, {args.cpu})
    env_base = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env_base.update({
        "XYO_JIT_MODE": "eager",
        "XYO_JIT_OPT_LEVEL": str(args.opt_level),
        "XYO_JIT_INLINE_LIMIT": str(args.inline_limit),
        "XYO_YIELD_QMODE": "off",
        "XYO_DUMPVARS": "json",
        "XYO_JIT_MEGA_TRACE": "1",
    })
    measurements = []
    for round_num in range(1, args.runs + 1):
        for enabled in ([False, True] if round_num % 2 else [True, False]):
            state = "mega" if enabled else "normal"
            env = dict(env_base, XYO_JIT_MEGA_DISPATCH="1" if enabled else "0")
            started = time.perf_counter_ns()
            run = subprocess.run(
                [str(frozen_binary), "run", str(frozen_project), "--fps", "0"],
                cwd=out, env=env, capture_output=True, text=True, timeout=args.timeout
            )
            wall_ms = (time.perf_counter_ns() - started) / 1e6
            label = f"{round_num:02d}-{state}"
            (out / f"{label}.stdout").write_text(run.stdout)
            (out / f"{label}.stderr").write_text(run.stderr)
            value = re.search(r'"s"\s*:\s*"([^"]+)"', run.stdout)
            actual = value.group(1) if value else None
            if run.returncode or actual != args.expected:
                raise RuntimeError(
                    f"{label}: exit={run.returncode}, s={actual}, "
                    f"expected={args.expected}: {run.stderr[-700:]}")
            if enabled and not re.search(
                    r"\[mega-goto\] .*fused=1 .*remaining=0 reason=fused",
                    run.stderr):
                raise RuntimeError(f"{label}: mega goto was not emitted: {run.stderr[-700:]}")
            measurements.append({
                "round": round_num, "variant": state,
                "wall_ms": wall_ms, "s": actual
            })
            print(f"{label} wall={wall_ms:.3f} ms s={actual}", flush=True)
    ratios = []
    for round_num in range(1, args.runs + 1):
        normal = next(r["wall_ms"] for r in measurements
                      if r["round"] == round_num and r["variant"] == "normal")
        mega = next(r["wall_ms"] for r in measurements
                    if r["round"] == round_num and r["variant"] == "mega")
        ratios.append(normal / mega)
    normal = [r["wall_ms"] for r in measurements if r["variant"] == "normal"]
    mega = [r["wall_ms"] for r in measurements if r["variant"] == "mega"]
    summary = {
        "normal_median_ms": statistics.median(normal),
        "mega_median_ms": statistics.median(mega),
        "paired_geometric_speedup": math.exp(statistics.mean(math.log(x) for x in ratios)),
        "mega_wins": sum(x > 1 for x in ratios),
        "pairs": args.runs,
    }
    result = {
        "binary_sha256": sha(frozen_binary),
        "project_sha256": sha(frozen_project),
        "common_env": {k: v for k, v in env_base.items() if k.startswith("XYO_")},
        "cpu": args.cpu, "summary": summary, "measurements": measurements
    }
    (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print("SUMMARY", json.dumps(summary), flush=True)

if __name__ == "__main__":
    main()
