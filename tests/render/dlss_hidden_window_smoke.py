#!/usr/bin/env python3
"""Real NGX/SDL regression: visible vs hidden window must render identical frames."""
from __future__ import annotations

from pathlib import Path
import sys
import tempfile

from dlss_pen_primitive_smoke import make_case
from dlss_pen_gpu_precision_smoke import error, read_input, run


def main() -> int:
    if len(sys.argv) != 2:
        raise ValueError("usage: dlss_hidden_window_smoke.py <xyo>")

    with tempfile.TemporaryDirectory(prefix="xyo-dlss-hidden-") as work:
        root = Path(work)
        scene = root / "pen.sb3"
        make_case(scene, "replay")

        baseline, normal_log = run(
            sys.argv[1], scene, root, "visible",
            gpu=True, precise=True, quantized=True, tiled=True,
            headless=False,
        )
        hidden, hidden_log = run(
            sys.argv[1], scene, root, "hidden",
            gpu=True, precise=True, quantized=True, tiled=True,
            headless=True,
        )

        required = (
            "SDL window hidden: yes",
            "GPU Pen quantized tiled16 active",
            "NVIDIA NGX initialized",
        )
        for marker in required:
            if marker not in hidden_log:
                raise RuntimeError(f"hidden playback lacks: {marker}")

        if "SDL window hidden: yes" in normal_log:
            raise RuntimeError("normal playback unexpectedly hid its window")

        for frame in range(1, 33):
            a = read_input(root, "visible", frame)
            b = read_input(root, "hidden", frame)
            if a != b:
                count, absolute = error(a, b)
                raise RuntimeError(
                    f"pre-NGX input differs at frame {frame}: "
                    f"{count} channels, absRGB={absolute}"
                )
        if baseline != hidden:
            count, absolute = error(baseline, hidden)
            raise RuntimeError(
                f"NGX final differs: {count} channels, absRGB={absolute}"
            )

        print(
            "PASS: XYO_HEADLESS=1 window hidden; RTX NGX, "
            "tiled Pen GPU 32/32 pre-NGX frames and final image "
            "byte-identical to visible mode",
            flush=True,
        )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError) as exc:
        print("FAIL:", exc, file=sys.stderr)
        sys.exit(1)
