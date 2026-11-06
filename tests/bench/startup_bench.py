#!/usr/bin/env python3
"""Paired xyo startup benchmark. Run from the xyo-cpp repository root.

Reports elapsed time to the first green-flag thread's spawn, NOT a complete
project execution time. Captures process wall time separately.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import re
import statistics
import subprocess
import time

CASES = ("app_ast", "app_brainfuck", "app_life", "algo_sort",
         "loops_repeat", "vars_setchange")
MARK = re.compile(r"^\[startup\]\s+([\w-]+)\s+([0-9.]+)ms$", re.M)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=Path("build/xyo"))
    parser.add_argument("--projects-dir", type=Path,
                        default=Path("build/scratchbench-cause-probes/projects"))
    parser.add_argument("--trials", type=int, default=10)
    parser.add_argument("--cases", nargs="+", default=list(CASES))
    parser.add_argument("--output", type=Path,
                        default=Path("build/startup-profile/paired.json"))
    args = parser.parse_args()
    binary = args.binary.resolve()
    cpus = os.sched_getaffinity(0)
    cpu = 12 if 12 in cpus else min(cpus)
    binary_sha = hashlib.sha256(binary.read_bytes()).hexdigest()
    print("binary", binary, "sha256", binary_sha, "cpu", cpu, flush=True)
    rows = []
    for case in args.cases:
        project = (args.projects_dir / (case + ".sb3")).resolve()
        if not project.is_file():
            raise FileNotFoundError(project)
        ratios = []
        for trial in range(args.trials):
            pair = {}
            order = [False, True]
            random.Random(1618 + trial * 73 + len(case)).shuffle(order)
            for enabled in order:
                env = {k: v for k, v in os.environ.items()
                       if not k.startswith("XYO_")}
                env.update({
                    "XYO_JIT_FAST_START": str(int(enabled)),
                    "XYO_STARTUP_TIMING": "1",
                    "XYO_GPU": "0", "XYO_PAR": "0",
                    "XYO_PARFOR": "0",
                })
                started = time.perf_counter()
                proc = subprocess.run(
                    ["taskset", "-c", str(cpu), str(binary), "run",
                     str(project), "--jit", "hotspot", "--frames", "1"],
                    env=env, capture_output=True, text=True, timeout=45,
                )
                wall_ms = (time.perf_counter() - started) * 1000
                stages = {k: float(v) for k, v in MARK.findall(proc.stderr)}
                if proc.returncode not in (0, 2) or not {
                        "before-greenflag", "greenflag-spawn",
                        "sb3-parse", "first-step"}.issubset(stages):
                    raise RuntimeError((case, enabled, proc.returncode,
                                        proc.stderr[-900:]))
                pair[enabled] = {
                    "ready_ms": stages["before-greenflag"]
                                + stages["greenflag-spawn"],
                    "parse_ms": stages["sb3-parse"],
                    "spawn_ms": stages["greenflag-spawn"],
                    "first_step_ms": stages["first-step"],
                    "wall_ms": wall_ms,
                    "returncode": proc.returncode,
                }
            ratio = pair[False]["ready_ms"] / pair[True]["ready_ms"]
            ratios.append(ratio)
            rows.append(dict(case=case, trial=trial, slow=pair[False],
                             fast=pair[True], speedup=ratio))
            print(case, trial, "ready ms off/on",
                  round(pair[False]["ready_ms"], 2),
                  round(pair[True]["ready_ms"], 2),
                  "speedup", round(ratio, 2), flush=True)
        case_rows = [r for r in rows if r["case"] == case]
        print("RESULT", case,
              "old_ready", round(statistics.median(
                  r["slow"]["ready_ms"] for r in case_rows), 2),
              "new_ready", round(statistics.median(
                  r["fast"]["ready_ms"] for r in case_rows), 2),
              "speedup", round(statistics.median(ratios), 2),
              "new_total_wall_ms", round(statistics.median(
                  r["fast"]["wall_ms"] for r in case_rows), 2), flush=True)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(rows, indent=2))


if __name__ == "__main__":
    main()
