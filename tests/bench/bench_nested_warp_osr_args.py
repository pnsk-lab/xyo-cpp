#!/usr/bin/env python3
"""Measure only the promoted native warp-loop interval (not LLVM compilation)."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/differential"))
from jit_nested_warp_args_preserved import fixture

PROMOTION = re.compile(r"\[jit-osr-nested\] promoted loop=.+ remaining=([0-9]+)")
MODES = {"reporter": (False, False), "direct": (True, False),
         "hoisted": (True, True)}


def measure(binary: Path, project: Path, mode: str, expected: dict) -> dict:
    direct, hoist = MODES[mode]
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "SDL_VIDEODRIVER": "offscreen", "SDL_AUDIODRIVER": "dummy",
        "XYO_JIT_MODE": "hotspot", "XYO_JIT_ASYNC": "1",
        "XYO_JIT_OSR": "1", "XYO_JIT_OSR_NESTED": "1",
        "XYO_JIT_OSR_NESTED_ARG_READS": "1",
        "XYO_JIT_OSR_NESTED_DIRECT_ARGS": "1" if direct else "0",
        "XYO_JIT_OSR_NESTED_HOIST_ARGS": "1" if hoist else "0",
        "XYO_JIT_OSR_TIMING": "1", "XYO_EXIT_ON_DONE": "1",
        "XYO_DUMPVARS": "json", "XYO_GPU": "0", "XYO_PAR": "0",
        "XYO_PARFOR": "0", "XYO_BLOCK_COUNT": "0",
    })
    start = finish = remaining = None
    stderr_lines = []
    with subprocess.Popen(
        [str(binary.resolve()), "play", str(project.resolve())],
        cwd=ROOT, env=env, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, bufsize=1, text=True,
    ) as proc:
        assert proc.stderr and proc.stdout
        # Also time out the streaming-read phase; subprocess.wait(timeout)
        # cannot protect a blocked readline() when a native fiber hangs.
        watchdog = threading.Timer(180, proc.kill)
        watchdog.start()
        try:
            for line in proc.stderr:
                instant = time.perf_counter_ns()
                stderr_lines.append(line)
                match = PROMOTION.search(line)
                if match:
                    start, remaining = instant, int(match.group(1))
                if "[jit-osr-nested] restored caller" in line:
                    finish = instant
            output = proc.stdout.read()
            proc.wait(timeout=180)
        finally:
            watchdog.cancel()
    assert proc.returncode == 0 and start and finish and remaining, (
        mode, proc.returncode, "".join(stderr_lines)[-1500:]
    )
    state, _ = json.JSONDecoder().raw_decode(output[output.index("{"):])
    assert all(state["Bench"].get(k) == value
               for k, value in expected.items()), (mode, state, expected)
    elapsed = (finish - start) / 1e9
    return {"mode": mode, "remaining": remaining,
            "nativeSeconds": round(elapsed, 6),
            "nativeMillionsPerSecond": round(remaining / elapsed / 1e6, 4)}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--iterations", type=int, default=12_000_000)
    parser.add_argument("--rounds", type=int, default=2)
    args = parser.parse_args()
    assert args.iterations >= 1_000_000 and args.rounds >= 1
    args.out.mkdir(parents=True, exist_ok=True)
    project = args.out / "native-arg-loop.sb3"
    _, expected = fixture(project, use_argument_in_loop=True,
                          iterations_override=args.iterations)
    samples = []
    for round_ in range(args.rounds):
        for mode in ("reporter", "direct", "hoisted",
                     "hoisted", "direct", "reporter"):
            result = measure(args.native, project, mode, expected)
            result["round"] = round_ + 1
            samples.append(result)
            print(f"round={round_+1} {mode} "
                  f"{result['nativeMillionsPerSecond']} Miter/s "
                  f"remaining={result['remaining']}", flush=True)
    medians = {
        mode: round(statistics.median(
            v["nativeMillionsPerSecond"] for v in samples if v["mode"] == mode
        ), 4) for mode in MODES
    }
    print("Median native Miter/s:", medians, flush=True)
    print("Speedups:", {
        mode: round(medians[mode] / medians["reporter"], 3)
        for mode in ("direct", "hoisted")
    }, flush=True)
    (args.out / "results.json").write_text(
        json.dumps({"medians": medians, "samples": samples}, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
