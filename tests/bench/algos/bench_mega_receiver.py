#!/usr/bin/env python3
"""Paired cold-process A/B for Mega broadcast receiver native continuation.

OFF and ON use the same frozen native binary and Scratch project. Startup,
individual JIT and additional shared LLVM compilation are all included.
"""
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
from decimal import Decimal


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary", type=Path, required=True)
    p.add_argument("--project", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--runs", type=int, default=8)
    p.add_argument("--cpu", type=int, default=16)
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    out = a.out.resolve()
    exe, project = out / "frozen-xyo", out / "source.sb3"
    shutil.copy2(a.binary.resolve(), exe)
    shutil.copy2(a.project.resolve(), project)
    exe.chmod(exe.stat().st_mode | 0o111)
    with zipfile.ZipFile(project) as archive:
        blocks = json.loads(archive.read("project.json"))["targets"][0]["blocks"]
    rounds = int(blocks["sendLoop"]["inputs"]["TIMES"][1][1])
    expected = {"done": Decimal(1)}
    for i in range(3):
        n = Decimal(blocks["repeat" + str(i)]["inputs"]["TIMES"][1][1])
        expected["rx" + str(i)] = Decimal(rounds) * n * (i + 1)
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update(XYO_JIT_MODE="eager", XYO_JIT_OPT_LEVEL="1",
               XYO_YIELD_QMODE="off", XYO_DUMPVARS="json",
               XYO_JIT_MEGA_BATCH_YIELD="1",
               XYO_JIT_MEGA_BATCH_TRACE="1")
    if hasattr(os, "sched_setaffinity"):
        os.sched_setaffinity(0, {a.cpu})
    records = []
    for pair in range(1, a.runs+1):
        for active in ([False, True] if pair % 2 else [True, False]):
            setting = dict(env, XYO_JIT_MEGA_BCAST_RECEIVERS=str(int(active)))
            start = time.perf_counter_ns()
            proc = subprocess.run(
                [str(exe), "run", str(project), "--fps", "0"],
                cwd=out, env=setting, text=True, capture_output=True,
                timeout=120)
            wall = (time.perf_counter_ns() - start) / 1e6
            label = f"{pair:02d}-{'on' if active else 'off'}"
            (out / f"{label}.out").write_text(proc.stdout)
            (out / f"{label}.err").write_text(proc.stderr)
            match = re.search(r'\{\s*"Stage"\s*:\s*\{.*?\}\s*\}',proc.stdout,re.S)
            values = json.loads(match.group())["Stage"] if match else None
            valid = values is not None and all(
                Decimal(str(values.get(k, "NaN"))) == v for k, v in expected.items())
            if proc.returncode or not valid:
                raise RuntimeError(
                    f"{label}: rc={proc.returncode}, state={values},"
                    f"expected={expected}, stderr={proc.stderr[-1200:]}")
            if active and "[mega-receivers] installed=3" not in proc.stderr:
                raise RuntimeError(f"{label}: receiver JIT was not attached")
            if not active and "[mega-receivers]" in proc.stderr:
                raise RuntimeError(f"{label}: receiver batch active while OFF")
            records.append({"pair":pair, "on":active, "wall_ms":wall})
            print(f"{label}: {wall:.3f} ms", flush=True)
    ratios = [
        next(r["wall_ms"] for r in records
             if r["pair"]==pair and not r["on"]) /
        next(r["wall_ms"] for r in records
             if r["pair"]==pair and r["on"])
        for pair in range(1,a.runs+1)
    ]
    summary = {
        "rounds": rounds,
        "off_median_ms": statistics.median(
            r["wall_ms"] for r in records if not r["on"]),
        "on_median_ms": statistics.median(
            r["wall_ms"] for r in records if r["on"]),
        "paired_speed_ratio": math.exp(statistics.mean(math.log(v) for v in ratios)),
        "on_wins": sum(v>1 for v in ratios),
        "pairs": a.runs
    }
    (out / "results.json").write_text(json.dumps({
        "binary_sha256": digest(exe), "project_sha256": digest(project),
        "cpu": a.cpu, "summary": summary, "measurements":records
    },indent=2)+"\n")
    print("SUMMARY", json.dumps(summary), flush=True)


if __name__ == "__main__":
    main()
