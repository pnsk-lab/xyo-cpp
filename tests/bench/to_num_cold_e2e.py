#!/usr/bin/env python3
"""End-to-end A/B test of xyo_to_num's JIT cold-string helper."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/differential"))
from jit_to_num_fastpath import fixture


def run(binary: Path, project: Path, enabled: bool, frames: int) -> tuple[float, dict]:
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update(XYO_PAR="0", XYO_GPU="0", XYO_DUMPVARS="json",
               XYO_JIT_TO_NUM_COLD_HELPER="1" if enabled else "0")
    start = time.perf_counter()
    proc = subprocess.run(
        [str(binary), "run", str(project), "--jit", "eager",
         "--frames", str(frames)], cwd=ROOT, env=env,
        capture_output=True, text=True, timeout=180,
    )
    ms = (time.perf_counter() - start) * 1000
    if proc.returncode:
        raise RuntimeError((enabled, proc.returncode, proc.stderr[-900:]))
    raw = proc.stdout
    result = json.loads(raw[raw.index("{"):raw.rindex("}") + 1])["Bench"]
    if result.get("done") != "1":
        raise AssertionError(("incomplete", enabled, result))
    return ms, result


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", type=Path, default=Path("build-to-num-cold/xyo"))
    ap.add_argument("--iterations", type=int, default=50000)
    ap.add_argument("--frames", type=int, default=2000)
    ap.add_argument("--runs", type=int, default=7)
    ap.add_argument("--output", type=Path, default=Path("build/to-num-cold-e2e.json"))
    args = ap.parse_args()
    binary = args.binary.resolve()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    project = output.parent / "to-num-cold-bench.sb3"
    fixture(project, warp=True, iterations=args.iterations)
    observations: dict[bool, list[float]] = {False: [], True: []}
    baseline = None
    for run_index in range(args.runs + 1):
        for enable in ((False, True) if run_index % 2 == 0 else (True, False)):
            elapsed, values = run(binary, project, enable, args.frames)
            if baseline is None:
                baseline = values
            elif values != baseline:
                raise AssertionError(("different Scratch result", enable, baseline, values))
            if run_index:
                observations[enable].append(round(elapsed, 4))
    result = dict(
        iterations=args.iterations, runs=args.runs,
        off_ms=observations[False], on_ms=observations[True],
        off_median_ms=statistics.median(observations[False]),
        on_median_ms=statistics.median(observations[True]),
        same_variables=True,
    )
    output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
