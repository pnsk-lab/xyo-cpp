#!/usr/bin/env python3
"""Ensure giant Eager scripts defer cold procedures without gating noisy timings.

The pinned full ScratchBench parity fixture contains a 241-function script.
Read only through the first project output; the full oracle is checked by the
independent ScratchBench run. This test checks the optimization selection,
not whether a fast machine happened to win a timing race.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import selectors
import subprocess
import tempfile
import time

TIMING = re.compile(r"\[jit-timing\].*?funcs=(\d+) ir=(\d+) tier=(lazy|eager)")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True, type=Path)
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()

    env = os.environ.copy()
    env.update({
        "SDL_VIDEODRIVER": "offscreen",
        "SDL_AUDIODRIVER": "dummy",
        "XYO_EXIT_ON_DONE": "1",
        "XYO_JIT_MODE": "eager",
        "XYO_JIT_LAZY_MODE": "auto",
        "XYO_JIT_FAST_START": "1",
        "XYO_JIT_TIMING": "1",
        "XYO_EVENT_SCHED": "0",
        "XYO_IDLE_SOUND_POLL": "0",
    })
    cmd = [
        str(args.native.resolve()), "play", str(args.project.resolve()),
        "--print-list", "output", "--list-limit", "200000",
    ]

    with tempfile.TemporaryFile() as stderr:
        started = time.perf_counter()
        proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=stderr, env=env,
            stdin=subprocess.DEVNULL,
        )
        first = b""
        selection = selectors.DefaultSelector()
        try:
            assert proc.stdout is not None
            selection.register(proc.stdout, selectors.EVENT_READ)
            output = b""
            while time.perf_counter() - started < args.timeout:
                if not selection.select(timeout=1):
                    if proc.poll() is not None:
                        break
                    continue
                chunk = os.read(proc.stdout.fileno(), 65536)
                if not chunk:
                    break
                output += chunk
                if b"\n" in output:
                    first = output.split(b"\n", 1)[0]
                    break
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait(timeout=10)
            selection.close()
        duration_ms = (time.perf_counter() - started) * 1000
        stderr.seek(0)
        timing = stderr.read().decode("utf-8", errors="replace")

    if not first.startswith(b"Scratch Parity Test: 2656 tests in 206 groups"):
        raise AssertionError(f"full parity fixture did not start: {first!r}\n{timing[-800:]}")

    scripts = [(int(funcs), int(ir), tier) for funcs, ir, tier in TIMING.findall(timing)]
    if not any(funcs >= 64 and ir >= 100000 and tier == "lazy"
               for funcs, ir, tier in scripts):
        raise AssertionError(f"giant Eager script was not deferred: {scripts}")
    if not any(funcs == 1 and tier == "eager" for funcs, ir, tier in scripts):
        raise AssertionError(f"small Eager script was unexpectedly deferred: {scripts}")
    print(f"PASS giant Eager lazy tier; small scripts eager; first output {duration_ms:.1f}ms")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
