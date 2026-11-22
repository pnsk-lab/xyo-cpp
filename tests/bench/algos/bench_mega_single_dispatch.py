#!/usr/bin/env python3
"""Paired ON/OFF test of one-survivor LLVM Mega dispatch.

Two independent green-flag repeat scripts share one multi-PC dispatcher.
One finishes after the first turn; the other runs long enough that nearly
every subsequent scheduler pass contains exactly one live Mega Thread.
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
    parser.add_argument("--runs", type=int, default=8)
    parser.add_argument("--count", type=int, default=120000)
    parser.add_argument("--cpu", type=int, default=16)
    parser.add_argument(
        "--specialize", action="store_true",
        help="Compare single-item LLVM specialization with generic count=1; "
             "both retain single-Thread batch dispatch")
    parser.add_argument(
        "--fast-scan", action="store_true",
        help="Compare early last-Thread scalar cache probe with general "
             "batch scanner; both use specialized LLVM single-item entry")
    parser.add_argument(
        "--preprove", action="store_true",
        help="Compare compiler-proved source metadata against general "
             "source metadata scanning; fast-scan enabled in both variants")
    parser.add_argument(
        "--burst", action="store_true",
        help="Compare bounded LLVM repeat bursts with one native call "
             "per scheduler turn (all other single-Thread optimizations ON)")
    args = parser.parse_args()
    if sum((args.specialize, args.fast_scan, args.preprove, args.burst)) > 1:
        parser.error("--specialize, --fast-scan, --preprove, and --burst "
                     "are mutually exclusive")
    if args.runs < 1 or args.count < 2:
        parser.error("--runs must be >= 1 and --count >= 2")
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    binary = args.out / "frozen-xyo"
    shutil.copy2(args.binary.resolve(), binary)
    binary.chmod(binary.stat().st_mode | 0o111)
    project = args.out / "single-survivor.sb3"
    with zipfile.ZipFile(project, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("project.json", json.dumps(
            generate([args.count, 1], [1.0, 2.0])))
    os.sched_setaffinity(0, {args.cpu})
    base = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    base.update({
        "XYO_JIT_MODE": "eager",
        "XYO_JIT_OPT_LEVEL": "1",
        "XYO_YIELD_QMODE": "off",
        "XYO_DUMPVARS": "json",
        "XYO_PAR": "0",
        "XYO_JIT_MEGA_BATCH_YIELD": "1",
        "XYO_JIT_MEGA_DIRECT_SCALAR": "1",
        "XYO_JIT_MEGA_SCALAR_BATCH": "1",
        "XYO_JIT_MEGA_MULTI_PC_BATCH": "1",
        "XYO_JIT_MEGA_BULK_STATS": "1",
    })
    if args.specialize or args.fast_scan or args.preprove or args.burst:
        base["XYO_JIT_MEGA_SINGLE_DISPATCH"] = "1"
    if args.fast_scan or args.preprove or args.burst:
        base["XYO_JIT_MEGA_SINGLE_SPECIALIZE"] = "1"
    if args.preprove or args.burst:
        base["XYO_JIT_MEGA_SINGLE_FAST_SCAN"] = "1"
    if args.burst:
        base["XYO_JIT_MEGA_SINGLE_PREPROVE"] = "1"
    toggle = ("XYO_JIT_MEGA_REPEAT_BURST" if args.burst else
              "XYO_JIT_MEGA_SINGLE_PREPROVE" if args.preprove else
              "XYO_JIT_MEGA_SINGLE_FAST_SCAN" if args.fast_scan else
              "XYO_JIT_MEGA_SINGLE_SPECIALIZE" if args.specialize else
              "XYO_JIT_MEGA_SINGLE_DISPATCH")
    results = []
    for pair in range(1, args.runs + 1):
        for active in ( (False, True) if pair % 2 else (True, False) ):
            variant = "on" if active else "off"
            env = dict(base)
            env[toggle] = "1" if active else "0"
            before = time.perf_counter_ns()
            proc = subprocess.run(
                [str(binary), "run", str(project), "--fps", "0"],
                env=env, cwd=args.out, text=True,
                capture_output=True, timeout=120)
            ms = (time.perf_counter_ns() - before) / 1e6
            (args.out / f"{pair:02d}-{variant}.out").write_text(proc.stdout)
            (args.out / f"{pair:02d}-{variant}.err").write_text(proc.stderr)
            match = re.search(r'\{\s*"Stage"\s*:\s*\{.*?\}\s*\}', proc.stdout, re.S)
            actual = json.loads(match.group()).get("Stage") if match else None
            if proc.returncode or not actual or float(actual.get("v0", "nan")) != args.count or float(actual.get("v1", "nan")) != 2:
                raise RuntimeError(f"incorrect {pair} {variant}: exit={proc.returncode}, vars={actual}, err={proc.stderr[-1000:]}")
            stats = re.search(r'\[mega-bulk\] calls=(\d+) threads=(\d+)', proc.stderr)
            if not stats:
                raise RuntimeError(f"missing Mega bulk stats {pair} {variant}")
            calls, threads = map(int, stats.groups())
            if args.burst:
                if active:
                    burst = re.search(
                        r'\[mega-repeat-burst\] calls=(\d+) turns=(\d+)',
                        proc.stderr)
                    if not burst:
                        raise RuntimeError(
                            f"missing LLVM burst stats in {pair} {variant}")
                    burst_calls, burst_turns = map(int, burst.groups())
                    if calls >= args.count // 8 or burst_turns < args.count - 256:
                        raise RuntimeError(
                            f"LLVM bursts failed: calls={calls}, turns={burst_turns}")
                elif calls <= args.count - 3:
                    raise RuntimeError(
                        f"OFF must use per-turn native dispatch: {calls}")
            else:
                if active and calls <= args.count - 3:
                    raise RuntimeError(f"single LLVM dispatch not used: {calls} calls")
                if not active and not (args.specialize or args.fast_scan or args.preprove) and calls > 2:
                    raise RuntimeError(f"OFF unexpectedly used single-item batches: {calls} calls")
                if not active and (args.specialize or args.fast_scan or args.preprove) and calls <= args.count - 3:
                    raise RuntimeError(f"OFF baseline did not batch single item: {calls} calls")
            result = {"pair":pair, "on":active, "ms":ms, "calls":calls, "threads":threads}
            results.append(result)
            print(f"{pair:02d} {variant}: {ms:.3f}ms, calls={calls}, threads={threads}", flush=True)
    ratios=[]
    for pair in range(1, args.runs+1):
        off=next(x["ms"] for x in results if x["pair"]==pair and not x["on"])
        on=next(x["ms"] for x in results if x["pair"]==pair and x["on"])
        ratios.append(off/on)
    summary={
        "off_median_ms":statistics.median(x["ms"] for x in results if not x["on"]),
        "on_median_ms":statistics.median(x["ms"] for x in results if x["on"]),
        "paired_geomean_speed_ratio":math.exp(statistics.mean(map(math.log,ratios))),
        "on_wins":sum(x>1 for x in ratios),
        "runs":args.runs,
    }
    report={"sha256_binary":hashlib.sha256(binary.read_bytes()).hexdigest(),
            "sha256_project":hashlib.sha256(project.read_bytes()).hexdigest(),
            "count":args.count,"cpu":args.cpu,
            "toggle":toggle,"summary":summary,"results":results}
    (args.out/"results.json").write_text(json.dumps(report,indent=2)+"\n")
    print("SUMMARY",json.dumps(summary),flush=True)


if __name__=="__main__":
    main()
