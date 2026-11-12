#!/usr/bin/env python3
"""Verify optional FP64 GPU Pen reconstruction against real CPU/GPU input.

The test measures all 32 actual pre-NGX SDL frames, not one snapshot.
Requires real NVIDIA NGX and GL 4.0; returns 77 (CTest skip) if FP64
fragment shaders are unsupported on the test GPU.
"""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile

from dlss_pen_primitive_smoke import make_case


def run(binary: str, project: Path, root: Path, name: str,
        gpu: bool, precise: bool, quantized: bool = False,
        scissor: bool = True, tiled: bool = False,
        headless: bool | str = False) -> tuple[bytes, str]:
    env = os.environ.copy()
    env.update({
        "XYO_DLSS_REAL": "1",
        "XYO_DLSS_OPTICAL_FLOW": "1",
        "XYO_DLSS_TEMPORAL": "1",
        "XYO_DLSS_SCENE_CUT_SYNC": "1",
        "XYO_DLSS_RASTER_JITTER": "1",
        "XYO_DLSS_PEN_PRIMITIVE_REPLAY": "1",
        "XYO_DLSS_PEN_REPLAY_GPU": "1" if gpu else "0",
        "XYO_DLSS_PEN_REPLAY_PRECISE": "1" if precise else "0",
        "XYO_DLSS_PEN_REPLAY_QUANTIZED": "1" if quantized else "0",
        "XYO_DLSS_PEN_REPLAY_SCISSOR": "1" if scissor else "0",
        "XYO_DLSS_PEN_REPLAY_TILED": "1" if tiled else "0",
        "XYO_HEADLESS": ("offscreen" if headless == "offscreen"
                         else "1" if headless else "0"),
        "XYO_DLSS_REAL_DEBUG": "1",
        "XYO_DLSS_MODE": "performance",
        "XYO_DLSS_PRESET": "M",
        "XYO_RENDER_SCALE": "0.5",
        "XYO_PEN_FSR": "0",
        "XYO_EXIT_FRAMES": "32",
        "XYO_DLSS_RASTER_INPUT_SERIES": str(root / name),
        "XYO_SHOT": str(root / f"{name}-final.ppm"),
    })
    result = subprocess.run(
        [binary, "play", str(project), "--scale", "2"],
        env=env, capture_output=True, text=True, timeout=50,
        check=False,
    )
    output = result.stdout + result.stderr
    if result.returncode or "NVIDIA NGX initialized" not in output:
        raise RuntimeError(f"{name}: real NGX failed:\n{output}")
    return (root / f"{name}-final.ppm").read_bytes(), output


def read_input(root: Path, name: str, frame: int) -> bytes:
    image = (root / f"{name}-{frame}.ppm").read_bytes()
    header = b"P6\n480 360\n255\n"
    if not image.startswith(header) or len(image) != len(header)+480*360*3:
        raise RuntimeError(f"Bad pre-NGX input: {name} frame {frame}")
    return image[len(header):]


def error(a: bytes, b: bytes) -> tuple[int, int]:
    if len(a) != len(b):
        raise RuntimeError("CPU/GPU output shape differs")
    return sum(x != y for x, y in zip(a, b)), sum(abs(x-y) for x, y in zip(a,b))


def main() -> int:
    if len(sys.argv) != 2:
        raise ValueError("usage: dlss_pen_gpu_precision_smoke.py <xyo>")
    with tempfile.TemporaryDirectory(prefix="xyo-pen-precise-") as folder:
        root = Path(folder)
        project = root / "pen.sb3"
        make_case(project, "replay")
        cpu, _ = run(sys.argv[1], project, root, "cpu", False, False)
        gpu, regular_log = run(sys.argv[1], project, root, "gpu", True, False)
        precise, precise_log = run(sys.argv[1], project, root,
                                  "precise", True, True)
        if "GPU Pen coverage shader: regular interpolated" not in regular_log:
            raise RuntimeError("Normal GPU program was not selected")
        if "GPU Pen coverage shader: FP64 per-pixel" not in precise_log:
            print("SKIP: no GL 4.0 double precision GPU Pen shader")
            return 77
        totals = {"gpu": 0, "precise": 0}
        max_error = {"gpu": 0, "precise": 0}
        for frame in range(1, 33):
            reference = read_input(root, "cpu", frame)
            for mode in ("gpu", "precise"):
                test = read_input(root, mode, frame)
                _, delta = error(reference, test)
                totals[mode] += delta
                max_error[mode] = max(
                    max_error[mode],
                    max(abs(a-b) for a,b in zip(reference, test)),
                )
            if frame in (5, 8, 16, 24, 32):
                print(f"frame {frame}: max regular={max_error['gpu']}, "
                      f"max FP64={max_error['precise']}")
        print(f"32 input frames RGB absolute delta: "
              f"regular={totals['gpu']} precise={totals['precise']}")
        if totals["precise"] >= totals["gpu"] or max_error["precise"] > 3:
            raise RuntimeError("FP64 Pen did not improve input raster parity")
        old_final = error(cpu, gpu)
        new_final = error(cpu, precise)
        print(f"32-frame NGX output absolute difference from CPU: "
              f"regular={old_final[1]} precise={new_final[1]}")
        if new_final[1] >= old_final[1]:
            raise RuntimeError("FP64 Pen did not improve temporal output parity")
    print("PASS: FP64 GPU Pen reduces 32-frame input and final output error")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as e:
        print("FAIL:", e, file=sys.stderr)
        sys.exit(1)
