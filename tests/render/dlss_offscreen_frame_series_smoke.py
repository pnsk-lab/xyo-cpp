#!/usr/bin/env python3
"""Full-resolution NGX frame captures must work with no display server."""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile
from dlss_pen_primitive_smoke import make_case


def play(binary: str, scene: Path, root: Path, name: str,
         interval: str, prefix: Path | None = None, count: int = 8,
         final_override: Path | None = None):
    prefix = prefix or root / name
    final = final_override or root / f"{name}-final.ppm"
    env = os.environ.copy()
    for key in ("DISPLAY", "WAYLAND_DISPLAY", "SDL_VIDEODRIVER", "EGL_PLATFORM"):
        env.pop(key, None)
    env.update({
        "XYO_HEADLESS": "offscreen",
        "XYO_DLSS_REAL": "1",
        "XYO_DLSS_OPTICAL_FLOW": "1",
        "XYO_DLSS_TEMPORAL": "1",
        "XYO_DLSS_REAL_DEBUG": "1",
        "XYO_RENDER_SCALE": "0.5",
        "XYO_EXIT_FRAMES": str(count),
        "XYO_SHOT": str(final),
        "XYO_SHOT_SERIES": str(prefix),
        "XYO_SHOT_EVERY": interval,
    })
    proc = subprocess.run(
        [binary, "play", str(scene), "--scale", "2"],
        env=env, capture_output=True, text=True, timeout=50, check=False,
    )
    return proc, final


def ppm(path: Path) -> bytes:
    contents = path.read_bytes()
    header = b"P6\n960 720\n255\n"
    if not contents.startswith(header) or len(contents) != len(header) + 960*720*3:
        raise RuntimeError(f"bad PPM: {path}")
    return contents


def main() -> None:
    if len(sys.argv) != 2:
        raise ValueError("usage: dlss_offscreen_frame_series_smoke.py <xyo>")
    with tempfile.TemporaryDirectory(prefix="xyo-offscreen-series-") as directory:
        root = Path(directory)
        scene = root / "pen.sb3"
        make_case(scene, "replay")
        first, last = play(sys.argv[1], scene, root, "all", "1")
        if first.returncode or "NVIDIA NGX initialized" not in first.stderr:
            raise RuntimeError(f"NGX unavailable: {first.stderr}")
        if "SDL video driver=offscreen" not in first.stderr:
            raise RuntimeError("offscreen driver was not selected")
        expected = [root / f"all-{i:06}.ppm" for i in range(1, 9)]
        if sorted(root.glob("all-??????.ppm")) != expected:
            raise RuntimeError("unexpected series filenames")
        frames = {i: ppm(path) for i, path in enumerate(expected, 1)}
        if frames[8] != ppm(last):
            raise RuntimeError("last screenshot differs from frame 8")

        second, last2 = play(sys.argv[1], scene, root, "every2", "2")
        if second.returncode or "NVIDIA NGX initialized" not in second.stderr:
            raise RuntimeError(f"stride-2 NGX failed: {second.stderr}")
        expected2 = [root / f"every2-{i:06}.ppm" for i in (2, 4, 6, 8)]
        if sorted(root.glob("every2-??????.ppm")) != expected2:
            raise RuntimeError("unexpected stride-2 filenames")
        for i, path in zip((2, 4, 6, 8), expected2):
            if frames[i] != ppm(path):
                raise RuntimeError(f"stride-2 mismatch frame {i}")
        if ppm(last2) != frames[8]:
            raise RuntimeError("stride-2 final image mismatch")

        invalid, _ = play(sys.argv[1], scene, root, "invalid", "0", count=2)
        if invalid.returncode != 2 or "XYO_SHOT_EVERY" not in invalid.stderr:
            raise RuntimeError("invalid stride accepted")

        failed, _ = play(sys.argv[1], scene, root, "bad-path", "1",
                         prefix=root / "no-parent" / "frame", count=2)
        if failed.returncode != 2 or "XYO_SHOT_SERIES: cannot save" not in failed.stderr:
            raise RuntimeError(f"save failure not propagated: {failed.stderr}")

        final_failed, _ = play(
            sys.argv[1], scene, root, "bad-final", "1", count=2,
            final_override=root / "missing-final-directory" / "last.ppm"
        )
        if (final_failed.returncode != 2 or
                "XYO_SHOT: cannot save" not in final_failed.stderr):
            raise RuntimeError(
                f"final screenshot failure not propagated: "
                f"{final_failed.stderr}"
            )

        print("PASS offscreen NGX 8-frame series, stride-2 4-frame series, "
              "final-frame byte parity, invalid stride, series/final IO errors")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
