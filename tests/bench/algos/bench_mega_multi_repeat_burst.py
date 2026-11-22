#!/usr/bin/env python3
"""Paired A/B benchmark of one LLVM call spanning many complete scheduler passes.

Two repeat-yielding Scratch green-flag scripts both remain runnable for N
passes. This benchmark compares bounded multi-Thread bursts ON vs OFF.
"""
import argparse
import hashlib
import json
import math
import os
import re
import shutil
import statistics
import subprocess
import time
import zipfile
from pathlib import Path

from build_mega_yield_fixture import generate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=12)
    parser.add_argument("--count", type=int, default=150000)
    parser.add_argument("--cpu", type=int, default=16)
    parser.add_argument("--shared", action="store_true",
                        help="Both Threads write the same Stage variable")
    parser.add_argument("--two-change", action="store_true",
                        help="Each repeat body contains two in-order numeric writes")
    args = parser.parse_args()
    if args.runs < 1 or args.count < 2:
        parser.error("runs >= 1 and count >= 2 required")
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    binary = args.out / "frozen-xyo"
    shutil.copy2(args.binary.resolve(), binary)
    binary.chmod(binary.stat().st_mode | 0o111)
    project = generate([args.count, args.count],
                       [0.1, 0.2] if args.shared else [1.0, 2.0])
    stage = project["targets"][0]
    if args.shared:
        stage["blocks"]["change1"]["fields"]["VARIABLE"] = ["v0", "v0"]
        del stage["variables"]["v1"]
    if args.two_change:
        for i in range(2):
            first = stage["blocks"][f"change{i}"]
            second_name = f"second{i}"
            second_variable = "v0" if args.shared else f"u{i}"
            if not args.shared:
                stage["variables"][second_variable] = [second_variable, 0]
            first["next"] = second_name
            stage["blocks"][second_name] = {
                "opcode": "data_changevariableby",
                "topLevel": False,
                "next": None,
                "parent": f"change{i}",
                "shadow": False,
                "fields": {"VARIABLE": [second_variable, second_variable]},
                "inputs": {"VALUE": [1, [4, str(0.3 + 0.1*i) if args.shared
                                           else str(3 + i)]]},
            }
    filename = args.out / "multi-repeat.sb3"
    with zipfile.ZipFile(filename, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("project.json", json.dumps(project))
    os.sched_setaffinity(0, {args.cpu})
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_JIT_MODE": "eager",
        "XYO_JIT_OPT_LEVEL": "1",
        "XYO_YIELD_QMODE": "off",
        "XYO_DUMPVARS": "json",
        "XYO_PAR": "0",
        "XYO_JIT_MEGA_BATCH_YIELD": "1",
        "XYO_JIT_MEGA_DIRECT_SCALAR": "1",
        "XYO_JIT_MEGA_SCALAR_BATCH": "1",
        "XYO_JIT_MEGA_MULTI_PC_BATCH": "1",
        "XYO_JIT_MEGA_SINGLE_DISPATCH": "1",
        "XYO_JIT_MEGA_SINGLE_SPECIALIZE": "1",
        "XYO_JIT_MEGA_SINGLE_FAST_SCAN": "1",
        "XYO_JIT_MEGA_SINGLE_PREPROVE": "1",
        "XYO_JIT_MEGA_REPEAT_BURST": "1",
        "XYO_JIT_MEGA_BULK_STATS": "1",
    })
    entries = []
    oracle = None
    for pair in range(1, args.runs + 1):
        for active in ((False, True) if pair % 2 else (True, False)):
            variant = "on" if active else "off"
            process_env = dict(env)
            process_env["XYO_JIT_MEGA_MULTI_REPEAT_BURST"] = "1" if active else "0"
            started = time.perf_counter_ns()
            result = subprocess.run(
                [str(binary), "run", str(filename), "--fps", "0"],
                env=process_env, cwd=args.out, text=True,
                capture_output=True, timeout=120)
            elapsed_ms = (time.perf_counter_ns() - started) / 1e6
            (args.out / f"{pair:02d}-{variant}.out").write_text(result.stdout)
            (args.out / f"{pair:02d}-{variant}.err").write_text(result.stderr)
            match = re.search(r'\{\s*"Stage"\s*:\s*\{.*?\}\s*\}',
                              result.stdout, re.S)
            actual = json.loads(match.group())["Stage"] if match else None
            if result.returncode != 0 or not actual:
                raise RuntimeError(f"{pair} {variant}: exit={result.returncode}, "
                                   f"vars={actual}, err={result.stderr[-1500:]}")
            if args.shared:
                expected = float(actual.get("v0", "nan"))
                if not math.isfinite(expected):
                    raise RuntimeError("Shared variable became nonfinite")
                if oracle is None:
                    oracle = actual
                elif actual != oracle:
                    raise RuntimeError(f"Shared variable mismatch: {actual} != {oracle}")
            else:
                if (float(actual.get("v0", "nan")) != args.count or
                    float(actual.get("v1", "nan")) != 2 * args.count or
                    (args.two_change and (
                        float(actual.get("u0", "nan")) != 3 * args.count or
                        float(actual.get("u1", "nan")) != 4 * args.count))):
                    raise RuntimeError(f"Incorrect independent values: {actual}")
            bulk = re.search(
                r"\[mega-bulk\] calls=(\d+) threads=(\d+)",
                result.stderr)
            if not bulk:
                raise RuntimeError(f"{pair} {variant}: missing mega stats")
            calls, turns = map(int, bulk.groups())
            if turns != 2 * args.count + 2:
                raise RuntimeError(f"Wrong logical turn count: {turns}")
            if active:
                multi = re.search(
                    r"\[mega-multi-burst\] calls=(\d+) turns=(\d+)",
                    result.stderr)
                if not multi:
                    raise RuntimeError(f"{pair} {variant}: missing multi burst stats")
                multi_calls, multi_turns = map(int, multi.groups())
                if multi_turns < args.count or calls > args.count // 4:
                    raise RuntimeError(
                        f"No meaningful batching: calls={calls}, turns={multi_turns}")
            elif calls < args.count:
                raise RuntimeError(f"OFF unexpectedly batch-fused: {calls}")
            print(f"{pair:02d} {variant}: {elapsed_ms:.3f}ms "
                  f"calls={calls} logical_turns={turns}", flush=True)
            entries.append({"pair": pair, "on": active, "ms": elapsed_ms,
                            "calls": calls, "turns": turns})
    ratios = []
    for pair in range(1, args.runs+1):
        off = next(x["ms"] for x in entries
                   if x["pair"] == pair and not x["on"])
        on = next(x["ms"] for x in entries
                  if x["pair"] == pair and x["on"])
        ratios.append(off/on)
    summary = {
        "off_median_ms": statistics.median(
            x["ms"] for x in entries if not x["on"]),
        "on_median_ms": statistics.median(
            x["ms"] for x in entries if x["on"]),
        "paired_geomean_speed_ratio": math.exp(
            statistics.mean(math.log(x) for x in ratios)),
        "on_wins": sum(x > 1 for x in ratios),
        "runs": args.runs,
    }
    report = {
        "sha256_binary": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "sha256_project": hashlib.sha256(filename.read_bytes()).hexdigest(),
        "count": args.count,
        "shared": args.shared,
        "two_change": args.two_change,
        "cpu": args.cpu,
        "summary": summary,
        "results": entries,
    }
    (args.out/"results.json").write_text(json.dumps(report, indent=2) + "\n")
    print("SUMMARY", json.dumps(summary), flush=True)


if __name__ == "__main__":
    main()
