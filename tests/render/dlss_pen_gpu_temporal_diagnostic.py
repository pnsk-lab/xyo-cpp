#!/usr/bin/env python3
"""Diagnose CPU/GPU Pen raster differences before NVIDIA DLSS temporal input.

Reads every pre-NGX low-resolution frame; intentionally does not assert final
PSNR parity because NGX temporal reconstruction is sensitive to rounding.
"""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile

from dlss_pen_primitive_smoke import make_case


def run(binary: str, sb3: Path, root: Path, mode: str) -> bytes:
    env = os.environ.copy()
    env.update({
        "XYO_EXIT_FRAMES": "32",
        "XYO_RENDER_SCALE": "0.5",
        "XYO_DLSS_REAL": "1",
        "XYO_DLSS_OPTICAL_FLOW": "1",
        "XYO_DLSS_TEMPORAL": "1",
        "XYO_DLSS_SCENE_CUT_SYNC": "1",
        "XYO_DLSS_RASTER_JITTER": "1",
        "XYO_DLSS_PEN_PRIMITIVE_REPLAY": "1",
        "XYO_DLSS_PEN_REPLAY_GPU": "1" if mode == "gpu" else "0",
        "XYO_DLSS_PEN_REPLAY_SYNC": "finish",
        "XYO_DLSS_MODE": "performance",
        "XYO_DLSS_PRESET": "M",
        "XYO_PEN_FSR": "0",
        "XYO_DLSS_REAL_DEBUG": "1",
        "XYO_DLSS_RASTER_INPUT_SERIES": str(root / mode),
        "XYO_SHOT": str(root / f"{mode}-final.ppm"),
    })
    result = subprocess.run(
        [binary, "play", str(sb3), "--scale", "2"],
        env=env, capture_output=True, text=True, timeout=50,
        check=False,
    )
    output = result.stdout + result.stderr
    if result.returncode or "NVIDIA NGX initialized" not in output:
        raise RuntimeError(f"{mode} NGX playback failed:\n{output}")
    return (root / f"{mode}-final.ppm").read_bytes()


def ppm_rgb(file: Path) -> bytes:
    data = file.read_bytes()
    header = b"P6\n480 360\n255\n"
    if not data.startswith(header):
        raise RuntimeError(f"unexpected input raster format: {file}")
    pixels = data[len(header):]
    if len(pixels) != 480 * 360 * 3:
        raise RuntimeError(f"unexpected image byte length: {file}")
    return pixels


def main() -> None:
    if len(sys.argv) != 2:
        raise ValueError("usage: dlss_pen_gpu_temporal_diagnostic.py <xyo>")
    with tempfile.TemporaryDirectory(prefix="xyo-dlss-pen-temporal-") as d:
        root = Path(d)
        sb3 = root / "pen-lines.sb3"
        make_case(sb3, "replay")
        cpu_final = run(sys.argv[1], sb3, root, "cpu")
        gpu_final = run(sys.argv[1], sb3, root, "gpu")
        total_changed = 0
        largest_frame = 0
        identical_frames = 0
        max_delta = 0
        for frame in range(1, 33):
            cpu = ppm_rgb(root / f"cpu-{frame}.ppm")
            gpu = ppm_rgb(root / f"gpu-{frame}.ppm")
            diff = [abs(a-b) for a,b in zip(cpu,gpu)]
            channel_count = sum(x!=0 for x in diff)
            total_changed += channel_count
            if channel_count == 0:
                identical_frames += 1
            largest_frame = max(largest_frame, channel_count)
            max_delta = max(max_delta, max(diff))
            if frame in (1, 5, 8, 16, 24, 32):
                print(f"frame {frame:2}: differing RGB bytes={channel_count}, "
                      f"maximum channel delta={max(diff)}")
        final_diff = sum(a!=b for a,b in zip(cpu_final,gpu_final))
        print(f"32 frames: exact={identical_frames}, "
              f"total differing RGB bytes={total_changed}, "
              f"largest frame={largest_frame}, max delta={max_delta}")
        print(f"final native NGX output differing bytes={final_diff}")
        # A genuinely different renderer/texture would differ by much more
        # than a few levels per channel. Do not make deterministic numerical
        # parity an NVIDIA driver requirement.
        if max_delta > 8:
            raise RuntimeError(f"GPU input deviated from CPU by >8: {max_delta}")
    print("PASS: consecutive input captures are bounded and verifiable")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError, subprocess.TimeoutExpired) as e:
        print("FAIL:", e, file=sys.stderr)
        sys.exit(1)
