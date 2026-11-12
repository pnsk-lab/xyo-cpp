#!/usr/bin/env python3
"""CPU-only LSan regression for native JIT lifetime and cache destruction.

Unlike OpenGL tests, this does not load Mesa/NVIDIA driver components.
The synthetic Scratch program has an infinite script, so xyo run's
frame-limit exit code 2 is expected and verified.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from dlss_pen_primitive_smoke import make_case


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--runs", type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.runs <= 50:
        parser.error("--runs must be 1..50")

    env = os.environ.copy()
    for key in ("XYO_JIT_DISABLE", "XYO_JIT_MODE",
                "XYO_SHOT_SERIES", "XYO_SHOT_SERIES_ASYNC"):
        env.pop(key, None)
    env["XYO_JIT_COMPILE_STATS"] = "1"
    with tempfile.TemporaryDirectory(prefix="xyo-jit-cpu-lsan-") as tmp:
        scene = Path(tmp) / "scene.sb3"
        make_case(scene, "replay")
        for index in range(args.runs):
            for mode in ("off", "hotspot", "eager"):
                proc = subprocess.run(
                    [str(args.binary.resolve()), "run", str(scene),
                     "--frames", "12", "--jit", mode],
                    env=env, capture_output=True, text=True, timeout=25,
                )
                # Two is xyo's deliberate exit for a running project that
                # reaches its requested frame cap. It is not a crash.
                if (proc.returncode != 2 or
                        "frame limit reached" not in proc.stdout or
                        "ERROR: LeakSanitizer" in proc.stderr or
                        "ERROR: AddressSanitizer" in proc.stderr or
                        "runtime error:" in proc.stderr):
                    raise RuntimeError(
                        f"mode={mode} trial={index} exit={proc.returncode}\n"
                        f"{proc.stdout[-500:]}\n{proc.stderr[-2600:]}"
                    )
                m = re.search(
                    r"\[jit-compile\] sync attempts=(\d+) success=(\d+)",
                    proc.stderr,
                )
                if not m:
                    raise RuntimeError(f"JIT telemetry absent in {mode}")
                attempts, success = map(int, m.groups())
                if mode == "off":
                    if attempts or success:
                        raise RuntimeError("JIT-disabled run compiled native code")
                elif not attempts or not success:
                    raise RuntimeError(f"{mode}: no successful native JIT compilation")
        print(
            f"PASS CPU-only native JIT sanitizer: "
            f"{3*args.runs} runs; no reported ASan/UBSan/LSan faults",
            flush=True,
        )


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
