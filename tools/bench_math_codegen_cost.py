#!/usr/bin/env python3
"""Measure native JIT launch, compile and peak RSS with Math tables ON/OFF.

This measures the *whole process*, not isolated LLVM compilation. ABBA
groups counter drift; os.wait4 gives per-process peak RSS on Linux.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import statistics
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

PROJECTS = {
    "log": ROOT / "build/math008-log-differential/warp.sb3",
    "exp": ROOT / "build/math008-exp-differential/warp.sb3",
}


def run(binary: Path, project: Path, feature: str, enabled: bool,
        logfile: Path, compare: str) -> dict:
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_DUMPVARS": "json", "XYO_GPU": "0",
        "XYO_PAR": "0", "XYO_PARFOR": "0",
        "XYO_JIT_NUMERIC_REPEAT_FLOAT": "1",
        "XYO_JIT_LOG_TABLE":
            "1" if (compare == "shared" and feature == "log")
            else ("1" if enabled and feature == "log" else "0"),
        "XYO_JIT_LOG_SHARED_TABLE":
            "1" if compare == "shared" and enabled else "0",
        "XYO_JIT_EXP_TABLE": "1" if enabled and feature == "exp" else "0",
        "XYO_JIT_POW10_TABLE": "1",
    })
    with logfile.open("wb") as out, logfile.with_suffix(".err").open("wb") as err:
        start = time.perf_counter()
        child = subprocess.Popen(
            [str(binary), "run", str(project), "--jit", "eager",
             "--frames", "30000"],
            cwd=ROOT, env=env, stdout=out, stderr=err)
        pid, status, usage = os.wait4(child.pid, 0)
        elapsed = time.perf_counter() - start
        assert pid == child.pid
        exit_code = os.waitstatus_to_exitcode(status)
        child.returncode = exit_code
    if exit_code:
        raise RuntimeError(
            f"{feature} enabled={enabled}: exit {exit_code}, "
            f"stderr={logfile.with_suffix('.err').read_text(errors='replace')[-1000:]}"
        )
    output = logfile.read_bytes()
    begin, end = output.index(b"{"), output.rindex(b"}") + 1
    result = json.loads(output[begin:end])["Bench"]
    assert result["done"] == "1"
    digest = hashlib.sha256(
        json.dumps(result, sort_keys=True).encode()).hexdigest()
    return {
        "feature": feature, "enabled": enabled,
        "elapsed_s": elapsed, "maxrss_kib": usage.ru_maxrss,
        "user_cpu_s": usage.ru_utime,
        "system_cpu_s": usage.ru_stime,
        "digest": digest, "exit": exit_code,
    }


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--binary", type=Path,
                   default=ROOT / "build/math009-stable-xyo")
    p.add_argument("--features", default="log,exp")
    p.add_argument("--compare", choices=("table", "shared"), default="table")
    p.add_argument("--groups", type=int, default=4)
    p.add_argument("--output", type=Path,
                   default=ROOT / "build/math009-codegen-cost.json")
    args = p.parse_args()
    binary = args.binary.resolve()
    features = args.features.split(",")
    assert args.groups >= 1
    out = args.output.parent / "math009-codegen-runs"
    out.mkdir(exist_ok=True, parents=True)
    samples = []
    for group in range(args.groups):
        names = features.copy()
        random.Random(20261108 + group).shuffle(names)
        for feature in names:
            project = PROJECTS[feature].resolve()
            pattern = ((False, True, True, False)
                       if group % 2 == 0 else
                       (True, False, False, True))
            for index, enabled in enumerate(pattern):
                log = out / f"{feature}-{group}-{index}.log"
                sample = run(binary, project, feature, enabled, log,
                             args.compare)
                samples.append({**sample, "group": group, "slot": index})
                args.output.parent.mkdir(exist_ok=True, parents=True)
                args.output.write_text(json.dumps(samples, indent=2))
                print(feature, group, index, int(enabled),
                      round(sample["elapsed_s"], 4), "s",
                      round(sample["maxrss_kib"]/1024, 1), "MiB", flush=True)
    for feature in features:
        rows = [r for r in samples if r["feature"] == feature]
        assert len({r["digest"] for r in rows}) == 1
        for enabled in (False, True):
            selected = [r for r in rows if r["enabled"] == enabled]
            print("RESULT", feature, "ON" if enabled else "OFF",
                  "median wall", round(statistics.median(
                      r["elapsed_s"] for r in selected), 4),
                  "median RSS MiB", round(statistics.median(
                      r["maxrss_kib"] for r in selected)/1024, 2),
                  "median CPU seconds", round(statistics.median(
                      r["user_cpu_s"] + r["system_cpu_s"]
                      for r in selected), 3), flush=True)


if __name__ == "__main__":
    main()
