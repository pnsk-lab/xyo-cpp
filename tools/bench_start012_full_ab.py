#!/usr/bin/env python3
"""Paired native-AOT v2/v3 complete Scratch parity with wall time and peak RSS.

Both object files must be regenerated from the same xyo binary/source.
A non-native fallback or incomplete Scratch oracle is a hard failure.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import statistics
import subprocess
import time
import threading

PASS = re.compile(r"\[PASSED\] All Tests Were Successful: 2656/2656")
RSS = re.compile(r"VmHWM:\s*(\d+) kB")


def run(binary: Path, project: Path, obj: Path, cpu: int, parallel: str = "1") -> tuple[float, int]:
    command = [
        "taskset", "-c", str(cpu),
        str(binary), "play", str(project), "--aot", str(obj),
        "--print-list", "output", "--list-limit", "200000",
    ]
    env = {
        **os.environ,
        "SDL_VIDEODRIVER": "offscreen",
        "SDL_AUDIODRIVER": "dummy",
        "XYO_AOT_PROGRESS": "0",
        "XYO_EXIT_ON_DONE": "1",
        "XYO_PAR": parallel,
    }
    started = time.perf_counter()
    with subprocess.Popen(
        command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
    ) as process:
        stopped = threading.Event()
        peak_kib = [0]

        def capture_rss() -> None:
            # Linux VmHWM is the kernel's exact process high-water RSS.
            # The child is taskset -> exec xyo; PID remains constant.
            status = Path(f"/proc/{process.pid}/status")
            while not stopped.is_set():
                try:
                    match = RSS.search(status.read_text())
                    if match:
                        peak_kib[0] = max(peak_kib[0], int(match.group(1)))
                except OSError:
                    pass
                stopped.wait(0.02)

        monitor = threading.Thread(target=capture_rss, daemon=True)
        monitor.start()
        try:
            stdout, stderr = process.communicate(timeout=120)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
            stopped.set()
            monitor.join()
        elapsed = (time.perf_counter() - started) * 1000
        if process.returncode or not PASS.search(stdout):
            raise RuntimeError(
                f"native Scratch parity failed: exit={process.returncode} "
                + (stdout + stderr)[-2000:]
            )
        if not re.search(r"aot: [1-9][0-9]* scripts precompiled", stderr) or (
            "aot: load failed" in stderr
        ):
            raise RuntimeError("native AOT load not verified: " + stderr[-1000:])
        if peak_kib[0] == 0:
            raise RuntimeError("VmHWM RSS was not sampled")
        return elapsed, peak_kib[0]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--v2", type=Path, required=True)
    parser.add_argument("--v3", type=Path, required=True)
    parser.add_argument("--cpu", type=int, default=10)
    parser.add_argument("--pairs", type=int, default=4)
    parser.add_argument("--par", choices=("0", "1"), default="1")
    args = parser.parse_args()
    paths = [p.resolve() for p in (args.bin, args.project, args.v2, args.v3)]
    for p in paths:
        if not p.is_file():
            raise FileNotFoundError(p)
    if args.pairs < 1:
        parser.error("pairs must be positive")

    results: dict[str, list[tuple[float, int]]] = {"v2": [], "v3": []}
    for i in range(args.pairs):
        order = ("v2", "v3") if i % 2 == 0 else ("v3", "v2")
        for version in order:
            obj = paths[2] if version == "v2" else paths[3]
            elapsed, rss = run(paths[0], paths[1], obj, args.cpu, args.par)
            results[version].append((elapsed, rss))
        print(
            f"pair={i + 1}: "
            + " ".join(
                f"{k}={results[k][-1][0]:.1f}ms/{results[k][-1][1]}KiB"
                for k in ("v2", "v3")
            ), flush=True,
        )
    for k, rows in results.items():
        print(
            f"{k}: median wall={statistics.median(r[0] for r in rows):.1f}ms"
            f" median rss={statistics.median(r[1] for r in rows):.0f}KiB",
            flush=True,
        )
    ratios = [
        results["v2"][i][0] / results["v3"][i][0]
        for i in range(args.pairs)
    ]
    import math
    print(
        f"v3 wins={sum(v > 1 for v in ratios)}/{args.pairs}"
        f" geometric speed ratio={math.exp(statistics.fmean(map(math.log, ratios))):.4f}",
        flush=True,
    )


if __name__ == "__main__":
    main()
