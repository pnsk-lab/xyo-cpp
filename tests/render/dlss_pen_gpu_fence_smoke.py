#!/usr/bin/env python3
"""Validate GPU Pen shared-context fence against glFinish with real NGX."""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile

from dlss_pen_primitive_smoke import make_case


def play(binary: str, project: Path, root: Path, mode: str,
         frames: int) -> tuple[bytes, bytes]:
    output = root / f"{mode}-{frames}.ppm"
    input_frame = root / f"{mode}-{frames}-input.ppm"
    env = os.environ.copy()
    env.update({
        "XYO_DLSS_REAL": "1",
        "XYO_DLSS_OPTICAL_FLOW": "1",
        "XYO_DLSS_TEMPORAL": "1",
        "XYO_DLSS_SCENE_CUT_SYNC": "1",
        "XYO_DLSS_RASTER_JITTER": "1",
        "XYO_DLSS_PEN_PRIMITIVE_REPLAY": "1",
        "XYO_DLSS_PEN_REPLAY_GPU": "1",
        "XYO_DLSS_PEN_REPLAY_SYNC": mode,
        "XYO_DLSS_REAL_DEBUG": "1",
        "XYO_DLSS_MODE": "performance",
        "XYO_DLSS_PRESET": "M",
        "XYO_RENDER_SCALE": "0.5",
        "XYO_PEN_FSR": "0",
        "XYO_EXIT_FRAMES": str(frames),
        "XYO_SHOT": str(output),
        "XYO_DLSS_RASTER_INPUT_CAPTURE": str(input_frame),
        "XYO_DLSS_RASTER_CAPTURE_FRAME": "8",
    })
    result = subprocess.run(
        [binary, "play", str(project), "--scale", "2"],
        env=env, capture_output=True, text=True, timeout=50, check=False,
    )
    logs = result.stderr + result.stdout
    if result.returncode != 0 or "NVIDIA NGX initialized" not in logs:
        raise RuntimeError(f"{mode} NGX playback failed:\n{logs}")
    expected = "shared GL fence" if mode == "fence" else "glFinish"
    if f"GPU pen replay sync: {expected}" not in logs:
        raise RuntimeError(f"Wrong sync path for {mode}:\n{logs}")
    if "GPU pen primitive rerasterization active" not in logs:
        raise RuntimeError(f"No GPU Pen replay for {mode}:\n{logs}")
    return input_frame.read_bytes(), output.read_bytes()


def main() -> None:
    if len(sys.argv) != 2:
        raise ValueError("usage: dlss_pen_gpu_fence_smoke.py <xyo>")
    with tempfile.TemporaryDirectory(prefix="xyo-dlss-pen-fence-") as folder:
        root = Path(folder)
        project = root / "pen-line.sb3"
        make_case(project, "replay")
        for frames in (16, 120):
            reference = play(sys.argv[1], project, root, "finish", frames)
            tested = play(sys.argv[1], project, root, "fence", frames)
            for name, base, candidate in zip(
                ("input", "final"), reference, tested
            ):
                changed = sum(x != y for x, y in zip(base, candidate))
                print(f"{frames} frames {name} difference={changed} bytes")
                if base != candidate:
                    raise RuntimeError(
                        f"GPU fence differs from glFinish: {frames} {name}"
                    )
    print("PASS: 16- and 120-frame NVIDIA NGX Pen GPU fence parity")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as e:
        print("FAIL:", e, file=sys.stderr)
        sys.exit(1)
