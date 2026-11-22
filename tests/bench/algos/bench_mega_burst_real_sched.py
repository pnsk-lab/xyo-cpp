#!/usr/bin/env python3
"""Same-binary paired A/B on the unchanged ScratchBench eight sched_* workloads.

Only non-scheduler suite *launch calls* are removed from a temporary sb3.
Scheduler source procedures, their arguments, timing loops, and output
checksums are kept untouched. The time-dependent sched checksums are not
expected to match across runs. This does not claim any general win.
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

from bench_scheduler import scheduler_project


FLAGS = (
    "XYO_JIT_MEGA_BATCH_YIELD",
    "XYO_JIT_MEGA_DIRECT_SCALAR",
    "XYO_JIT_MEGA_SCALAR_BATCH",
    "XYO_JIT_MEGA_MULTI_PC_BATCH",
    "XYO_JIT_MEGA_SINGLE_DISPATCH",
    "XYO_JIT_MEGA_SINGLE_SPECIALIZE",
    "XYO_JIT_MEGA_SINGLE_FAST_SCAN",
    "XYO_JIT_MEGA_SINGLE_PREPROVE",
    "XYO_JIT_MEGA_REPEAT_BURST",
)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", required=True, type=Path)
    ap.add_argument("--project", type=Path, default=Path("scratch-bench/.cache/build/performance.sb3"))
    ap.add_argument("--prepared-project", type=Path,
                    help="Reuse an already isolated sched-8.sb3 verbatim; "
                         "avoids dependence on scratch-bench temporary caches")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--pairs", type=int, default=4)
    ap.add_argument("--cpu", type=int, default=16)
    ap.add_argument("--jit", choices=("eager", "hotspot"), default="eager")
    ap.add_argument("--fps", type=int, default=30)
    args = ap.parse_args()
    if args.pairs < 1:
        ap.error("--pairs must be >=1")
    if args.cpu is not None:
        os.sched_setaffinity(0, {args.cpu})
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    binary = args.out / "frozen-xyo"
    shutil.copy2(args.binary.resolve(), binary)
    binary.chmod(binary.stat().st_mode | 0o111)
    project = args.out / "sched-8.sb3"
    if args.prepared_project:
        shutil.copy2(args.prepared_project.resolve(), project)
    else:
        scheduler_project(args.project.resolve(), project)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("XYO_")}
    env.update({flag: "1" for flag in FLAGS})
    env.update({
        "XYO_JIT_MODE": args.jit,
        "XYO_JIT_OPT_LEVEL": "1",
        "XYO_JIT_MEGA_BULK_STATS": "1",
        "XYO_PAR": "0",
        "XYO_YIELD_QMODE": "off",
    })
    samples = []
    for pair in range(1, args.pairs + 1):
        for active in ((False, True) if pair % 2 else (True, False)):
            label = "on" if active else "off"
            run_env = dict(env)
            run_env["XYO_JIT_MEGA_MULTI_REPEAT_BURST"] = "1" if active else "0"
            completed = subprocess.run([
                str(binary), "run", str(project), "--fps", str(args.fps),
                "--print-list", "output",
            ], env=run_env, cwd=args.out,
                capture_output=True, text=True, timeout=180)
            (args.out / f"{pair:02d}-{label}.out").write_text(completed.stdout)
            (args.out / f"{pair:02d}-{label}.err").write_text(completed.stderr)
            if completed.returncode != 0:
                raise RuntimeError(
                    f"bad exit ({pair} {label}): {completed.returncode}; "
                    f"{completed.stderr[-3000:]}")
            output = completed.stdout.splitlines()
            if not any(line == "DONE" for line in output):
                raise RuntimeError(f"ScratchBench did not finish: {pair} {label}")
            rows = {}
            for line in output:
                columns = [value.strip() for value in line.split("|")]
                if len(columns) == 6 and columns[0].startswith("sched_"):
                    rows[columns[0]] = {
                        "ops_per_ms": float(columns[3]),
                        "checksum": columns[5],
                    }
            if len(rows) != 8:
                raise RuntimeError(f"Found {len(rows)} scheduler cases: {sorted(rows)}")
            bursts = re.search(
                r"\[mega-multi-burst\] calls=(\d+) turns=(\d+)",
                completed.stderr)
            bulk = re.search(
                r"\[mega-bulk\] calls=(\d+) threads=(\d+)",
                completed.stderr)
            sample = {
                "pair": pair, "on": active, "rows": rows,
                "multi_burst_calls": int(bursts.group(1)) if bursts else 0,
                "multi_burst_turns": int(bursts.group(2)) if bursts else 0,
                "mega_bulk_calls": int(bulk.group(1)) if bulk else 0,
            }
            samples.append(sample)
            print(f"{pair:02d} {label}: multi_burst={sample['multi_burst_calls']} "
                  + " ".join(f"{key}={value['ops_per_ms']:.2f}"
                             for key, value in rows.items()), flush=True)
    keys = sorted(samples[0]["rows"])
    summary = {}
    for key in keys:
        off = [sample["rows"][key]["ops_per_ms"]
               for sample in samples if not sample["on"]]
        on = [sample["rows"][key]["ops_per_ms"]
              for sample in samples if sample["on"]]
        ratios = [a/b for a,b in zip(on, off)]
        summary[key] = {
            "off_median": statistics.median(off),
            "on_median": statistics.median(on),
            "paired_geomean_ratio": math.exp(statistics.mean(
                math.log(ratio) for ratio in ratios)),
            "on_wins": sum(ratio > 1 for ratio in ratios),
        }
    report = {
        "sha256_binary": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "sha256_project": hashlib.sha256(project.read_bytes()).hexdigest(),
        "pairs": args.pairs, "cpu": args.cpu, "jit": args.jit,
        "fps": args.fps, "summary": summary, "samples": samples,
    }
    (args.out/"results.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False)+"\n")
    print("SUMMARY", json.dumps(summary, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
