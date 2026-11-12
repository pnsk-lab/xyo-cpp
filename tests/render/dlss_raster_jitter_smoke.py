#!/usr/bin/env python3
"""GPU end-to-end test that DLSS jitter changes the *rasterized SDL input*.

Tests 2x3 Halton jitter at frame 3 of a Scratch project with a rotating
transparent PNG, not just the NGX parameter log. Requires NVIDIA + SDL GL.
"""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile

from dlss_coverage_project_smoke import build_donut


def play(binary: str, project: Path, capture: Path,
         *, jitter: bool, temporal: bool) -> bytes:
    env = os.environ.copy()
    env.update({
        "XYO_DLSS_REAL": "1",
        "XYO_DLSS_OPTICAL_FLOW": "1",
        "XYO_DLSS_TEMPORAL": "1" if temporal else "0",
        "XYO_DLSS_RASTER_JITTER": "1" if jitter else "0",
        "XYO_DLSS_REAL_DEBUG": "1",
        "XYO_RENDER_SCALE": "0.5",
        "XYO_PEN_FSR": "0",
        "XYO_EXIT_FRAMES": "10",
        "XYO_DLSS_RASTER_CAPTURE_FRAME": "3",
        "XYO_DLSS_RASTER_INPUT_CAPTURE": str(capture),
    })
    p = subprocess.run(
        [binary, "play", str(project), "--scale", "2"],
        env=env, capture_output=True, text=True, timeout=35, check=False,
    )
    output = p.stdout + p.stderr
    if p.returncode:
        raise RuntimeError(f"real NGX playback failed:\n{output}")
    if "NVIDIA NGX initialized" not in output:
        raise RuntimeError("no native NVIDIA NGX initialization:\n" + output)
    active = "raster jitter frame 3:" in output
    if active != (jitter and temporal):
        raise RuntimeError(f"wrong jitter activation ({active}):\n{output}")
    if not capture.exists():
        raise RuntimeError("pre-NGX raster image capture missing")
    data = capture.read_bytes()
    if not data.startswith(b"P6\n480 360\n255\n"):
        raise RuntimeError("pre-NGX image dimensions or PPM format invalid")
    return data


def main() -> int:
    if len(sys.argv) != 2:
        raise ValueError("usage: dlss_raster_jitter_smoke.py <xyo executable>")
    with tempfile.TemporaryDirectory(prefix="xyo-raster-jitter-") as directory:
        root = Path(directory)
        project = root / "rotating-alpha-png.sb3"
        build_donut(project)
        normal = play(sys.argv[1], project, root / "normal.ppm",
                      jitter=False, temporal=True)
        jittered = play(sys.argv[1], project, root / "jitter.ppm",
                        jitter=True, temporal=True)
        if len(normal) != len(jittered):
            raise RuntimeError("rendered inputs differ in resolution")
        changed = sum(a != b for a, b in zip(normal, jittered))
        print(f"Pre-NGX SDL raster: changed RGB bytes with jitter={changed}")
        if changed < 30:
            raise RuntimeError("jitter did not measurably affect SDL raster")
        off_temporal = play(
            sys.argv[1], project, root / "no-temporal.ppm",
            jitter=True, temporal=False,
        )
        if off_temporal != normal:
            raise RuntimeError(
                "disabling temporal did not disable actual input jitter")
    print("PASS: actual subpixel input changed, NGX jitter synchronized, "
          "temporal-disabled parity preserved")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as e:
        print(f"FAIL: {e}", file=sys.stderr)
        sys.exit(1)
