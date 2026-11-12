#!/usr/bin/env python3
"""Opt-in NVIDIA NGX/GL A-B benchmark of CPU-quantized Pen scissoring.

Generates one real Scratch Pen .sb3 and measures renderer.draw() in
XYO_PLAY_PROFILE, omitting the first two frames. Each four-run block
uses ABBA order; this is not an isolated GPU timestamp benchmark.
"""
from __future__ import annotations

import argparse
import csv
import os
from pathlib import Path
import statistics
import subprocess
import tempfile

from dlss_pen_primitive_smoke import make_case


def measure(binary: Path, project: Path, profile: Path,
            scissor: bool, frames: int, tiled: bool = False,
            require_gpu: bool = True) -> float:
    env = os.environ.copy()
    env.update({
        "XYO_DLSS_REAL": "1",
        "XYO_DLSS_OPTICAL_FLOW": "1",
        "XYO_DLSS_TEMPORAL": "1",
        "XYO_DLSS_SCENE_CUT_SYNC": "1",
        "XYO_DLSS_RASTER_JITTER": "1",
        "XYO_DLSS_PEN_PRIMITIVE_REPLAY": "1",
        "XYO_DLSS_PEN_REPLAY_GPU": "1",
        "XYO_DLSS_PEN_REPLAY_PRECISE": "1",
        "XYO_DLSS_PEN_REPLAY_QUANTIZED": "1",
        "XYO_DLSS_PEN_REPLAY_SCISSOR": "1" if scissor else "0",
        "XYO_DLSS_PEN_REPLAY_TILED": "1" if tiled else "0",
        "XYO_DLSS_MODE": "performance",
        "XYO_DLSS_PRESET": "M",
        "XYO_RENDER_SCALE": "0.5",
        "XYO_PEN_FSR": "0",
        "XYO_EXIT_FRAMES": str(frames),
        "XYO_DLSS_REAL_DEBUG": "1",
        "XYO_PLAY_PROFILE": str(profile),
    })
    proc = subprocess.run(
        [str(binary), "play", str(project), "--scale", "2"],
        env=env, capture_output=True, text=True, timeout=90, check=False
    )
    if proc.returncode:
        raise RuntimeError(f"play returned {proc.returncode}: {proc.stderr[-2000:]}")
    if require_gpu and "GPU Pen coverage shader: FP64 CPU-quantized" not in proc.stderr:
        raise RuntimeError("CPU-quantized GPU shader was not selected")
    if tiled and "GPU Pen quantized tiled16 active" not in proc.stderr:
        raise RuntimeError("16x16 tile masks were not selected")
    with profile.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != frames:
        raise RuntimeError(f"profile expected {frames} frames; got {len(rows)}")
    return statistics.median(float(row["draw_ms"]) for row in rows[2:])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="NGX-enabled xyo executable")
    parser.add_argument("--blocks", type=int, default=2,
                        help="number of 4-run ABBA blocks")
    parser.add_argument("--frames", type=int, default=32)
    args = parser.parse_args()
    if args.blocks < 1 or args.frames < 4:
        parser.error("blocks must be positive and frames must be >= 4")
    times: dict[bool, list[float]] = {False: [], True: []}
    with tempfile.TemporaryDirectory(prefix="xyo-pen-scissor-bench-") as work:
        root = Path(work)
        project = root / "pen.sb3"
        make_case(project, "replay")
        for block in range(args.blocks):
            sequence = (False, True, True, False) if block % 2 == 0                 else (True, False, False, True)
            for slot, scissor in enumerate(sequence):
                profile = root / f"block-{block}-slot-{slot}.csv"
                value = measure(args.binary.resolve(), project, profile,
                                scissor, args.frames)
                times[scissor].append(value)
                print(f"block={block + 1} trial={slot + 1} "
                      f"scissor={int(scissor)} median_draw_ms={value:.4f}",
                      flush=True)
    off = statistics.mean(times[False])
    on = statistics.mean(times[True])
    print(f"mean trial median: OFF={off:.4f} ms ON={on:.4f} ms "
          f"reduction={(1.0 - on/off)*100:.2f}% "
          f"off/on={off/on:.3f}x")


if __name__ == "__main__":
    main()
