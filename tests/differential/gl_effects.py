#!/usr/bin/env python3
"""Compare CPU and OpenGL Scratch effect rendering without a desktop session.

Usage:
    python3 tests/differential/gl_effects.py build/xyo

Run on an SDL build with the offscreen+OpenGL renderer. A GPU is not required;
Mesa llvmpipe is sufficient. Does not mutate the source project.
"""
import argparse
import csv
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def read_ppm(path):
    with open(path, "rb") as file:
        assert file.readline().strip() == b"P6"
        w, h = map(int, file.readline().split())
        assert file.readline().strip() == b"255"
        pixels = file.read()
    assert len(pixels) == w * h * 3
    return (w, h), pixels


def run(binary, project, prefix, use_cpu, frames):
    env = os.environ.copy()
    env.update(
        SDL_VIDEODRIVER="offscreen",
        SDL_RENDER_DRIVER="opengl",
        SDL_AUDIODRIVER="dummy",
        XYO_EXIT_FRAMES=str(frames),
        XYO_PLAY_PROFILE=str(prefix) + ".csv",
        XYO_SHOT=str(prefix) + ".ppm",
        XYO_FX_DEBUG="1",
    )
    if use_cpu:
        env["XYO_FX_CPU"] = "1"
    else:
        env.pop("XYO_FX_CPU", None)
    result = subprocess.run(
        [str(binary), "play", str(project), "--jit=hotspot", "--fps", "30"],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, timeout=90, check=False,
    )
    if result.returncode:
        raise RuntimeError(
            f"xyo exited {result.returncode}\n{result.stderr[-4000:]}"
        )
    with open(str(prefix) + ".csv", newline="") as file:
        samples = list(csv.DictReader(file))
    return samples, result.stderr


def compare(cpu, gpu):
    (size_cpu, a), (size_gpu, b) = read_ppm(cpu), read_ppm(gpu)
    assert size_cpu == size_gpu
    max_diff = 0
    bad = 0
    total = size_cpu[0] * size_cpu[1]
    for index in range(0, len(a), 3):
        difference = max(abs(a[index + k] - b[index + k]) for k in range(3))
        max_diff = max(max_diff, difference)
        bad += difference > 5
    # Texture filtering on pixelated sprites differs at a few edge pixels.
    assert bad <= total * 0.003, (
        f"GPU rendering differs at {bad} / {total} pixels, max={max_diff}"
    )
    return max_diff, bad, total


def total(rows, field):
    return sum(int(row[field]) for row in rows)


def test(binary):
    here = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="xyo-glfx-") as temp:
        folder = Path(temp)
        demo = folder / "fx.sb3"
        subprocess.run(
            [sys.executable, str(here / "fixtures/gen_fx_demo.py"), str(demo)],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        cpu, _ = run(binary, demo, folder / "cpu", True, 4)
        gpu, stderr = run(binary, demo, folder / "gpu", False, 4)
        if "FX OpenGL" not in stderr:
            print("SKIP: an SDL OpenGL 3.3+ renderer is not available")
            return

        assert "FX OpenGL" in stderr and "enabled" in stderr
        assert total(cpu, "effect_builds") > 0
        assert total(gpu, "gpu_effect_passes") > 0
        assert total(gpu, "effect_builds") == 0
        max_diff, bad, total_pixels = compare(
            folder / "cpu.ppm", folder / "gpu.ppm"
        )
        print(
            "PASS: GPU effects",
            f"passes={total(gpu, 'gpu_effect_passes')}",
            f"CPU effect builds={total(cpu, 'effect_builds')}",
            f"pixels with difference >5={bad}/{total_pixels}",
            f"maximum channel difference={max_diff}",
        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    test(args.binary.resolve())
