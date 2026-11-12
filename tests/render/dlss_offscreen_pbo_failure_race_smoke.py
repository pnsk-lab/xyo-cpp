#!/usr/bin/env python3
"""Direct mapped-PBO cleanup when writer fails during renderer handoff."""
from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from dlss_pen_primitive_smoke import make_case


def run(binary: str, scene: Path, prefix: Path, *, rgb: bool,
        fail_after: int = 0, abort_before_enqueue: bool = False
        ) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    for key in ("DISPLAY", "WAYLAND_DISPLAY", "SDL_VIDEODRIVER",
                "EGL_PLATFORM", "XYO_SHOT_ASYNC_TEST_FAIL_AFTER",
                "XYO_SHOT_ASYNC_TEST_ENQUEUE_DELAY_MS",
                "XYO_SHOT_ASYNC_TEST_WRITE_DELAY_MS",
                "XYO_SHOT_ASYNC_TEST_ABORT_BEFORE_ENQUEUE"):
        env.pop(key, None)
    env.update({
        "XYO_HEADLESS": "offscreen",
        "XYO_DLSS_REAL": "1",
        "XYO_DLSS_OPTICAL_FLOW": "1",
        "XYO_DLSS_TEMPORAL": "1",
        "XYO_RENDER_SCALE": "0.5",
        "XYO_DLSS_REAL_DEBUG": "1",
        "XYO_EXIT_FRAMES": "12",
        "XYO_SHOT_SERIES": str(prefix),
        "XYO_SHOT_SERIES_ASYNC": "1",
        "XYO_SHOT_ASYNC_DIRECT_MAP": "1",
        "XYO_SHOT_ASYNC_RGB": "1" if rgb else "0",
        "XYO_SHOT_ASYNC_STATS": "1",
    })
    if fail_after:
        env["XYO_SHOT_ASYNC_TEST_FAIL_AFTER"] = str(fail_after)
        env["XYO_SHOT_ASYNC_TEST_ENQUEUE_DELAY_MS"] = "40"
    if abort_before_enqueue:
        env["XYO_SHOT_ASYNC_TEST_ABORT_BEFORE_ENQUEUE"] = "1"

    return subprocess.run(
        [binary, "play", str(scene), "--scale", "2", "--fps", "1000"],
        capture_output=True, text=True, check=False, env=env, timeout=30,
    )


def check_lifecycle(result: subprocess.CompletedProcess[str]) -> tuple[int, int, int]:
    match = re.search(
        r"\[async PPM\] lifecycle created=(\d+) deleted=(\d+) "
        r"borrowed_remaining=(\d+) completed_remaining=(\d+) "
        r"aborted_before_enqueue=(\d+)",
        result.stderr,
    )
    if not match:
        raise RuntimeError(f"missing lifecycle telemetry:\n{result.stderr[-2500:]}")
    created, deleted, borrowed, completed, aborted = map(int, match.groups())
    if created == 0 or created != deleted or borrowed or completed:
        raise RuntimeError(f"GL PBO resource leak: {match.group()}")
    if "GL buffer lifecycle mismatch" in result.stderr:
        raise RuntimeError("GL lifecycle mismatch reported")
    return created, deleted, aborted


def main() -> None:
    if len(sys.argv) != 2:
        raise ValueError("usage: dlss_offscreen_pbo_failure_race_smoke.py <xyo>")
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="xyo-direct-pbo-failure-") as tmp:
        root = Path(tmp)
        scene = root / "pen.sb3"
        make_case(scene, "replay")
        for rgb in (False, True):
            label = "rgb" if rgb else "rgba"
            normal = run(binary, scene, root / f"{label}-normal", rgb=rgb)
            if normal.returncode or "NVIDIA NGX initialized" not in normal.stderr:
                raise RuntimeError(f"{label}: normal capture failed:\n{normal.stderr[-1800:]}")
            _, _, normal_aborts = check_lifecycle(normal)
            if normal_aborts != 0:
                raise RuntimeError("PBO was unexpectedly abandoned on normal exit")
            if len(list(root.glob(f"{label}-normal-??????.ppm"))) != 12:
                raise RuntimeError(f"{label}: missing normal frames")

            failed = run(
                binary, scene, root / f"{label}-failure",
                rgb=rgb, fail_after=1,
            )
            if (failed.returncode != 2 or
                    "[async PPM] cannot save" not in failed.stderr or
                    "asynchronous GPU readback or write failed" not in failed.stderr):
                raise RuntimeError(
                    f"{label}: injected error not propagated (exit {failed.returncode}):\n"
                    f"{failed.stderr[-2500:]}"
                )
            _, _, writer_abort = check_lifecycle(failed)
            # The writer might fail just before or just after a pending
            # PBO is mapped. Either scheduling is valid; lifecycle cleanup
            # must hold in both cases.
            aborted_result = run(
                binary, scene, root / f"{label}-abort-before-enqueue",
                rgb=rgb, abort_before_enqueue=True,
            )
            if (aborted_result.returncode != 2 or
                    "injected mapped PBO abort before enqueue"
                    not in aborted_result.stderr or
                    "asynchronous GPU readback or write failed"
                    not in aborted_result.stderr):
                raise RuntimeError(
                    f"{label}: forced mapped-PBO handoff abort failed:\n"
                    f"{aborted_result.stderr[-2500:]}"
                )
            _, _, forced_abort = check_lifecycle(aborted_result)
            if forced_abort < 1:
                raise RuntimeError(
                    f"{label}: mapped-PBO handoff cleanup was not exercised:\n"
                    f"{aborted_result.stderr[-2500:]}"
                )
            print(
                f"{label}: writer-error aborts={writer_abort}, "
                f"forced mapped handoff aborts={forced_abort}; "
                "exit=2, created=deleted, borrowed/completed=0",
                flush=True,
            )
        print("PASS: RGBA and RGB writer failure and deterministic mapped-PBO abort")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
