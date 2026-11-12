#!/usr/bin/env python3
"""START-012 startup-only alternating native AOT v2/v3 comparison (Linux)."""
from __future__ import annotations

import argparse
import math
import os
from pathlib import Path
import re
import selectors
import signal
import statistics
import subprocess
import time


def first_output_ms(binary: Path, project: Path, obj: Path, cpu: int) -> float:
    args = [
        "taskset", "-c", str(cpu), str(binary),
        "play", str(project), "--aot", str(obj),
        "--print-list", "output", "--list-limit", "200000",
    ]
    env = {
        **os.environ,
        "SDL_VIDEODRIVER": "offscreen",
        "SDL_AUDIODRIVER": "dummy",
        "XYO_AOT_PROGRESS": "0",
        "XYO_EXIT_ON_DONE": "1",
    }
    started = time.perf_counter()
    proc = subprocess.Popen(
        args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
        start_new_session=True,
    )
    data = bytearray()
    err = bytearray()
    ms = None
    try:
        with selectors.DefaultSelector() as selector:
            assert proc.stdout is not None and proc.stderr is not None
            selector.register(proc.stdout, selectors.EVENT_READ)
            selector.register(proc.stderr, selectors.EVENT_READ)
            deadline = time.monotonic() + 30
            while ms is None and time.monotonic() < deadline:
                for key, _ in selector.select(timeout=1):
                    chunk = os.read(key.fileobj.fileno(), 65536)
                    if not chunk:
                        selector.unregister(key.fileobj)
                        continue
                    if key.fileobj == proc.stdout:
                        data.extend(chunk)
                        if b"\n" in data:
                            ms = (time.perf_counter() - started) * 1000
                            break
                    else:
                        err.extend(chunk)
            if ms is None:
                raise RuntimeError("no initial Scratch output within 30 seconds")
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGKILL)
        rest_out, rest_err = proc.communicate(timeout=10)
        err.extend(rest_err)
    first = bytes(data).split(b"\n", 1)[0]
    if not first.startswith(b"Scratch Parity Test: 2656 tests in 206 groups"):
        raise RuntimeError("unexpected first stdout line: " + repr(first[:150]))
    if not re.search(rb"aot: 479 scripts precompiled \(8423 slots\)", err):
        raise RuntimeError("native AOT load unverified: " + repr(bytes(err)[-300:]))
    return ms


def main() -> None:
    ap = argparse.ArgumentParser()
    for name in ("bin", "project", "v2", "v3"):
        ap.add_argument("--" + name, type=Path, required=True)
    ap.add_argument("--cpu", type=int, default=10)
    ap.add_argument("--pairs", type=int, default=20)
    a = ap.parse_args()
    files = {key: getattr(a, key).resolve() for key in ("bin", "project", "v2", "v3")}
    for p in files.values():
        if not p.is_file():
            raise FileNotFoundError(p)
    results = {"v2": [], "v3": []}
    for i in range(a.pairs):
        for name in (("v2", "v3") if i % 2 == 0 else ("v3", "v2")):
            results[name].append(first_output_ms(files["bin"], files["project"], files[name], a.cpu))
        print(f"{i+1}: v2={results['v2'][-1]:.1f}ms v3={results['v3'][-1]:.1f}ms", flush=True)
    v2, v3 = results["v2"], results["v3"]
    geo = math.exp(statistics.fmean(math.log(x / y) for x, y in zip(v2, v3)))
    print(f"median v2={statistics.median(v2):.1f}ms v3={statistics.median(v3):.1f}ms "
          f"v3-wins={sum(x>y for x,y in zip(v2,v3))}/{a.pairs} "
          f"geomean-v2-over-v3={geo:.4f}", flush=True)


if __name__ == "__main__":
    main()
