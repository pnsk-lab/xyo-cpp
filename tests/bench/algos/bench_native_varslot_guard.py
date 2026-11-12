#!/usr/bin/env python3
"""Paired in-Scratch timing for native Variable* slot borrowing vs rebinds.

Pure Scratch warp loop, timed after JIT warm-up; same binary, CPU affinity,
alternating guard policy between fresh processes. Strict state checks.
The guarded policy is a lifetime/provenance trade-off, not a GC certificate.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import (
    Gen, S_add, S_call, S_delall, S_repeat, S_set, add, days, make_project,
    mul, num, sub, var, write_sb3
)

MODES = ("baseline", "guard", "guard_fast", "guard_indexed",
         "guard_inline")
KINDS = ("single_read", "three_reads")


def project(count: int, kind: str):
    gen = Gen(("source", "other", "third", "sum", "start", "end"),
              ("times", "checks"))
    if kind == "single_read":
        value = var("source")
        expected = count * 3
    elif kind == "three_reads":
        value = add(add(var("source"), var("other")), var("third"))
        expected = count * 10
    else:
        raise ValueError(kind)
    gen.define("work", [], [
        S_set("sum", num(0)),
        S_repeat(num(count), [S_set("sum", add(var("sum"), value))]),
    ], warp=True)
    gen.script([
        S_set("source", num(3)),
        S_set("other", num(2)),
        S_set("third", num(5)),
        S_delall("times"), S_delall("checks"),
        S_call("work"),  # compile & warm before timing
        S_repeat(num(3), [
            S_set("start", days()),
            S_call("work"),
            S_set("end", days()),
            S_add("times", mul(sub(var("end"), var("start")), num(86400000))),
            S_add("checks", var("sum")),
        ]),
    ])
    return make_project(gen.vars.keys(), gen.lists.keys(), gen), expected


def measure(binary: Path, sb3: Path, cpu: int, mode: str,
            expected: int) -> dict:
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update(
        XYO_GPU="0", XYO_PAR="0", XYO_PARFOR="0",
        XYO_RT_GUARD_NATIVE_VAR_SLOTS="0" if mode == "baseline" else "1",
        XYO_RT_GUARD_NATIVE_VAR_SLOTS_FAST=(
            "1" if mode in ("guard_fast", "guard_indexed",
                            "guard_inline") else "0"
        ),
        XYO_RT_GUARD_NATIVE_VAR_SLOTS_INDEXED=(
            "1" if mode in ("guard_indexed", "guard_inline") else "0"
        ),
        XYO_JIT_VAR_GUARD_INLINE="1" if mode == "guard_inline" else "0",
        XYO_JIT_LAZY_MODE="off", XYO_DUMPVARS="json",
        XYO_JIT_ASYNC="0", XYO_REPEAT_UNROLL="0",
    )
    proc = subprocess.run([
        "taskset", "-c", str(cpu), str(binary), "run", str(sb3),
        "--jit", "eager", "--frames", "500", "--fps", "60",
    ], cwd=ROOT, env=env, capture_output=True, text=True, timeout=100)
    if proc.returncode:
        raise RuntimeError((mode, proc.returncode, proc.stderr[-1500:]))
    try:
        result = json.loads(
            proc.stdout[proc.stdout.index("{"):proc.stdout.rindex("}") + 1]
        )["Bench"]
        times = [float(t) for t in result["times"]]
        checks = [float(x) for x in result["checks"]]
    except (ValueError, KeyError, IndexError) as exc:
        raise RuntimeError((mode, proc.stdout[-1500:])) from exc
    assert (len(times) == len(checks) == 3 and
            all(t > 0 for t in times) and
            all(x == expected for x in checks)), (
        mode, times, checks, expected,
    )
    return {"median_ms": statistics.median(times),
            "times_ms": times, "checksum": checks[0]}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cpu", type=int, default=None)
    parser.add_argument("--count", type=int, default=200_000)
    parser.add_argument("--pairs", type=int, default=6)
    args = parser.parse_args()
    cpus = sorted(os.sched_getaffinity(0))
    cpu = args.cpu if args.cpu is not None else cpus[0]
    assert cpu in cpus and args.count > 0 and args.pairs > 0
    args.out.parent.mkdir(parents=True, exist_ok=True)
    inputs = {}
    for kind in KINDS:
        path = args.out.parent / f"guard-bench-{kind}.sb3"
        project_data, expected = project(args.count, kind)
        write_sb3(path, project_data)
        inputs[kind] = (path, expected)
    rows = []
    for pair in range(args.pairs):
        for kind, (path, expected) in inputs.items():
            # Alternating and reversing across blocks limits drift.
            sequence = MODES if pair % 2 == 0 else tuple(reversed(MODES))
            for mode in sequence:
                data = measure(args.binary.resolve(), path, cpu, mode, expected)
                row = {"pair": pair + 1, "case": kind, "mode": mode, **data}
                rows.append(row)
                args.out.write_text(json.dumps(rows, indent=2) + "\n")
                print("RUN", pair + 1, kind, mode,
                      round(data["median_ms"], 4), flush=True)
    for kind in KINDS:
        by = {mode: [x for x in rows
                      if x["case"] == kind and x["mode"] == mode]
              for mode in MODES}
        assert len({x["checksum"] for group in by.values() for x in group}) == 1
        base = [x["median_ms"] for x in by["baseline"]]
        for mode in MODES[1:]:
            other = [x["median_ms"] for x in by[mode]]
            ratios = [o / b for b, o in zip(base, other)]
            print("RESULT", kind, mode,
                  "overhead_geom", round(statistics.geometric_mean(ratios), 5),
                  "overhead_median", round(statistics.median(ratios), 5),
                  "slow_pairs", sum(x > 1 for x in ratios),
                  "of", len(ratios),
                  "baseline_ms", round(statistics.median(base), 3),
                  "guarded_ms", round(statistics.median(other), 3),
                  flush=True)


if __name__ == "__main__":
    main()
