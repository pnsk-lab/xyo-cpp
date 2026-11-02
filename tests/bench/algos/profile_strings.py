#!/usr/bin/env python3
"""Capture xyo string diagnostics separately from uninstrumented SB3 timing.

Uses existing SB3s, verifies their project.json against bench_strings.generate,
records exact binary/project hashes, and retains complete stdout/stderr logs.
RTProf timings are inclusive and instrumented; never subtract them from baseline.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import time
import zipfile

from bench import parse_xyo
from bench_strings import generate


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--xyo", default="build/xyo")
    ap.add_argument("--projects", default="build/string-perf")
    ap.add_argument("--out", required=True)
    ap.add_argument("--scale", type=int, default=80)
    ap.add_argument("--cpu", type=int, default=12)
    ap.add_argument("--kinds", default="join,join_observe,join_observe_last")
    args = ap.parse_args()
    if args.scale < 1:
        ap.error("scale must be positive")
    os.sched_setaffinity(0, {args.cpu})
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=False)
    xyo = Path(args.xyo).resolve()
    summary = {
        "binary": str(xyo), "binary_sha256": digest(xyo),
        "platform": platform.platform(), "cpu": args.cpu, "scale": args.scale,
        "scope": "one warmup set + three measured calls + final verification; diagnostics are process-wide",
        "warning": "RTProf adds timers, atomic counters and a name-map mutex; its inclusive times overlap GC and are not baseline cost fractions.",
        "workloads": {},
    }
    base_env = dict(os.environ)
    # Avoid silently inheriting diagnostic, JIT, danger or GC settings.
    for key in list(base_env):
        if key.startswith("XYO_"):
            del base_env[key]
    modes = {"plain": {}, "counts": {
        "XYO_STR_DEBUG": "1", "XYO_GC_DEBUG": "1", "XYO_HOT_STATS": "1"},
        "rtprof": {"XYO_PROF": "1", "XYO_STR_DEBUG": "1", "XYO_GC_DEBUG": "1"}}
    for kind in args.kinds.split(","):
        path = (Path(args.projects) / (kind + ".sb3")).resolve()
        project, expected = generate(kind, args.scale)
        with zipfile.ZipFile(path) as z:
            raw_project = z.read("project.json")
            if json.loads(raw_project) != project:
                raise RuntimeError(f"{path}: does not match generated {kind} scale={args.scale}")
        row = {"project": str(path), "sb3_sha256": digest(path),
               "project_json_sha256": hashlib.sha256(raw_project).hexdigest(),
               "expected_check": expected, "modes": {}}
        for mode, settings in modes.items():
            env = dict(base_env, XYO_DUMPVARS="json", **settings)
            command = [str(xyo), "run", str(path), "--jit", "hotspot"]
            start = time.monotonic()
            p = subprocess.run(command, env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, timeout=300)
            wall_ms = (time.monotonic() - start) * 1000
            stem = out / f"{kind}-{mode}"
            stem.with_suffix(".stdout.txt").write_bytes(p.stdout)
            stem.with_suffix(".stderr.txt").write_bytes(p.stderr)
            if p.returncode:
                raise RuntimeError(f"{kind}/{mode}: exit {p.returncode}; see {stem}.stderr.txt")
            parsed = parse_xyo(p.stdout)
            if str(parsed["check"]) != expected:
                raise RuntimeError(f"{kind}/{mode}: wrong check {parsed['check']} != {expected}")
            stderr = p.stderr.decode("utf-8", "replace")
            counters = re.search(r"\[str\] cons=(\d+) flatten=(\d+)", stderr)
            gc = re.findall(r"\[gc\] collect #(\d+) live=([\d.]+)MB objs=(\d+) total=([\d.]+)ms pool=([\d.]+)MB", stderr)
            functions = dict((name, int(count)) for name, count in re.findall(r"\[fn\] (\S+) (\d+)", stderr))
            record = {"command": command, "settings": settings, "exit_code": p.returncode,
                      "wall_ms": wall_ms, **parsed,
                      "median_in_project_ms": statistics.median(parsed["times"]),
                      "str": {"cons": int(counters[1]), "flatten": int(counters[2])} if counters else None,
                      "gc_collections_logged": len(gc),
                      "gc_last": {"sequence": int(gc[-1][0]), "live_mb": float(gc[-1][1]),
                                  "objects": int(gc[-1][2]), "cumulative_ms": float(gc[-1][3]),
                                  "pool_mb": float(gc[-1][4])} if gc else None,
                      "helper_calls_top20": functions,
                      "rtprof_lines": [line for line in stderr.splitlines() if line.startswith("[rtprof]")]}
            row["modes"][mode] = record
            print(kind, mode, "median_ms=", record["median_in_project_ms"],
                  "cons/flatten=", record["str"], "GC=", len(gc), flush=True)
        summary["workloads"][kind] = row
        (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
