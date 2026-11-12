#!/usr/bin/env python3
"""Verify genuine SDL/EGL offscreen NGX playback with no X11/Wayland DISPLAY."""
from __future__ import annotations

import os
from pathlib import Path
import sys
import tempfile

from dlss_pen_primitive_smoke import make_case
from dlss_pen_gpu_precision_smoke import error, read_input, run


def main() -> int:
    if len(sys.argv) != 2:
        raise ValueError("usage: dlss_surfaceless_gpu_smoke.py <xyo>")

    with tempfile.TemporaryDirectory(prefix="xyo-dlss-egl-offscreen-") as work:
        root = Path(work)
        scene = root / "pen.sb3"
        make_case(scene, "replay")

        # Force the test to fail if xyo accidentally relies on the user's
        # desktop X11/Wayland server. The app itself must select the SDL
        # offscreen video driver from XYO_HEADLESS=offscreen.
        old_environment = os.environ.copy()
        try:
            for name in ("DISPLAY", "WAYLAND_DISPLAY",
                         "SDL_VIDEODRIVER", "EGL_PLATFORM"):
                os.environ.pop(name, None)
            cpu, cpu_log = run(
                sys.argv[1], scene, root, "offscreen-cpu",
                gpu=False, precise=False, headless="offscreen",
            )
            gpu, gpu_log = run(
                sys.argv[1], scene, root, "offscreen-gpu",
                gpu=True, precise=True, quantized=True, tiled=True,
                headless="offscreen",
            )
        finally:
            os.environ.clear()
            os.environ.update(old_environment)

        for name, log in (("CPU", cpu_log), ("GPU", gpu_log)):
            for marker in ("SDL window hidden: yes",
                           "SDL video driver=offscreen",
                           "SDL renderer=opengl",
                           "NVIDIA NGX initialized"):
                if marker not in log:
                    raise RuntimeError(f"{name} did not initialize: {marker}")
        if "GPU Pen quantized tiled16 active" not in gpu_log:
            raise RuntimeError("offscreen GPU quantized tile shader not selected")

        for frame in range(1, 33):
            expected = read_input(root, "offscreen-cpu", frame)
            actual = read_input(root, "offscreen-gpu", frame)
            if expected != actual:
                count, absolute = error(expected, actual)
                raise RuntimeError(
                    f"offscreen frame {frame} differed: "
                    f"{count} channels, absRGB={absolute}"
                )

        if cpu != gpu:
            count, absolute = error(cpu, gpu)
            raise RuntimeError(
                f"offscreen final NGX differs: "
                f"{count} channels, absRGB={absolute}"
            )

        print(
            "PASS: without DISPLAY/WAYLAND_DISPLAY, SDL offscreen EGL "
            "and NGX initialized; 32 pre-NGX frames and final image "
            "are byte-identical for CPU vs tiled GPU Pen",
            flush=True,
        )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError) as exc:
        print("FAIL:", exc, file=sys.stderr)
        sys.exit(1)
