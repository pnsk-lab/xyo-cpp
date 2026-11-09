#!/usr/bin/env python3
"""Run the real SDL-offscreen Scratch parity project against Scratch expectations.

Unlike wasm_scratchbench_backend_parity.py, this checks the project's own
PASS/FAIL oracle, not just equality between several xyo execution modes.
A prior renderer-specific STATUS_YIELD_TICK change made 18 existing tests
fail, but the backend-to-backend comparison still claimed 2123/2123 parity.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
PROJECT = ROOT / "scratch-bench/parity/Scratch Parity Test.sb3"
SUMMARY = re.compile(
    r"\[(PASSED|FAILED)\] (?:All Tests Were Successful|"
    r"Some Tests Were Not Successful): (\d+)/(\d+)"
)


def verify(binary: Path, project: Path, mode: str, minimum: int) -> None:
    env = os.environ.copy()
    env.update({
        "SDL_VIDEODRIVER": "offscreen",
        "SDL_AUDIODRIVER": "dummy",
        "XYO_EXIT_ON_DONE": "1",
        "XYO_JIT_MODE": mode,
        # Avoid opt-in probes interfering with the Scratch baseline.
        "XYO_EVENT_SCHED": "0",
        "XYO_IDLE_SOUND_POLL": "0",
    })
    cmd = [
        str(binary.resolve()), "play", str(project.resolve()),
        "--print-list", "output", "--list-limit", "200000",
    ]
    completed = subprocess.run(
        cmd, cwd=ROOT, env=env, capture_output=True,
        text=True, timeout=120,
    )
    transcript = completed.stdout + "\n" + completed.stderr
    matches = list(SUMMARY.finditer(transcript))
    if completed.returncode or not matches:
        raise AssertionError(
            f"{mode}: exit={completed.returncode}; "
            f"no complete parity summary\n{transcript[-2500:]}"
        )
    verdict, passed_text, total_text = matches[-1].groups()
    passed, total = int(passed_text), int(total_text)
    if verdict != "PASSED" or passed != total or total < minimum:
        failures = "\n".join(
            line for line in transcript.splitlines()
            if "[FAILED]" in line or "Test " in line
        )
        raise AssertionError(
            f"{mode}: {passed}/{total} (expected all and at least "
            f"{minimum} checks)\n{failures[-3500:]}"
        )
    print(f"PASS {mode}: real GUI Scratch parity {passed}/{total}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--project", type=Path, default=PROJECT)
    parser.add_argument("--minimum", type=int, default=2123)
    parser.add_argument(
        "--modes", nargs="+",
        choices=["off", "hotspot", "eager"], default=["off", "hotspot", "eager"],
    )
    args = parser.parse_args()
    for mode in args.modes:
        verify(args.native, args.project, mode, args.minimum)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
