#!/usr/bin/env python3
"""Regression: LLVM JIT compile-cache teardown with real SDL offscreen+NGX.

Alternate Off, HotSpot and Eager modes to detect shutdown regressions.
The JIT-enabled modes must actually compile the provided Scratch script.
"""
from __future__ import annotations

import argparse
import collections
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from dlss_pen_primitive_smoke import make_case


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--runs", type=int, default=12)
    parser.add_argument("--frames", type=int, default=16)
    parser.add_argument("--timeout", type=int, default=35,
                        help="per-process timeout (increase for sanitizer builds)")
    parser.add_argument("--modes", default="off,hotspot,eager",
                        help="comma-separated modes; useful for sanitizer isolation")
    parser.add_argument("--capture", action="store_true",
                        help="also stress async durable PBO and orphan cleanup")
    parser.add_argument("--allow-dlss-fallback", action="store_true",
                        help="allow OpenGL fallback when sanitizer/driver interop rejects NGX")
    args = parser.parse_args()
    if not (1 <= args.runs <= 100 and 8 <= args.frames <= 120 and
            10 <= args.timeout <= 300):
        parser.error("runs=1..100, frames=8..120, timeout=10..300 required")
    modes = tuple(args.modes.split(","))
    if not modes or len(set(modes)) != len(modes) or any(
        mode not in ("off", "hotspot", "eager") for mode in modes
    ):
        parser.error("--modes must contain unique off,hotspot,eager names")

    env = os.environ.copy()
    for key in (
        "DISPLAY", "WAYLAND_DISPLAY", "EGL_PLATFORM", "SDL_VIDEODRIVER",
        "XYO_JIT_MODE", "XYO_JIT_DISABLE", "XYO_SHOT_SERIES", "XYO_SHOT",
        "XYO_SHOT_SERIES_ASYNC", "XYO_SHOT_ASYNC_RGB",
        "XYO_SHOT_ASYNC_DIRECT_MAP", "XYO_SHOT_ASYNC_DURABLE",
        "XYO_SHOT_ASYNC_RECOVER_STALE", "XYO_SHOT_ASYNC_RECOVERY_THREAD",
        "XYO_SHOT_ASYNC_LOCK_SERIES", "XYO_SHOT_ASYNC_STATS",
        "XYO_SHOT_ASYNC_BUFFER_POOL", "XYO_SHOT_ASYNC_ATOMIC",
        "XYO_SHOT_ASYNC_TEST_FAIL_STAGE", "XYO_SHOT_ASYNC_TEST_FAIL_SYNC",
        "XYO_SHOT_ASYNC_TEST_STAGE_PAUSE_MS",
        "XYO_SHOT_ASYNC_TEST_WRITE_DELAY_MS",
        "XYO_SHOT_ASYNC_TEST_PRE_UNLINK_PAUSE_MS",
        "XYO_SHOT_ASYNC_TEST_POST_UNLINK_PAUSE_MS",
        "XYO_SHOT_ASYNC_TEST_AFTER_RECOVERY_BATCH_PAUSE_MS",
        "XYO_SHOT_ASYNC_TEST_FAIL_RECOVERY_SYNC",
        "XYO_SHOT_ASYNC_TEST_RECOVERY_PAUSE_MS",
        "XYO_SHOT_ASYNC_TEST_RECOVERY_ENTRY_DELAY_MS",
    ):
        env.pop(key, None)
    env.update({
        "XYO_HEADLESS": "offscreen",
        "XYO_EXIT_FRAMES": str(args.frames),
        "XYO_DLSS_REAL": "1",
        "XYO_DLSS_REAL_DEBUG": "1",
        "XYO_DLSS_TEMPORAL": "1",
        "XYO_DLSS_OPTICAL_FLOW": "1",
        "XYO_RENDER_SCALE": "0.5",
        "XYO_HOT_STATS": "1",
        "XYO_JIT_COMPILE_STATS": "1",
    })
    results: dict[str, list[int]] = collections.defaultdict(list)
    backends: collections.Counter[str] = collections.Counter()
    with tempfile.TemporaryDirectory(prefix="xyo-jit-teardown-offscreen-") as temp:
        scene = Path(temp) / "scene.sb3"
        make_case(scene, "replay")
        for iteration in range(args.runs):
            # Rotate the execution order to mitigate thermal/driver drift.
            ordered = modes[iteration % len(modes):] + modes[:iteration % len(modes)]
            for mode in ordered:
                run_env = env.copy()
                if args.capture:
                    prefix = Path(temp) / f"shots-{iteration}-{mode}"
                    run_env.update({
                        "XYO_SHOT_SERIES": str(prefix),
                        "XYO_SHOT_SERIES_ASYNC": "1",
                        "XYO_SHOT_ASYNC_RGB": "1",
                        "XYO_SHOT_ASYNC_DIRECT_MAP": "1",
                        "XYO_SHOT_ASYNC_DURABLE": "1",
                        "XYO_SHOT_ASYNC_RECOVER_STALE": "1",
                        "XYO_SHOT_ASYNC_RECOVERY_THREAD": "1",
                        "XYO_SHOT_ASYNC_STATS": "1",
                    })
                    stamp = time.time() - 2 * 86400
                    for item in range(3):
                        orphan = Path(str(prefix) +
                                      f"-{item+1000:06d}.ppm.part.{item:06d}")
                        orphan.write_bytes(b"old staging file")
                        os.utime(orphan, (stamp, stamp))
                try:
                    completed = subprocess.run(
                        [str(args.binary.resolve()), "play", str(scene),
                         "--scale", "2", "--fps", "1000", "--jit", mode],
                        env=run_env, capture_output=True, text=True,
                        timeout=args.timeout,
                    )
                except subprocess.TimeoutExpired as timeout:
                    tail = timeout.stderr or b""
                    if isinstance(tail, bytes):
                        tail = tail.decode("utf-8", errors="replace")
                    raise RuntimeError(
                        f"iteration {iteration} mode {mode} timeout after "
                        f"{args.timeout}s; captured stderr:\n{tail[-2800:]}"
                    ) from timeout
                results[mode].append(completed.returncode)
                print(f"iteration={iteration} mode={mode} exit={completed.returncode}",
                      flush=True)
                if args.capture and completed.returncode == 0:
                    images = list(Path(temp).glob(
                        f"shots-{iteration}-{mode}-??????.ppm"))
                    if len(images) != args.frames:
                        raise RuntimeError(
                            f"{mode} iteration {iteration}: saved "
                            f"{len(images)} of {args.frames} PPM frames")
                    if ("durability enabled=1 file_fsyncs="
                            f"{args.frames} dir_fsyncs={args.frames} "
                            "sync_failures=0") not in completed.stderr:
                        raise RuntimeError(
                            f"{mode} iteration {iteration}: durable "
                            f"writer incomplete\n{completed.stderr[-1700:]}")
                    if ("recovery checked=3 removed=3" not in
                            completed.stderr):
                        raise RuntimeError(
                            f"{mode} iteration {iteration}: staged orphan "
                            f"recovery incomplete\n{completed.stderr[-1700:]}")

                ngx_ok = "NVIDIA NGX initialized" in completed.stderr
                gl_fallback = (
                    args.allow_dlss_fallback and
                    "SDL renderer=opengl, OpenGL context=yes" in completed.stderr and
                    "using GL fallback" in completed.stderr
                )
                if completed.returncode or not (ngx_ok or gl_fallback):
                    raise RuntimeError(
                        f"iteration {iteration} mode {mode}: exit="
                        f"{completed.returncode}\n{completed.stderr[-2800:]}"
                    )
                backends["NGX" if ngx_ok else "GL-fallback"] += 1
                if mode == "hotspot":
                    required = (
                        "[jit-compile] prepare attempts=1 success=1",
                        "[jit-compile] finish attempts=1 success=1",
                    )
                elif mode == "eager":
                    required = ("[jit-compile] sync attempts=1 success=1",)
                else:
                    required = ("[jit-compile] sync attempts=0 success=0",)
                for marker in required:
                    if marker not in completed.stderr:
                        raise RuntimeError(
                            f"iteration {iteration} {mode}: expected actual JIT "
                            f"activity {marker!r}\n{completed.stderr[-2500:]}"
                        )

    for mode in modes:
        print(f"{mode}: {len(results[mode])}/{args.runs} clean exits", flush=True)
    print(
        f"render backends: NGX={backends['NGX']} "
        f"OpenGL fallback={backends['GL-fallback']}",
        flush=True,
    )
    print(
        "PASS: compiled LLVM JIT cache teardown, offscreen rendering "
        + ("and async durable PBO recovery" if args.capture else "(no capture)")
    )


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
