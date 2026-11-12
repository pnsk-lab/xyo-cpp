#!/usr/bin/env python3
"""Measure the cost of post-NGX PPM frame capture in SDL/EGL offscreen mode."""
from __future__ import annotations

import argparse
import csv
import os
from pathlib import Path
import statistics
import subprocess
import tempfile


def measure(binary: Path, scene: Path, root: Path, trial: int,
            interval: int | None, frames: int) -> float:
    env = os.environ.copy()
    for key in ("DISPLAY", "WAYLAND_DISPLAY", "SDL_VIDEODRIVER",
                "EGL_PLATFORM", "XYO_SHOT", "XYO_SHOT_SERIES",
                "XYO_SHOT_EVERY"):
        env.pop(key, None)
    env.update({
        "XYO_HEADLESS": "offscreen",
        "XYO_DLSS_REAL": "1",
        "XYO_DLSS_OPTICAL_FLOW": "1",
        "XYO_DLSS_TEMPORAL": "1",
        "XYO_RENDER_SCALE": "0.5",
        "XYO_DLSS_REAL_DEBUG": "1",
        "XYO_EXIT_FRAMES": str(frames),
        "XYO_PLAY_PROFILE": str(root / f"trial-{trial}.csv"),
    })
    if interval is not None:
        env["XYO_SHOT_SERIES"] = str(root / f"frame-{trial}")
        env["XYO_SHOT_EVERY"] = str(interval)

    result = subprocess.run(
        [str(binary), "play", str(scene), "--scale", "2"],
        env=env, capture_output=True, text=True, timeout=120, check=False,
    )
    if result.returncode or "NVIDIA NGX initialized" not in result.stderr:
        raise RuntimeError(f"NGX trial {trial} failed:\n{result.stderr[-2000:]}")
    if "SDL video driver=offscreen" not in result.stderr:
        raise RuntimeError("SDL offscreen driver not selected")

    with (root / f"trial-{trial}.csv").open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != frames:
        raise RuntimeError(f"trial {trial}: expected {frames} rows, got {len(rows)}")
    actual = len(list(root.glob(f"frame-{trial}-*.ppm")))
    expected = 0 if interval is None else frames // interval
    if actual != expected:
        raise RuntimeError(f"trial {trial}: expected {expected} images, got {actual}")
    return statistics.median(float(row["draw_ms"]) for row in rows[2:])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="NGX-enabled xyo executable")
    parser.add_argument(
        "--project", type=Path,
        default=Path(__file__).resolve().parents[2] / "debug/projects/3D2.sb3",
    )
    parser.add_argument("--frames", type=int, default=40)
    parser.add_argument("--blocks", type=int, default=1)
    args = parser.parse_args()
    if args.frames < 4 or args.blocks < 1:
        parser.error("frames must be >= 4, blocks >= 1")

    sequence: tuple[tuple[str, int | None], ...] = (
        ("off", None), ("every8", 8), ("every1", 1),
        ("every1", 1), ("every8", 8), ("off", None),
    )
    samples: dict[str, list[float]] = {label: [] for label, _ in sequence}
    with tempfile.TemporaryDirectory(prefix="xyo-dlss-capture-bench-") as tmp:
        directory = Path(tmp)
        for block in range(args.blocks):
            for idx, (label, interval) in enumerate(sequence):
                trial = block * len(sequence) + idx
                value = measure(
                    args.binary.resolve(), args.project.resolve(),
                    directory, trial, interval, args.frames,
                )
                samples[label].append(value)
                print(f"block={block+1} trial={idx+1} mode={label} "
                      f"median_draw_ms={value:.4f}", flush=True)
    for label, times in samples.items():
        print(f"{label}: mean trial median draw_ms={statistics.mean(times):.4f}")


if __name__ == "__main__":
    main()
